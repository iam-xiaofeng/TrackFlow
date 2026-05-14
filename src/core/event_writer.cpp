#include "core/event_writer.hpp"

#include <mysql/mysql.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace yolo_edge {

namespace {

// 把 std::optional<T> 渲染成 SQL 字面值 (T 是字符串则加引号; 否则数值/NULL)
std::string opt_str(const std::optional<std::string> &v, MYSQL *conn) {
  if (!v.has_value()) return "NULL";
  const auto &s = *v;
  std::string esc(s.size() * 2 + 1, '\0');
  unsigned long n = mysql_real_escape_string(conn, esc.data(), s.c_str(),
                                             static_cast<unsigned long>(s.size()));
  esc.resize(n);
  return "'" + esc + "'";
}

template <typename T>
std::string opt_num(const std::optional<T> &v) {
  if (!v.has_value()) return "NULL";
  return std::to_string(*v);
}

std::string opt_json(const std::optional<json> &v, MYSQL *conn) {
  if (!v.has_value()) return "NULL";
  std::string s = v->dump();
  std::string esc(s.size() * 2 + 1, '\0');
  unsigned long n = mysql_real_escape_string(conn, esc.data(), s.c_str(),
                                             static_cast<unsigned long>(s.size()));
  esc.resize(n);
  return "'" + esc + "'";
}

std::string esc_str(const std::string &s, MYSQL *conn) {
  std::string esc(s.size() * 2 + 1, '\0');
  unsigned long n = mysql_real_escape_string(conn, esc.data(), s.c_str(),
                                             static_cast<unsigned long>(s.size()));
  esc.resize(n);
  return "'" + esc + "'";
}

}  // namespace

EventWriter &EventWriter::instance() {
  static EventWriter inst;
  return inst;
}

EventWriter::EventWriter() = default;
EventWriter::~EventWriter() { stop(); }

void EventWriter::configure(const Config &cfg) {
  if (running_.load()) {
    fprintf(stderr, "[WARN] EventWriter::configure called while running, ignored\n");
    return;
  }
  cfg_ = cfg;
}

bool EventWriter::start() {
  if (running_.exchange(true)) {
    fprintf(stderr, "[WARN] EventWriter already running\n");
    return true;
  }
  worker_ = std::thread([this] { worker_loop(); });
  fprintf(stderr,
          "[INFO] EventWriter started (mysql=%s:%d db=%s queue_max=%zu)\n",
          cfg_.host.c_str(), cfg_.port, cfg_.db.c_str(), cfg_.max_queue_size);
  return true;
}

void EventWriter::stop() {
  if (!running_.exchange(false)) return;
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
  close_connection();
  Stats s = stats();
  fprintf(stderr,
          "[INFO] EventWriter stopped (enq=%lu wri=%lu fail=%lu drop=%lu rec=%lu)\n",
          static_cast<unsigned long>(s.enqueued),
          static_cast<unsigned long>(s.written),
          static_cast<unsigned long>(s.failed),
          static_cast<unsigned long>(s.dropped),
          static_cast<unsigned long>(s.reconnects));
}

void EventWriter::enqueue_event(EventVar ev) {
  if (!running_.load()) return;  // not started, silently drop
  {
    std::lock_guard<std::mutex> lk(mtx_);
    if (queue_.size() >= cfg_.max_queue_size) {
      drop_count_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    queue_.push(std::move(ev));
    enq_count_.fetch_add(1, std::memory_order_relaxed);
  }
  cv_.notify_one();
}

void EventWriter::enqueue_vehicle_event(VehicleEventRow row) {
  enqueue_event(std::move(row));
}

void EventWriter::enqueue_conflict_event(ConflictEventRow row) {
  enqueue_event(std::move(row));
}

void EventWriter::enqueue_track(TrackRow row) {
  enqueue_event(std::move(row));
}

void EventWriter::enqueue_flow_stat(FlowStatRow row) {
  enqueue_event(std::move(row));
}

void EventWriter::enqueue_queue_stat(QueueStatRow row) {
  enqueue_event(std::move(row));
}

EventWriter::Stats EventWriter::stats() const {
  Stats s;
  s.enqueued = enq_count_.load();
  s.written = wri_count_.load();
  s.failed = fail_count_.load();
  s.dropped = drop_count_.load();
  s.reconnects = reconnect_count_.load();
  s.connected = connected_.load();
  {
    std::lock_guard<std::mutex> lk(mtx_);
    s.queue_size_now = queue_.size();
  }
  return s;
}

bool EventWriter::ensure_connection() {
  if (conn_ && connected_.load()) return true;
  close_connection();

  MYSQL *m = mysql_init(nullptr);
  if (!m) {
    fprintf(stderr, "[ERROR] EventWriter: mysql_init failed\n");
    return false;
  }
  // 自动重连 + 字符集 + 超时
  bool reconnect = true;
  mysql_options(m, MYSQL_OPT_RECONNECT, &reconnect);
  unsigned int timeout = 5;
  mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");

  if (!mysql_real_connect(m, cfg_.host.c_str(), cfg_.user.c_str(),
                          cfg_.password.c_str(), cfg_.db.c_str(),
                          static_cast<unsigned int>(cfg_.port), nullptr, 0)) {
    fprintf(stderr, "[WARN] EventWriter connect failed: %s\n", mysql_error(m));
    mysql_close(m);
    return false;
  }
  conn_ = m;
  connected_.store(true);
  reconnect_count_.fetch_add(1, std::memory_order_relaxed);
  fprintf(stderr, "[INFO] EventWriter connected to mysql://%s@%s:%d/%s\n",
          cfg_.user.c_str(), cfg_.host.c_str(), cfg_.port, cfg_.db.c_str());
  return true;
}

void EventWriter::close_connection() {
  if (conn_) {
    mysql_close(static_cast<MYSQL *>(conn_));
    conn_ = nullptr;
  }
  connected_.store(false);
}

bool EventWriter::write_vehicle_event(const VehicleEventRow &r) {
  auto *m = static_cast<MYSQL *>(conn_);
  std::string sql =
      "INSERT INTO vehicle_events "
      "(intersection_id, track_id, event_type, start_time, end_time, "
      "entry_region, exit_region, entry_lane, movement, is_lane_violation, "
      "stop_duration, avg_speed, extra) VALUES (" +
      esc_str(r.intersection_id, m) + "," + std::to_string(r.track_id) + "," +
      esc_str(r.event_type, m) + "," + std::to_string(r.start_time) + "," +
      std::to_string(r.end_time) + "," + opt_str(r.entry_region, m) + "," +
      opt_str(r.exit_region, m) + "," + opt_str(r.entry_lane, m) + "," +
      opt_str(r.movement, m) + "," + (r.is_lane_violation ? "1" : "0") + "," +
      opt_num(r.stop_duration) + "," + opt_num(r.avg_speed) + "," +
      opt_json(r.extra, m) + ")";
  if (mysql_query(m, sql.c_str()) != 0) {
    fprintf(stderr, "[ERROR] write_vehicle_event: %s\n", mysql_error(m));
    return false;
  }
  return true;
}

bool EventWriter::write_conflict_event(const ConflictEventRow &r) {
  auto *m = static_cast<MYSQL *>(conn_);
  std::string sql =
      "INSERT INTO conflict_events "
      "(intersection_id, event_type, timestamp, track_a, track_b, risk_score, "
      "min_distance, pet, speed_a, speed_b, description, video_clip_path, extra) "
      "VALUES (" +
      esc_str(r.intersection_id, m) + "," + esc_str(r.event_type, m) + "," +
      std::to_string(r.timestamp) + "," + opt_num(r.track_a) + "," +
      opt_num(r.track_b) + "," + opt_num(r.risk_score) + "," +
      opt_num(r.min_distance) + "," + opt_num(r.pet) + "," +
      opt_num(r.speed_a) + "," + opt_num(r.speed_b) + "," +
      opt_str(r.description, m) + "," + opt_str(r.video_clip_path, m) + "," +
      opt_json(r.extra, m) + ")";
  if (mysql_query(m, sql.c_str()) != 0) {
    fprintf(stderr, "[ERROR] write_conflict_event: %s\n", mysql_error(m));
    return false;
  }
  return true;
}

bool EventWriter::write_track(const TrackRow &r) {
  auto *m = static_cast<MYSQL *>(conn_);
  std::string sql =
      "INSERT INTO tracks "
      "(intersection_id, camera_id, track_id, object_type, start_time, end_time, "
      "start_frame, end_frame, avg_speed, max_speed, entry_region, entry_lane, "
      "exit_region, exit_lane, movement, trajectory_json, extra) VALUES (" +
      esc_str(r.intersection_id, m) + "," + esc_str(r.camera_id, m) + "," +
      std::to_string(r.track_id) + "," + esc_str(r.object_type, m) + "," +
      std::to_string(r.start_time) + "," + std::to_string(r.end_time) + "," +
      opt_num(r.start_frame) + "," + opt_num(r.end_frame) + "," +
      opt_num(r.avg_speed) + "," + opt_num(r.max_speed) + "," +
      opt_str(r.entry_region, m) + "," + opt_str(r.entry_lane, m) + "," +
      opt_str(r.exit_region, m) + "," + opt_str(r.exit_lane, m) + "," +
      opt_str(r.movement, m) + "," + opt_json(r.trajectory_json, m) + "," +
      opt_json(r.extra, m) + ")";
  if (mysql_query(m, sql.c_str()) != 0) {
    fprintf(stderr, "[ERROR] write_track: %s\n", mysql_error(m));
    return false;
  }
  return true;
}

bool EventWriter::write_flow_stat(const FlowStatRow &r) {
  auto *m = static_cast<MYSQL *>(conn_);
  // 同 (intersection, bucket, approach, movement) UPSERT 累加
  std::string sql =
      "INSERT INTO flow_stats "
      "(intersection_id, time_bucket, approach, movement, vehicle_count, pedestrian_count) "
      "VALUES (" +
      esc_str(r.intersection_id, m) + "," + esc_str(r.time_bucket, m) + "," +
      esc_str(r.approach, m) + "," + esc_str(r.movement, m) + "," +
      std::to_string(r.vehicle_count) + "," +
      std::to_string(r.pedestrian_count) +
      ") ON DUPLICATE KEY UPDATE "
      "vehicle_count = vehicle_count + VALUES(vehicle_count), "
      "pedestrian_count = pedestrian_count + VALUES(pedestrian_count)";
  if (mysql_query(m, sql.c_str()) != 0) {
    fprintf(stderr, "[ERROR] write_flow_stat: %s\n", mysql_error(m));
    return false;
  }
  return true;
}

bool EventWriter::write_queue_stat(const QueueStatRow &r) {
  auto *m = static_cast<MYSQL *>(conn_);
  // queue_stats 没有 UNIQUE KEY, 每次 INSERT 一行覆盖同桶的多次累计——
  // 调用方应确保一个桶只 enqueue 一次 (在 bucket 切换时统一写)
  std::string sql =
      "INSERT INTO queue_stats "
      "(intersection_id, time_bucket, approach, lane_id, avg_queue_length, "
      "max_queue_length, avg_wait_time, queue_vehicle_count) VALUES (" +
      esc_str(r.intersection_id, m) + "," + esc_str(r.time_bucket, m) + "," +
      esc_str(r.approach, m) + "," + opt_str(r.lane_id, m) + "," +
      opt_num(r.avg_queue_length) + "," + opt_num(r.max_queue_length) + "," +
      opt_num(r.avg_wait_time) + "," + opt_num(r.queue_vehicle_count) + ")";
  if (mysql_query(m, sql.c_str()) != 0) {
    fprintf(stderr, "[ERROR] write_queue_stat: %s\n", mysql_error(m));
    return false;
  }
  return true;
}

void EventWriter::worker_loop() {
  using namespace std::chrono_literals;
  auto last_reconnect_attempt = std::chrono::steady_clock::now() - 1h;

  while (running_.load()) {
    // 取一条事件; 队列为空时阻塞等
    EventVar ev;
    {
      std::unique_lock<std::mutex> lk(mtx_);
      cv_.wait_for(lk, 500ms,
                   [this] { return !queue_.empty() || !running_.load(); });
      if (!running_.load() && queue_.empty()) break;
      if (queue_.empty()) continue;
      ev = std::move(queue_.front());
      queue_.pop();
    }

    // 写之前确保连接活着
    if (!ensure_connection()) {
      // 暂未连上 — 等下次循环重试, 把当前事件丢回队首
      {
        std::lock_guard<std::mutex> lk(mtx_);
        // 注意: queue 没有 push_front, 用 deque 不划算; 直接 push_back 让后续重试
        queue_.push(std::move(ev));
      }
      auto now = std::chrono::steady_clock::now();
      if (now - last_reconnect_attempt <
          std::chrono::milliseconds(cfg_.reconnect_interval_ms)) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(cfg_.reconnect_interval_ms));
      }
      last_reconnect_attempt = std::chrono::steady_clock::now();
      continue;
    }

    bool ok = false;
    std::visit(
        [&](auto &row) {
          using T = std::decay_t<decltype(row)>;
          if constexpr (std::is_same_v<T, VehicleEventRow>) {
            ok = write_vehicle_event(row);
          } else if constexpr (std::is_same_v<T, ConflictEventRow>) {
            ok = write_conflict_event(row);
          } else if constexpr (std::is_same_v<T, TrackRow>) {
            ok = write_track(row);
          } else if constexpr (std::is_same_v<T, FlowStatRow>) {
            ok = write_flow_stat(row);
          } else if constexpr (std::is_same_v<T, QueueStatRow>) {
            ok = write_queue_stat(row);
          }
        },
        ev);

    if (ok) {
      wri_count_.fetch_add(1, std::memory_order_relaxed);
    } else {
      fail_count_.fetch_add(1, std::memory_order_relaxed);
      // 连接可能挂了, 标记 disconnected 让下次 ensure 重连
      auto *m = static_cast<MYSQL *>(conn_);
      if (m && mysql_errno(m) >= 2000) {  // 客户端错误码段
        connected_.store(false);
      }
    }
  }

  // shutdown: 尝试 flush 剩余事件 (最多 5s)
  if (conn_ && connected_.load()) {
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      EventVar ev;
      {
        std::lock_guard<std::mutex> lk(mtx_);
        if (queue_.empty()) break;
        ev = std::move(queue_.front());
        queue_.pop();
      }
      std::visit(
          [&](auto &row) {
            using T = std::decay_t<decltype(row)>;
            if constexpr (std::is_same_v<T, VehicleEventRow>)
              write_vehicle_event(row);
            else if constexpr (std::is_same_v<T, ConflictEventRow>)
              write_conflict_event(row);
            else if constexpr (std::is_same_v<T, TrackRow>)
              write_track(row);
            else if constexpr (std::is_same_v<T, FlowStatRow>)
              write_flow_stat(row);
            else if constexpr (std::is_same_v<T, QueueStatRow>)
              write_queue_stat(row);
          },
          ev);
    }
  }
}

}  // namespace yolo_edge
