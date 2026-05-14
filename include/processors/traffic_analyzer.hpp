#pragma once

#include "core/image_processor.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <opencv2/core.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace yolo_edge {

/**
 * TrafficAnalyzer — pipeline 末层, 把跟踪输出转成结构化交通事件并写库
 *
 * M2 实现:
 *   - 加载 RoiRegistry (启动时从 MySQL 读 roi_configs)
 *   - per-track 状态机: 区域投票锁定入口 + 实时记录当前区域
 *   - track 失踪 N 帧 -> 生成 movement 事件 + tracks 行, 异步写库
 *   - 内存累计 5min 桶: 流量 + 简化版排队 -> flow_stats / queue_stats
 *
 * 时间:
 *   start_time / end_time 用墙钟 (UTC epoch 秒, 双精度)
 *   time_bucket 用 floor 到 5 分钟的本地时间 (与 sql time_zone='+08:00' 对齐)
 */
class TrafficAnalyzer : public ImageProcessor {
public:
  TrafficAnalyzer();
  ~TrafficAnalyzer() override;

  bool process(ProcessingContext &ctx) override;
  std::string name() const override { return "TrafficAnalyzer"; }
  void configure(const json &config) override;
  bool is_stateful() const override { return true; }

private:
  struct TrackState {
    int track_id = 0;
    std::string object_type;
    double start_time = 0;       // epoch 秒
    double last_seen_time = 0;
    int start_frame = 0;
    int last_frame = 0;
    std::string entry_region;    // 锁定后不变
    std::string entry_lane;
    std::string current_region;
    std::string current_lane;
    std::deque<std::string> region_history;  // 最近 N 帧
    std::deque<std::string> lane_history;
    bool entry_locked = false;
    std::vector<cv::Point2d> trajectory_samples;  // 稀疏采样: 每 ~10 帧存一次
    double total_speed = 0;
    int speed_samples = 0;
    double max_speed = 0;
    int low_speed_frames = 0;    // 连续低速帧数, 用于排队判定
    // 累计在每个 5min 桶中是否被记为排队
    double last_stationary_check_time = 0;
  };

  using BucketKey = std::tuple<std::string /*bucket_str*/,
                               std::string /*approach*/,
                               std::string /*movement*/>;
  struct BucketKeyHash {
    size_t operator()(const BucketKey &k) const noexcept {
      auto h = std::hash<std::string>{};
      return h(std::get<0>(k)) ^ (h(std::get<1>(k)) << 1) ^ (h(std::get<2>(k)) << 2);
    }
  };

  struct QueueBucketKey {
    std::string bucket_str;
    std::string approach;
    std::string lane_id;
    bool operator==(const QueueBucketKey &o) const {
      return bucket_str == o.bucket_str && approach == o.approach && lane_id == o.lane_id;
    }
  };
  struct QueueBucketKeyHash {
    size_t operator()(const QueueBucketKey &k) const noexcept {
      auto h = std::hash<std::string>{};
      return h(k.bucket_str) ^ (h(k.approach) << 1) ^ (h(k.lane_id) << 2);
    }
  };
  struct QueueAgg {
    int max_simultaneous = 0;      // 该桶内同一时刻最大排队数 (车辆数, 非米)
    std::unordered_map<int, double> track_first_queued;  // track_id -> first time it started queueing
    std::unordered_map<int, double> track_total_wait;    // track_id -> 累计低速秒数
  };

  // M2 主流程
  void update_track_state(TrackState &st, const std::string &region_id,
                          const std::string &lane_id, double now,
                          int frame_id, double speed_mps,
                          const cv::Point2d &pixel);
  void finalize_track(int track_id);
  void finalize_all();
  void flush_completed_buckets(double now);

  // 工具
  static std::string bucket_string(double epoch_sec);
  static double now_epoch_seconds();
  std::string vote_region(const std::deque<std::string> &history) const;

  // 配置
  std::string intersection_id_ = "A001";
  std::string camera_id_ = "default";
  bool enabled_ = true;
  int region_history_size_ = 30;
  int entry_lock_min_frames_ = 5;
  double entry_lock_min_ratio_ = 0.5;
  int finalize_grace_frames_ = 60;
  double low_speed_threshold_mps_ = 0.5;
  int low_speed_queue_frames_ = 30;   // ~1s @30fps 进入排队

  // 状态
  std::unordered_map<int, TrackState> tracks_;
  std::unordered_map<BucketKey, int, BucketKeyHash> flow_buckets_;       // count
  std::unordered_map<QueueBucketKey, QueueAgg, QueueBucketKeyHash> queue_buckets_;
  std::string current_bucket_str_;  // 当前 5min 桶字符串

  std::atomic<bool> first_frame_logged_{false};
  std::atomic<uint64_t> frame_count_{0};
  std::atomic<uint64_t> events_emitted_{0};
};

}  // namespace yolo_edge
