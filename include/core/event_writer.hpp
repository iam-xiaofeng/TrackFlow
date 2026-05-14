#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <variant>

namespace yolo_edge {

using json = nlohmann::json;

// 与 sql/001_init.sql 的 vehicle_events 表对应
struct VehicleEventRow {
  std::string intersection_id;
  int track_id = 0;
  std::string event_type;  // "movement" / "queue" / "stop" / "violation"
  double start_time = 0.0;
  double end_time = 0.0;
  std::optional<std::string> entry_region;
  std::optional<std::string> exit_region;
  std::optional<std::string> entry_lane;
  std::optional<std::string> movement;
  bool is_lane_violation = false;
  std::optional<float> stop_duration;
  std::optional<float> avg_speed;
  std::optional<json> extra;
};

// 与 conflict_events 表对应
struct ConflictEventRow {
  std::string intersection_id;
  std::string event_type;  // "vehicle_vehicle" / "vehicle_pedestrian"
  double timestamp = 0.0;
  std::optional<int> track_a;
  std::optional<int> track_b;
  std::optional<float> risk_score;
  std::optional<float> min_distance;
  std::optional<float> pet;
  std::optional<float> speed_a;
  std::optional<float> speed_b;
  std::optional<std::string> description;
  std::optional<std::string> video_clip_path;
  std::optional<json> extra;
};

// 与 tracks 表对应
struct TrackRow {
  std::string intersection_id;
  std::string camera_id = "default";
  int track_id = 0;
  std::string object_type;
  double start_time = 0.0;
  double end_time = 0.0;
  std::optional<int> start_frame;
  std::optional<int> end_frame;
  std::optional<float> avg_speed;
  std::optional<float> max_speed;
  std::optional<std::string> entry_region;
  std::optional<std::string> entry_lane;
  std::optional<std::string> exit_region;
  std::optional<std::string> exit_lane;
  std::optional<std::string> movement;
  std::optional<json> trajectory_json;
  std::optional<json> extra;
};

// flow_stats: 5min 桶聚合, 用 INSERT ... ON DUPLICATE KEY UPDATE 累加
struct FlowStatRow {
  std::string intersection_id;
  std::string time_bucket;  // "YYYY-MM-DD HH:MM:00", 已 floor 到 5min
  std::string approach;
  std::string movement;
  int vehicle_count = 0;
  int pedestrian_count = 0;
};

// queue_stats: 5min 桶聚合
struct QueueStatRow {
  std::string intersection_id;
  std::string time_bucket;
  std::string approach;
  std::optional<std::string> lane_id;
  std::optional<float> avg_queue_length;
  std::optional<float> max_queue_length;
  std::optional<float> avg_wait_time;
  std::optional<int> queue_vehicle_count;
};

/**
 * 异步事件写入器 (单例)
 *
 * 设计要点:
 *   - C++ 推理线程只调 enqueue_*, 写库走单独 worker 线程, 不阻塞主管线
 *   - 队列上限 max_queue_size, 满了之后新事件被丢弃并计数, 不会无限增长
 *   - MySQL 断线自动重连, 重连期间事件仍入队
 *   - stop() 会尝试 flush 最多 5s, 之后强制退出
 *
 * 用法:
 *   auto& w = EventWriter::instance();
 *   w.configure({...});
 *   w.start();
 *   w.enqueue_vehicle_event({...});
 *   ...
 *   w.stop();
 */
class EventWriter {
public:
  struct Config {
    std::string host = "127.0.0.1";
    int port = 3306;
    std::string user = "trackflow";
    std::string password;
    std::string db = "trackflow";
    size_t max_queue_size = 10000;
    int reconnect_interval_ms = 2000;
  };

  struct Stats {
    uint64_t enqueued = 0;
    uint64_t written = 0;
    uint64_t failed = 0;
    uint64_t dropped = 0;  // 队列满时丢弃数
    uint64_t reconnects = 0;
    size_t queue_size_now = 0;
    bool connected = false;
  };

  static EventWriter &instance();

  // 不允许复制/移动
  EventWriter(const EventWriter &) = delete;
  EventWriter &operator=(const EventWriter &) = delete;

  void configure(const Config &cfg);
  bool start();  // true if worker thread started; 连接失败也会启动, worker 会重试
  void stop();
  bool is_running() const { return running_.load(); }

  // 入队接口 — 线程安全, 非阻塞 (除了取队列锁的极短时间)
  void enqueue_vehicle_event(VehicleEventRow row);
  void enqueue_conflict_event(ConflictEventRow row);
  void enqueue_track(TrackRow row);
  void enqueue_flow_stat(FlowStatRow row);
  void enqueue_queue_stat(QueueStatRow row);

  Stats stats() const;

private:
  EventWriter();
  ~EventWriter();

  void worker_loop();
  bool ensure_connection();
  void close_connection();

  bool write_vehicle_event(const VehicleEventRow &r);
  bool write_conflict_event(const ConflictEventRow &r);
  bool write_track(const TrackRow &r);
  bool write_flow_stat(const FlowStatRow &r);
  bool write_queue_stat(const QueueStatRow &r);

  using EventVar = std::variant<VehicleEventRow, ConflictEventRow, TrackRow,
                                FlowStatRow, QueueStatRow>;
  void enqueue_event(EventVar ev);

  Config cfg_;

  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::thread worker_;

  mutable std::mutex mtx_;
  std::condition_variable cv_;
  std::queue<EventVar> queue_;

  // worker 私有, 不需要锁
  void *conn_ = nullptr;  // 实际是 MYSQL*, 用 void* 避免头文件污染

  // 统计 — 简单原子计数
  mutable std::atomic<uint64_t> enq_count_{0};
  mutable std::atomic<uint64_t> wri_count_{0};
  mutable std::atomic<uint64_t> fail_count_{0};
  mutable std::atomic<uint64_t> drop_count_{0};
  mutable std::atomic<uint64_t> reconnect_count_{0};
};

}  // namespace yolo_edge
