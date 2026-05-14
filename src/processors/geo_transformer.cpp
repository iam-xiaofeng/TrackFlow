#include "processors/geo_transformer.hpp"
#include "core/processor_factory.hpp"
#include <chrono>
#include <cstdio>
#include <opencv2/calib3d.hpp>

#include <proj.h>

namespace yolo_edge {

GeoTransformer::GeoTransformer() = default;

GeoTransformer::~GeoTransformer() {
  if (proj_) {
    proj_destroy(static_cast<PJ *>(proj_));
  }
  if (proj_ctx_) {
    proj_context_destroy(static_cast<PJ_CONTEXT *>(proj_ctx_));
  }
}

void GeoTransformer::configure(const json &config) {
  if (!config.contains("homography")) {
    fprintf(stderr, "[WARN] GeoTransformer: No homography matrix provided, "
                    "will skip transformation\n");
    return;
  }

  auto h_data = config["homography"].get<std::vector<double>>();
  if (h_data.size() != 9) {
    fprintf(stderr,
            "[ERROR] GeoTransformer: Homography matrix must have 9 elements\n");
    return;
  }

  H_ = cv::Mat(3, 3, CV_64F);
  for (int i = 0; i < 9; ++i) {
    H_.at<double>(i / 3, i % 3) = h_data[i];
  }

  if (!config.contains("origin_lon") || !config.contains("origin_lat")) {
    fprintf(stderr,
            "[ERROR] GeoTransformer: Missing origin_lon or origin_lat\n");
    return;
  }

  origin_lon_ = config["origin_lon"].get<double>();
  origin_lat_ = config["origin_lat"].get<double>();

  if (config.contains("camera_matrix") && config.contains("dist_coeffs")) {
    auto k_data = config["camera_matrix"].get<std::vector<double>>();
    if (k_data.size() == 9) {
      K_ = cv::Mat(3, 3, CV_64F);
      for (int i = 0; i < 9; ++i) {
        K_.at<double>(i / 3, i % 3) = k_data[i];
      }

      auto d_data = config["dist_coeffs"].get<std::vector<double>>();
      dist_ = cv::Mat(1, d_data.size(), CV_64F);
      for (size_t i = 0; i < d_data.size(); ++i) {
        dist_.at<double>(0, i) = d_data[i];
      }

      has_camera_params_ = true;
    }
  }

  init_proj();
  initialized_ = true;

  fprintf(stderr,
          "[INFO] GeoTransformer: Configured with origin (%.6f, %.6f)\n",
          origin_lon_, origin_lat_);
}

void GeoTransformer::init_proj() {
  proj_ctx_ = proj_context_create();

  int zone = static_cast<int>((origin_lon_ + 180.0) / 6.0) + 1;
  bool is_north = origin_lat_ >= 0;
  int epsg = is_north ? (32600 + zone) : (32700 + zone);

  std::string utm_def = "EPSG:" + std::to_string(epsg);
  PJ *base = proj_create_crs_to_crs(static_cast<PJ_CONTEXT *>(proj_ctx_),
                                    utm_def.c_str(), "EPSG:4326", nullptr);
  if (!base) {
    fprintf(stderr,
            "[ERROR] GeoTransformer: Failed to create PROJ transformation\n");
    return;
  }

  // 关键: 默认 PROJ ≥ 6 遵循 EPSG 轴顺序, EPSG:4326 是 (lat, lon),
  // 这会让 proj_coord(lon, lat) 被解释成 (lat, lon) 而完全错位。
  // normalize_for_visualization 强制所有地理 CRS 使用 (lon, lat)、
  // 投影 CRS 使用 (east, north), 与 pyproj 的 always_xy=True 行为一致。
  proj_ = proj_normalize_for_visualization(
      static_cast<PJ_CONTEXT *>(proj_ctx_), base);
  proj_destroy(base);

  if (!proj_) {
    fprintf(stderr,
            "[ERROR] GeoTransformer: proj_normalize_for_visualization failed\n");
    return;
  }

  // 原点的 UTM 坐标 (PJ_INV: 4326→UTM, 输入是 (lon, lat) 因为已 normalize)
  PJ_COORD origin_wgs84 = proj_coord(origin_lon_, origin_lat_, 0, 0);
  PJ_COORD origin_utm =
      proj_trans(static_cast<PJ *>(proj_), PJ_INV, origin_wgs84);
  origin_utm_x_ = origin_utm.xy.x;
  origin_utm_y_ = origin_utm.xy.y;
}

std::pair<double, double> GeoTransformer::utm_to_lonlat(double easting,
                                                        double northing) {
  double world_x = easting + origin_utm_x_;
  double world_y = northing + origin_utm_y_;

  // PJ_FWD: UTM→4326. proj_ 已 normalize, 输出按 (lon, lat) 顺序存入 v[0]/v[1].
  // PJ_LP.lam=v[0]=lon, PJ_LP.phi=v[1]=lat, 名称与含义一致。
  PJ_COORD utm = proj_coord(world_x, world_y, 0, 0);
  PJ_COORD lonlat = proj_trans(static_cast<PJ *>(proj_), PJ_FWD, utm);

  return {lonlat.lp.lam, lonlat.lp.phi};
}

bool GeoTransformer::process(ProcessingContext &ctx) {
  if (!initialized_ || ctx.detections.empty()) {
    return true;
  }

  using Clock = std::chrono::high_resolution_clock;
  auto start = Clock::now();

  std::vector<cv::Point2f> centers;
  centers.reserve(ctx.detections.size());
  for (const auto &det : ctx.detections) {
    centers.push_back(det.obb.center);
  }

  // 难点：畸变校正和单应变换都支持向量化，批量处理能明显减少小对象分配。
  if (has_camera_params_ && !ctx.get_or<bool>("undistorted", false)) {
    std::vector<cv::Point2f> undistorted;
    cv::undistortPoints(centers, undistorted, K_, dist_, cv::noArray(), K_);
    centers = std::move(undistorted);
  }

  std::vector<cv::Point2f> ground_points;
  cv::perspectiveTransform(centers, ground_points, H_);

  for (size_t i = 0; i < ctx.detections.size(); ++i) {
    auto &det = ctx.detections[i];
    const auto &ground = ground_points[i];
    det.ground_x = ground.x;
    det.ground_y = ground.y;
    auto [lon, lat] = utm_to_lonlat(ground.x, ground.y);
    det.lon = lon;
    det.lat = lat;
  }

  auto end = Clock::now();
  ctx.geo_time_ms =
      std::chrono::duration<double, std::milli>(end - start).count();

  return true;
}

REGISTER_PROCESSOR("geo_transform", GeoTransformer);

} // namespace yolo_edge
