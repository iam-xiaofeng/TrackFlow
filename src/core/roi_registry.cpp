#include "core/roi_registry.hpp"

#include <mysql/mysql.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace yolo_edge {

namespace {
using json = nlohmann::json;

// 单元分隔符 0x1F, 不可能出现在 ROI id 里
inline std::string pair_key(const std::string &a, const std::string &b) {
  return a + "\x1f" + b;
}

}  // namespace

RoiRegistry &RoiRegistry::instance() {
  static RoiRegistry inst;
  return inst;
}

cv::Point2d RoiRegistry::compute_centroid(const std::vector<cv::Point2d> &poly) {
  if (poly.empty()) return {0, 0};
  // 简单算术平均, 对凸多边形足够
  double sx = 0, sy = 0;
  for (const auto &p : poly) {
    sx += p.x;
    sy += p.y;
  }
  return {sx / poly.size(), sy / poly.size()};
}

bool RoiRegistry::point_in_polygon(double x, double y,
                                   const std::vector<cv::Point2d> &poly) {
  if (poly.size() < 3) return false;
  bool inside = false;
  size_t n = poly.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    double xi = poly[i].x, yi = poly[i].y;
    double xj = poly[j].x, yj = poly[j].y;
    bool intersect = ((yi > y) != (yj > y)) &&
                     (x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi);
    if (intersect) inside = !inside;
  }
  return inside;
}

std::string RoiRegistry::classify_movement(const cv::Point2d &center,
                                           const cv::Point2d &entry_centroid,
                                           const cv::Point2d &exit_centroid) {
  // 入车流方向: 从进口 centroid 指向 path 中心
  double ex = center.x - entry_centroid.x;
  double ey = center.y - entry_centroid.y;
  // 出车流方向: 从中心指向出口 centroid
  double xx = exit_centroid.x - center.x;
  double xy = exit_centroid.y - center.y;

  double en = std::hypot(ex, ey);
  double xn = std::hypot(xx, xy);
  if (en < 1e-6 || xn < 1e-6) return "other";
  ex /= en;
  ey /= en;
  xx /= xn;
  xy /= xn;

  double dot = ex * xx + ey * xy;
  // 图像坐标系 y 向下, "右转" 视觉上是顺时针 = 正 cross
  double cross = ex * xy - ey * xx;
  double angle_deg = std::atan2(cross, dot) * 180.0 / M_PI;

  if (std::fabs(angle_deg) < 30.0) return "straight";
  if (std::fabs(angle_deg) > 150.0) return "u_turn";
  if (angle_deg > 0) return "right";
  return "left";
}

void RoiRegistry::infer_intersection_centers() {
  for (auto &[iid, data] : data_) {
    // 先看是否有 type=center 的 ROI
    cv::Point2d c{0, 0};
    int n_center = 0;
    for (const auto &r : data.rois) {
      if (r.type == "center") {
        c += r.centroid;
        ++n_center;
      }
    }
    if (n_center > 0) {
      data.center = c * (1.0 / n_center);
      data.has_center = true;
      continue;
    }
    // 退化方案: 取所有进口/出口 centroid 的均值
    int n_app = 0;
    cv::Point2d sum{0, 0};
    for (const auto &r : data.rois) {
      if (r.type == "approach" || r.type == "exit") {
        sum += r.centroid;
        ++n_app;
      }
    }
    if (n_app > 0) {
      data.center = sum * (1.0 / n_app);
      data.has_center = true;
    }
  }
}

void RoiRegistry::infer_movement_rules() {
  for (auto &[iid, data] : data_) {
    if (!data.has_center) continue;
    data.movement_rules.clear();
    std::vector<const RoiSpec *> apps, exs;
    for (const auto &r : data.rois) {
      if (r.type == "approach") apps.push_back(&r);
      if (r.type == "exit") exs.push_back(&r);
    }
    for (const auto *a : apps) {
      for (const auto *e : exs) {
        std::string mv = classify_movement(data.center, a->centroid, e->centroid);
        data.movement_rules[pair_key(a->id, e->id)] = mv;
      }
    }
  }
}

void RoiRegistry::rebuild_from(std::vector<RoiSpec> specs) {
  std::unordered_map<std::string, IntersectionData> next;
  for (auto &s : specs) {
    if (s.polygon.size() < 3 && s.type != "stop_line") continue;
    if (s.centroid.x == 0 && s.centroid.y == 0) {
      s.centroid = compute_centroid(s.polygon);
    }
    next[s.intersection_id].rois.push_back(std::move(s));
  }

  std::unique_lock<std::shared_mutex> lk(mtx_);
  data_ = std::move(next);
  lk.unlock();

  // infer 阶段也需要写访问
  std::unique_lock<std::shared_mutex> lk2(mtx_);
  infer_intersection_centers();
  infer_movement_rules();
}

void RoiRegistry::load_from_specs(std::vector<RoiSpec> specs) {
  rebuild_from(std::move(specs));
}

int RoiRegistry::reload_from_mysql(const DbConfig &cfg) {
  MYSQL *m = mysql_init(nullptr);
  if (!m) return -1;
  if (!mysql_real_connect(m, cfg.host.c_str(), cfg.user.c_str(),
                          cfg.password.c_str(), cfg.db.c_str(),
                          static_cast<unsigned int>(cfg.port), nullptr, 0)) {
    fprintf(stderr, "[ERROR] RoiRegistry connect: %s\n", mysql_error(m));
    mysql_close(m);
    return -1;
  }

  const char *sql =
      "SELECT id, intersection_id, camera_id, type, "
      "IFNULL(parent_id, ''), polygon_json, "
      "IFNULL(JSON_EXTRACT(allowed_movements, '$'), 'null') "
      "FROM roi_configs";
  if (mysql_query(m, sql) != 0) {
    fprintf(stderr, "[ERROR] RoiRegistry query: %s\n", mysql_error(m));
    mysql_close(m);
    return -1;
  }
  MYSQL_RES *res = mysql_store_result(m);
  if (!res) {
    fprintf(stderr, "[ERROR] RoiRegistry store_result: %s\n", mysql_error(m));
    mysql_close(m);
    return -1;
  }

  std::vector<RoiSpec> specs;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    RoiSpec s;
    s.id = row[0] ? row[0] : "";
    s.intersection_id = row[1] ? row[1] : "";
    s.camera_id = row[2] ? row[2] : "default";
    s.type = row[3] ? row[3] : "";
    s.parent_id = row[4] ? row[4] : "";
    if (row[5]) {
      try {
        auto j = json::parse(row[5]);
        for (auto &pt : j) {
          if (pt.is_array() && pt.size() >= 2) {
            s.polygon.emplace_back(pt[0].get<double>(), pt[1].get<double>());
          }
        }
      } catch (const std::exception &e) {
        fprintf(stderr, "[WARN] RoiRegistry: bad polygon_json for %s: %s\n",
                s.id.c_str(), e.what());
        continue;
      }
    }
    if (row[6]) {
      try {
        auto j = json::parse(row[6]);
        if (j.is_array()) {
          for (auto &m : j) {
            if (m.is_string()) s.allowed_movements.push_back(m.get<std::string>());
          }
        }
      } catch (...) {
        // 容忍 NULL / 非法 JSON
      }
    }
    specs.push_back(std::move(s));
  }
  mysql_free_result(res);
  mysql_close(m);

  int count = static_cast<int>(specs.size());
  rebuild_from(std::move(specs));
  fprintf(stderr, "[INFO] RoiRegistry loaded %d ROIs across %zu intersections\n",
          count, known_intersections().size());
  return count;
}

PointLocation RoiRegistry::locate(const std::string &intersection_id, double x,
                                  double y) const {
  std::shared_lock<std::shared_mutex> lk(mtx_);
  PointLocation out;
  auto it = data_.find(intersection_id);
  if (it == data_.end()) return out;
  // 优先级: 车道 > 进口/出口 > 中心 > 人行道
  // 但为了支持"行人在路口中心" + "右转车在冲突区", 都查一遍
  for (const auto &r : it->second.rois) {
    if (r.type == "lane" && point_in_polygon(x, y, r.polygon)) {
      out.lane_id = r.id;
      if (out.region_id.empty()) out.region_id = r.parent_id;
    }
  }
  if (out.region_id.empty()) {
    for (const auto &r : it->second.rois) {
      if ((r.type == "approach" || r.type == "exit") &&
          point_in_polygon(x, y, r.polygon)) {
        out.region_id = r.id;
        break;
      }
    }
  }
  for (const auto &r : it->second.rois) {
    if (r.type == "crosswalk" && point_in_polygon(x, y, r.polygon)) {
      out.in_crosswalk = r.id;
      break;
    }
  }
  for (const auto &r : it->second.rois) {
    if (r.type == "center" && point_in_polygon(x, y, r.polygon)) {
      out.in_center = true;
      break;
    }
  }
  return out;
}

std::string RoiRegistry::lookup_movement(const std::string &intersection_id,
                                         const std::string &entry_region_id,
                                         const std::string &exit_region_id) const {
  std::shared_lock<std::shared_mutex> lk(mtx_);
  auto it = data_.find(intersection_id);
  if (it == data_.end()) return "other";
  auto mit = it->second.movement_rules.find(pair_key(entry_region_id, exit_region_id));
  if (mit == it->second.movement_rules.end()) return "other";
  return mit->second;
}

std::vector<std::string> RoiRegistry::allowed_movements(
    const std::string &intersection_id, const std::string &lane_id) const {
  std::shared_lock<std::shared_mutex> lk(mtx_);
  auto it = data_.find(intersection_id);
  if (it == data_.end()) return {};
  for (const auto &r : it->second.rois) {
    if (r.id == lane_id) return r.allowed_movements;
  }
  return {};
}

std::optional<cv::Point2d> RoiRegistry::intersection_center(
    const std::string &intersection_id) const {
  std::shared_lock<std::shared_mutex> lk(mtx_);
  auto it = data_.find(intersection_id);
  if (it == data_.end() || !it->second.has_center) return std::nullopt;
  return it->second.center;
}

size_t RoiRegistry::roi_count() const {
  std::shared_lock<std::shared_mutex> lk(mtx_);
  size_t total = 0;
  for (const auto &[_, d] : data_) total += d.rois.size();
  return total;
}

std::vector<std::string> RoiRegistry::known_intersections() const {
  std::shared_lock<std::shared_mutex> lk(mtx_);
  std::vector<std::string> v;
  v.reserve(data_.size());
  for (const auto &[k, _] : data_) v.push_back(k);
  std::sort(v.begin(), v.end());
  return v;
}

}  // namespace yolo_edge
