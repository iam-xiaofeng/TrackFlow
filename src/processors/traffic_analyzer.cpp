#include "processors/traffic_analyzer.hpp"

#include "core/event_writer.hpp"
#include "core/processor_factory.hpp"

#include <cstdio>

namespace yolo_edge {

TrafficAnalyzer::TrafficAnalyzer() = default;
TrafficAnalyzer::~TrafficAnalyzer() = default;

void TrafficAnalyzer::configure(const json &config) {
  if (config.contains("intersection_id")) {
    intersection_id_ = config["intersection_id"].get<std::string>();
  }
  if (config.contains("camera_id")) {
    camera_id_ = config["camera_id"].get<std::string>();
  }
  if (config.contains("enabled")) {
    enabled_ = config["enabled"].get<bool>();
  }
  fprintf(stderr,
          "[INFO] TrafficAnalyzer configured (intersection=%s camera=%s enabled=%d)\n",
          intersection_id_.c_str(), camera_id_.c_str(),
          static_cast<int>(enabled_));
}

bool TrafficAnalyzer::process(ProcessingContext &ctx) {
  if (!enabled_) return true;

  // M1: 不做实际事件提取, 只验证 pipeline 能挂上 + EventWriter 可调用
  if (!first_frame_logged_.exchange(true)) {
    fprintf(stderr,
            "[INFO] TrafficAnalyzer first frame: frame_id=%d detections=%zu "
            "writer_running=%d\n",
            ctx.frame_id, ctx.detections.size(),
            static_cast<int>(EventWriter::instance().is_running()));
  }
  frame_count_.fetch_add(1, std::memory_order_relaxed);

  // 给下游 / 调试用, 标记本帧由 TrafficAnalyzer 处理过
  ctx.set<std::string>("traffic_analyzer", "m1_stub");
  return true;
}

REGISTER_PROCESSOR("traffic_analyzer", TrafficAnalyzer);

}  // namespace yolo_edge
