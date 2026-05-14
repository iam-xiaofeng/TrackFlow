// 独立验证: 直接调 PROJ + OpenCV 复现 C++ 修复后的链路, 与 pyproj 结果对比
// build:
//   g++ -std=c++20 -O2 scripts/geo_validate.cpp $(pkg-config --libs --cflags proj opencv4) -o /tmp/geo_validate
// run:
//   /tmp/geo_validate

#include <cstdio>
#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <proj.h>

int main() {
  // ===== 测试 case (一组典型上海陆家嘴附近的合成值, 仅做数值一致性验证) =====
  double origin_lon = 121.5067;
  double origin_lat = 31.2399;

  // 一个随便的 3x3 单应矩阵 (像素→地面米)
  double H_data[9] = {
      0.0527410344,  0.0318683830, -150.572796,
      0.0293295034, -0.0486227823,   13.7203625,
      4.69869138e-06, -4.19682072e-05, 1.0,
  };
  cv::Mat H(3, 3, CV_64F, H_data);

  // 一个像素点 (e.g. 检测中心)
  double px = 1024.0, py = 512.0;

  // ===== 同 traj_keyong.py: cv2.perspectiveTransform =====
  std::vector<cv::Point2f> in = {cv::Point2f((float)px, (float)py)};
  std::vector<cv::Point2f> out;
  cv::perspectiveTransform(in, out, H);
  double ground_x = out[0].x;
  double ground_y = out[0].y;
  printf("ground (x, y) = (%.6f, %.6f) m\n", ground_x, ground_y);

  // ===== UTM 配置 =====
  int zone = (int)((origin_lon + 180.0) / 6.0) + 1;
  int epsg = (origin_lat >= 0 ? 32600 : 32700) + zone;
  printf("UTM zone=%d EPSG=%d\n", zone, epsg);

  PJ_CONTEXT *ctx = proj_context_create();
  char utm_def[32];
  snprintf(utm_def, sizeof(utm_def), "EPSG:%d", epsg);

  // ----- 修复前 (默认 axis order, lat-first) -----
  PJ *p_buggy = proj_create_crs_to_crs(ctx, utm_def, "EPSG:4326", nullptr);
  // origin UTM via INV
  PJ_COORD o_wgs_buggy = proj_coord(origin_lon, origin_lat, 0, 0);
  PJ_COORD o_utm_buggy = proj_trans(p_buggy, PJ_INV, o_wgs_buggy);
  double ox_b = o_utm_buggy.xy.x, oy_b = o_utm_buggy.xy.y;
  PJ_COORD world_buggy = proj_coord(ground_x + ox_b, ground_y + oy_b, 0, 0);
  PJ_COORD ll_buggy = proj_trans(p_buggy, PJ_FWD, world_buggy);
  printf("BUGGY:  origin_utm=(%.3f, %.3f)  return=(%.6f, %.6f) -- 第1当lon, 第2当lat\n",
         ox_b, oy_b, ll_buggy.lp.lam, ll_buggy.lp.phi);
  proj_destroy(p_buggy);

  // ----- 修复后 (normalize for visualization) -----
  PJ *base = proj_create_crs_to_crs(ctx, utm_def, "EPSG:4326", nullptr);
  PJ *p_fixed = proj_normalize_for_visualization(ctx, base);
  proj_destroy(base);
  PJ_COORD o_wgs_fixed = proj_coord(origin_lon, origin_lat, 0, 0);
  PJ_COORD o_utm_fixed = proj_trans(p_fixed, PJ_INV, o_wgs_fixed);
  double ox_f = o_utm_fixed.xy.x, oy_f = o_utm_fixed.xy.y;
  PJ_COORD world_fixed = proj_coord(ground_x + ox_f, ground_y + oy_f, 0, 0);
  PJ_COORD ll_fixed = proj_trans(p_fixed, PJ_FWD, world_fixed);
  printf("FIXED:  origin_utm=(%.3f, %.3f)  return=(%.6f, %.6f) -- 第1当lon, 第2当lat\n",
         ox_f, oy_f, ll_fixed.lp.lam, ll_fixed.lp.phi);
  proj_destroy(p_fixed);
  proj_context_destroy(ctx);

  // ===== 把 ground_x, ground_y 和 origin_lon/lat 也输出, 让 Python 同样输入对比 =====
  printf("\n--- Python pyproj 对照 ---\n");
  printf("import pyproj\n");
  printf("origin_lon, origin_lat = %.10f, %.10f\n", origin_lon, origin_lat);
  printf("ground_x, ground_y = %.10f, %.10f\n", ground_x, ground_y);
  printf("utm_crs = pyproj.CRS.from_epsg(%d)\n", epsg);
  printf("t_ll2utm = pyproj.Transformer.from_crs('EPSG:4326', utm_crs, always_xy=True)\n");
  printf("ox, oy = t_ll2utm.transform(origin_lon, origin_lat)\n");
  printf("t_utm2ll = pyproj.Transformer.from_crs(utm_crs, 'EPSG:4326', always_xy=True)\n");
  printf("print(t_utm2ll.transform(ground_x+ox, ground_y+oy))\n");
  return 0;
}
