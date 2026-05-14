#include "processors/traffic_analyzer.hpp"

#include "core/event_writer.hpp"
#include "core/processor_factory.hpp"
#include "core/roi_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace yolo_edge {

namespace {
// 简单 OBB 底边中点近似为接地点 — 俯视镜头下比几何中心更靠近真实地面位置
cv::Point2d obb_ground_point(const Detection &det) {
  if (!det.geometry_ready || det.obb_points[0] == 0.0f) {
    return {static_cast<double>(det.obb.center.x),
            static_cast<double>(det.obb.center.y)};
  }
  // 取 OBB 四点 y 最大 (图像下方) 的两个点的中点
  std::array<cv::Point2d, 4> pts = {
      {{det.obb_points[0], det.obb_points[1]},
       {det.obb_points[2], det.obb_points[3]},
       {det.obb_points[4], det.obb_points[5]},
       {det.obb_points[6], det.obb_points[7]}}};
  std::sort(pts.begin(), pts.end(),
            [](const cv::Point2d &a, const cv::Point2d &b) { return a.y > b.y; });
  return {(pts[0].x + pts[1].x) * 0.5, (pts[0].y + pts[1].y) * 0.5};
}
}  // namespace

TrafficAnalyzer::TrafficAnalyzer() = default;
TrafficAnalyzer::~TrafficAnalyzer() { finalize_all(); }

void TrafficAnalyzer::configure(const json &config) {
  if (config.contains("intersection_id"))
    intersection_id_ = config["intersection_id"].get<std::string>();
  if (config.contains("camera_id"))
    camera_id_ = config["camera_id"].get<std::string>();
  if (config.contains("enabled"))
    enabled_ = config["enabled"].get<bool>();
  if (config.contains("region_history_size"))
    region_history_size_ = config["region_history_size"].get<int>();
  if (config.contains("finalize_grace_frames"))
    finalize_grace_frames_ = config["finalize_grace_frames"].get<int>();
  if (config.contains("low_speed_threshold_mps"))
    low_speed_threshold_mps_ = config["low_speed_threshold_mps"].get<double>();
  if (config.contains("low_speed_queue_frames"))
    low_speed_queue_frames_ = config["low_speed_queue_frames"].get<int>();

  fprintf(stderr,
          "[INFO] TrafficAnalyzer configured (intersection=%s camera=%s enabled=%d "
          "grace=%d low_speed=%.2fm/s rois=%zu)\n",
          intersection_id_.c_str(), camera_id_.c_str(),
          static_cast<int>(enabled_), finalize_grace_frames_,
          low_speed_threshold_mps_, RoiRegistry::instance().roi_count());
}

double TrafficAnalyzer::now_epoch_seconds() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

std::string TrafficAnalyzer::bucket_string(double epoch_sec) {
  // 5min 向下取整, 格式 "YYYY-MM-DD HH:MM:00" (本地时间)
  std::time_t t = static_cast<std::time_t>(epoch_sec);
  std::tm tm{};
#if defined(__unix__) || defined(__APPLE__)
  localtime_r(&t, &tm);
#else
  tm = *std::localtime(&t);
#endif
  tm.tm_min = (tm.tm_min / 5) * 5;
  tm.tm_sec = 0;
  std::ostringstream os;
  os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return os.str();
}

std::string TrafficAnalyzer::vote_region(
    const std::deque<std::string> &history) const {
  if (history.empty()) return "";
  std::unordered_map<std::string, int> counts;
  for (const auto &r : history) {
    if (r.empty()) continue;
    counts[r]++;
  }
  if (counts.empty()) return "";
  auto best = std::max_element(
      counts.begin(), counts.end(),
      [](const auto &a, const auto &b) { return a.second < b.second; });
  // 至少占比 entry_lock_min_ratio_
  if (best->second < entry_lock_min_frames_) return "";
  double ratio = static_cast<double>(best->second) / history.size();
  if (ratio < entry_lock_min_ratio_) return "";
  return best->first;
}

void TrafficAnalyzer::update_track_state(TrackState &st,
                                         const std::string &region_id,
                                         const std::string &lane_id,
                                         double now, int frame_id,
                                         double speed_mps,
                                         const cv::Point2d &pixel) {
  st.last_seen_time = now;
  st.last_frame = frame_id;
  st.region_history.push_back(region_id);
  st.lane_history.push_back(lane_id);
  while (static_cast<int>(st.region_history.size()) > region_history_size_)
    st.region_history.pop_front();
  while (static_cast<int>(st.lane_history.size()) > region_history_size_)
    st.lane_history.pop_front();
  st.current_region = region_id;
  st.current_lane = lane_id;

  if (!st.entry_locked) {
    auto vr = vote_region(st.region_history);
    if (!vr.empty() && (vr.find("_in") != std::string::npos ||
                        vr.find("approach") != std::string::npos)) {
      st.entry_region = vr;
      auto vl = vote_region(st.lane_history);
      st.entry_lane = vl;
      st.entry_locked = true;
    }
  }

  if (speed_mps >= 0) {
    st.total_speed += speed_mps;
    st.speed_samples += 1;
    if (speed_mps > st.max_speed) st.max_speed = speed_mps;
    if (speed_mps < low_speed_threshold_mps_) {
      st.low_speed_frames += 1;
    } else {
      st.low_speed_frames = 0;
    }
  }

  if (st.trajectory_samples.empty() || (frame_id - st.start_frame) % 10 == 0) {
    st.trajectory_samples.push_back(pixel);
    if (st.trajectory_samples.size() > 256) {
      // 上限保护, 之后丢弃中间样本
      st.trajectory_samples.erase(st.trajectory_samples.begin() +
                                  st.trajectory_samples.size() / 2);
    }
  }
}

void TrafficAnalyzer::finalize_track(int track_id) {
  auto it = tracks_.find(track_id);
  if (it == tracks_.end()) return;
  auto &st = it->second;

  // exit_region: 取 history 中最后一个出口类型的 region
  std::string exit_region;
  for (auto rit = st.region_history.rbegin(); rit != st.region_history.rend(); ++rit) {
    if (rit->find("_out") != std::string::npos ||
        rit->find("exit") != std::string::npos) {
      exit_region = *rit;
      break;
    }
  }
  if (exit_region.empty() && !st.current_region.empty()) exit_region = st.current_region;

  std::string movement;
  bool is_violation = false;
  if (!st.entry_region.empty() && !exit_region.empty()) {
    movement = RoiRegistry::instance().lookup_movement(
        intersection_id_, st.entry_region, exit_region);
    if (!st.entry_lane.empty()) {
      auto allowed = RoiRegistry::instance().allowed_movements(
          intersection_id_, st.entry_lane);
      if (!allowed.empty() &&
          std::find(allowed.begin(), allowed.end(), movement) == allowed.end()) {
        is_violation = true;
      }
    }
  }

  float avg_speed = st.speed_samples > 0
                        ? static_cast<float>(st.total_speed / st.speed_samples)
                        : -1.0f;

  // 累积到 flow_buckets_ (用 entry_region 作为 approach 维度)
  if (!st.entry_region.empty() && !movement.empty() && movement != "other") {
    std::string bucket = bucket_string(st.start_time);
    BucketKey k{bucket, st.entry_region, movement};
    flow_buckets_[k] += 1;
  }

  // 写 vehicle_events
  VehicleEventRow vrow;
  vrow.intersection_id = intersection_id_;
  vrow.track_id = track_id;
  vrow.event_type = "movement";
  vrow.start_time = st.start_time;
  vrow.end_time = st.last_seen_time;
  if (!st.entry_region.empty()) vrow.entry_region = st.entry_region;
  if (!exit_region.empty()) vrow.exit_region = exit_region;
  if (!st.entry_lane.empty()) vrow.entry_lane = st.entry_lane;
  if (!movement.empty()) vrow.movement = movement;
  vrow.is_lane_violation = is_violation;
  if (st.low_speed_frames > low_speed_queue_frames_) {
    // 简化: 用低速帧数 / 30 ≈ 停车秒数
    vrow.stop_duration = static_cast<float>(st.low_speed_frames) / 30.0f;
  }
  if (st.speed_samples > 0) vrow.avg_speed = avg_speed;
  EventWriter::instance().enqueue_vehicle_event(std::move(vrow));

  // 写 tracks
  TrackRow trow;
  trow.intersection_id = intersection_id_;
  trow.camera_id = camera_id_;
  trow.track_id = track_id;
  trow.object_type = st.object_type;
  trow.start_time = st.start_time;
  trow.end_time = st.last_seen_time;
  trow.start_frame = st.start_frame;
  trow.end_frame = st.last_frame;
  if (st.speed_samples > 0) {
    trow.avg_speed = avg_speed;
    trow.max_speed = static_cast<float>(st.max_speed);
  }
  if (!st.entry_region.empty()) trow.entry_region = st.entry_region;
  if (!st.entry_lane.empty()) trow.entry_lane = st.entry_lane;
  if (!exit_region.empty()) trow.exit_region = exit_region;
  if (!movement.empty()) trow.movement = movement;
  if (!st.trajectory_samples.empty()) {
    json arr = json::array();
    for (const auto &p : st.trajectory_samples) {
      arr.push_back({p.x, p.y});
    }
    trow.trajectory_json = std::move(arr);
  }
  EventWriter::instance().enqueue_track(std::move(trow));

  events_emitted_.fetch_add(1, std::memory_order_relaxed);
  tracks_.erase(it);
}

void TrafficAnalyzer::finalize_all() {
  std::vector<int> ids;
  ids.reserve(tracks_.size());
  for (const auto &[id, _] : tracks_) ids.push_back(id);
  for (int id : ids) finalize_track(id);

  // 把所有未刷的 flow_buckets_ / queue_buckets_ 全部写出去
  for (const auto &[k, cnt] : flow_buckets_) {
    FlowStatRow row;
    row.intersection_id = intersection_id_;
    row.time_bucket = std::get<0>(k);
    row.approach = std::get<1>(k);
    row.movement = std::get<2>(k);
    row.vehicle_count = cnt;
    EventWriter::instance().enqueue_flow_stat(std::move(row));
  }
  flow_buckets_.clear();

  for (const auto &[k, agg] : queue_buckets_) {
    QueueStatRow row;
    row.intersection_id = intersection_id_;
    row.time_bucket = k.bucket_str;
    row.approach = k.approach;
    if (!k.lane_id.empty()) row.lane_id = k.lane_id;
    row.max_queue_length = static_cast<float>(agg.max_simultaneous);
    int n_tracks = static_cast<int>(agg.track_first_queued.size());
    if (n_tracks > 0) {
      row.queue_vehicle_count = n_tracks;
      double total_wait = 0;
      for (const auto &[_, w] : agg.track_total_wait) total_wait += w;
      row.avg_wait_time = static_cast<float>(total_wait / n_tracks);
    }
    EventWriter::instance().enqueue_queue_stat(std::move(row));
  }
  queue_buckets_.clear();
}

void TrafficAnalyzer::flush_completed_buckets(double now) {
  std::string cur = bucket_string(now);
  if (current_bucket_str_.empty()) {
    current_bucket_str_ = cur;
    return;
  }
  if (cur == current_bucket_str_) return;

  // 桶切换: 把所有"非当前桶"的累计 enqueue 出去
  for (auto it = flow_buckets_.begin(); it != flow_buckets_.end();) {
    if (std::get<0>(it->first) == cur) {
      ++it;
      continue;
    }
    FlowStatRow row;
    row.intersection_id = intersection_id_;
    row.time_bucket = std::get<0>(it->first);
    row.approach = std::get<1>(it->first);
    row.movement = std::get<2>(it->first);
    row.vehicle_count = it->second;
    EventWriter::instance().enqueue_flow_stat(std::move(row));
    it = flow_buckets_.erase(it);
  }
  for (auto it = queue_buckets_.begin(); it != queue_buckets_.end();) {
    if (it->first.bucket_str == cur) {
      ++it;
      continue;
    }
    QueueStatRow row;
    row.intersection_id = intersection_id_;
    row.time_bucket = it->first.bucket_str;
    row.approach = it->first.approach;
    if (!it->first.lane_id.empty()) row.lane_id = it->first.lane_id;
    row.max_queue_length = static_cast<float>(it->second.max_simultaneous);
    int n_tracks = static_cast<int>(it->second.track_first_queued.size());
    if (n_tracks > 0) {
      row.queue_vehicle_count = n_tracks;
      double total_wait = 0;
      for (const auto &[_, w] : it->second.track_total_wait) total_wait += w;
      row.avg_wait_time = static_cast<float>(total_wait / n_tracks);
    }
    EventWriter::instance().enqueue_queue_stat(std::move(row));
    it = queue_buckets_.erase(it);
  }
  current_bucket_str_ = cur;
}

bool TrafficAnalyzer::process(ProcessingContext &ctx) {
  if (!enabled_) return true;

  double now = now_epoch_seconds();
  if (current_bucket_str_.empty()) current_bucket_str_ = bucket_string(now);

  if (!first_frame_logged_.exchange(true)) {
    fprintf(stderr,
            "[INFO] TrafficAnalyzer first frame: frame_id=%d detections=%zu "
            "rois=%zu intersections=%zu\n",
            ctx.frame_id, ctx.detections.size(),
            RoiRegistry::instance().roi_count(),
            RoiRegistry::instance().known_intersections().size());
  }
  frame_count_.fetch_add(1, std::memory_order_relaxed);

  // 1) 处理本帧所有 detection
  std::unordered_set<int> seen_ids;
  for (const auto &det : ctx.detections) {
    if (det.track_id < 0) continue;
    seen_ids.insert(det.track_id);

    cv::Point2d gp = obb_ground_point(det);
    auto loc = RoiRegistry::instance().locate(intersection_id_, gp.x, gp.y);

    auto &st = tracks_[det.track_id];
    if (st.track_id == 0 && st.start_frame == 0) {
      st.track_id = det.track_id;
      st.object_type = det.class_name;
      st.start_time = now;
      st.start_frame = ctx.frame_id;
    }

    // 速度: 优先用地面坐标差分 (要求 GeoTransformer 已经填了 ground_x/y)
    double speed_mps = -1.0;
    if (det.ground_x.has_value() && det.ground_y.has_value() &&
        st.speed_samples >= 0 && !st.trajectory_samples.empty()) {
      // 简化: 拿不到上一帧的 ground 直接 store, 这里跳过精确速度
      // M3 再精细化 (需要保存上一帧 ground + dt)
    }

    update_track_state(st, loc.region_id, loc.lane_id, now, ctx.frame_id,
                       speed_mps, gp);
  }

  // 2) finalize 失踪 track
  std::vector<int> to_finalize;
  for (auto &[id, st] : tracks_) {
    if (seen_ids.count(id)) continue;
    if (ctx.frame_id - st.last_frame > finalize_grace_frames_) {
      to_finalize.push_back(id);
    }
  }
  for (int id : to_finalize) finalize_track(id);

  // 3) 排队统计: 本帧"正在排队"的 track 按 (approach, lane) 分组, 更新 max_simultaneous
  std::string cur_bucket = bucket_string(now);
  std::unordered_map<QueueBucketKey, int, QueueBucketKeyHash> cur_frame_count;
  for (auto &[id, st] : tracks_) {
    if (!seen_ids.count(id)) continue;
    if (st.low_speed_frames < low_speed_queue_frames_) continue;
    if (st.current_region.find("_in") == std::string::npos &&
        st.current_region.find("approach") == std::string::npos)
      continue;
    QueueBucketKey k{cur_bucket, st.current_region, st.current_lane};
    cur_frame_count[k] += 1;
    auto &agg = queue_buckets_[k];
    if (!agg.track_first_queued.count(id)) {
      agg.track_first_queued[id] = now;
    }
    agg.track_total_wait[id] =
        now - agg.track_first_queued[id];  // 累计低速秒数 (近似)
  }
  for (auto &[k, cnt] : cur_frame_count) {
    if (cnt > queue_buckets_[k].max_simultaneous) {
      queue_buckets_[k].max_simultaneous = cnt;
    }
  }

  // 4) 5min 桶滚动 → flush
  flush_completed_buckets(now);

  ctx.set<std::string>("traffic_analyzer", "m2");
  return true;
}

REGISTER_PROCESSOR("traffic_analyzer", TrafficAnalyzer);

}  // namespace yolo_edge
