#pragma once

#include "core/image_processor.hpp"

#include <atomic>
#include <string>

namespace yolo_edge {

/**
 * TrafficAnalyzer — pipeline 末层, 把跟踪输出转成结构化交通事件并写库
 *
 * M1 阶段 (本文件): 骨架 + 注册 + 配置解析, 不输出真实事件
 * M2 阶段: ROI 加载 + 区域投票 + 转向/排队/流量事件
 * M3+:     冲突分析 / 车道违规 / 视频片段触发
 *
 * 与 GeoTransformer 的关系:
 *   - 若 detection 有 ground_x/ground_y, 使用米为单位计算速度、距离、PET
 *   - 若没有 (未配置 homography), 仅做像素空间 ROI 归属判断, 不输出速度类事件
 *
 * 与 EventWriter 的关系:
 *   - 本 processor 不直接 INSERT, 而是构造 EventRow 调 EventWriter::instance().enqueue_*
 *   - EventWriter 单例在 main.cpp 启动时初始化
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
  std::string intersection_id_ = "A001";
  std::string camera_id_ = "default";
  bool enabled_ = true;

  // 仅在 M1 用于一次性 log, 避免每帧刷屏
  std::atomic<bool> first_frame_logged_{false};
  std::atomic<uint64_t> frame_count_{0};
};

}  // namespace yolo_edge
