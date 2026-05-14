#pragma once

#include <opencv2/core.hpp>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yolo_edge {

// 一条 ROI 配置 (对应 sql 表 roi_configs 一行)
struct RoiSpec {
  std::string id;
  std::string intersection_id;
  std::string camera_id;
  std::string type;       // approach / exit / lane / crosswalk / center / stop_line / conflict
  std::string parent_id;  // 车道的进口/出口 ROI id
  std::vector<cv::Point2d> polygon;
  std::vector<std::string> allowed_movements;  // ["straight","left","right","u_turn"]
  cv::Point2d centroid;
};

// 点的归属结果 (优先级: 车道最具体)
struct PointLocation {
  std::string lane_id;       // "" if not in any lane
  std::string region_id;     // approach / exit id; "" if not in any
  std::string in_crosswalk;  // crosswalk id; "" otherwise
  bool in_center = false;
};

/**
 * RoiRegistry
 *   - 启动时从 MySQL 加载所有路口 ROI
 *   - 内存常驻, 推理热路径不查库
 *   - 自动从几何推导 movement_rules (不需要用户写规则)
 *
 * 线程安全: 多读偶尔重载, 用 shared_mutex
 */
class RoiRegistry {
public:
  struct DbConfig {
    std::string host = "127.0.0.1";
    int port = 3306;
    std::string user;
    std::string password;
    std::string db;
  };

  static RoiRegistry &instance();

  // 一次性全量加载. 重复调用相当于刷新.
  // 返回成功加载的 ROI 数量; -1 表示连库失败
  int reload_from_mysql(const DbConfig &cfg);

  // 仅做单元测试用的注入接口
  void load_from_specs(std::vector<RoiSpec> specs);

  // 查询: 点 (x, y) 在该路口落在哪些 ROI 内
  PointLocation locate(const std::string &intersection_id, double x,
                       double y) const;

  // 转向查询. 找不到返回 "other".
  std::string lookup_movement(const std::string &intersection_id,
                              const std::string &entry_region_id,
                              const std::string &exit_region_id) const;

  // 车道允许的转向 (空 vector 表示未设置, 视为全允许)
  std::vector<std::string> allowed_movements(
      const std::string &intersection_id, const std::string &lane_id) const;

  // 路口中心 (如果有 center 类型 ROI, 取其 centroid; 否则取所有进口出口的 centroid 平均)
  std::optional<cv::Point2d> intersection_center(
      const std::string &intersection_id) const;

  // 统计 (调试 / Agent 工具用)
  size_t roi_count() const;
  std::vector<std::string> known_intersections() const;

private:
  RoiRegistry() = default;

  struct IntersectionData {
    cv::Point2d center{0, 0};
    bool has_center = false;
    std::vector<RoiSpec> rois;  // 全量, 通过 type 字段分类
    // movement_rules: key = entry_id + "\x1f" + exit_id (单元分隔符避免歧义)
    std::unordered_map<std::string, std::string> movement_rules;
  };

  void rebuild_from(std::vector<RoiSpec> specs);
  void infer_intersection_centers();
  void infer_movement_rules();

  static cv::Point2d compute_centroid(const std::vector<cv::Point2d> &poly);
  static bool point_in_polygon(double x, double y,
                               const std::vector<cv::Point2d> &poly);
  static std::string classify_movement(const cv::Point2d &center,
                                       const cv::Point2d &entry_centroid,
                                       const cv::Point2d &exit_centroid);

  mutable std::shared_mutex mtx_;
  std::unordered_map<std::string, IntersectionData> data_;
};

}  // namespace yolo_edge
