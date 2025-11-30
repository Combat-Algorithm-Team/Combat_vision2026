
// detector.cpp 实现装甲板检测主流程，包含灯条、装甲板的几何筛选、分类、去重、可视化等功能
// 依赖 OpenCV、YAML、fmt、C++17 filesystem 及本项目自定义类型
/*
结构概述
头文件引用：包含本地和第三方依赖。
匿名命名空间：提供 GUI 检查的辅助函数。
auto_aim 命名空间：实现 Detector 类，包含构造、主检测流程、辅助几何/类型/名称检查、颜色/图案提取、结果展示、灯条点修正等方法。
功能与意图
主要功能：检测输入图像中的装甲板（Armor），并对其进行分类、类型判别、去重、可视化等处理。
意图：为自动瞄准系统提供高鲁棒性、高准确率的装甲板检测与识别能力，支持调试、数据采集和模型迭代
*/
#include "detector.hpp"


#include <fmt/chrono.h>         // 用于格式化时间字符串
#include <yaml-cpp/yaml.h>      // 用于解析 YAML 配置
#include <filesystem>           // C++17 文件系统操作
#include "armor.hpp"           // 装甲板相关类型定义
#include "tools/img_tools.hpp" // 图像工具函数
#include "tools/logger.hpp"    // 日志工具
#include <cstdlib>              // getenv
#include <cstring>              // strcmp


namespace {
// 判断当前是否具备图形显示环境（X11/Wayland），用于在 headless 环境下关闭所有 GUI 显示
// 若设置 OPENCV_HEADLESS=1，则强制无 GUI
inline bool gui_available() {
  const char *disp = getenv("DISPLAY");           // X11 环境变量
  const char *wayland = getenv("WAYLAND_DISPLAY"); // Wayland 环境变量
  const char *headless = getenv("OPENCV_HEADLESS"); // OpenCV 无头模式
  if (headless && std::strcmp(headless, "1") == 0) return false;
  return disp != nullptr || wayland != nullptr;
}
}

namespace auto_aim
{

// Detector 构造函数：加载 YAML 配置，初始化分类器、参数，创建图案保存目录
Detector::Detector(const std::string & config_path, bool debug)
  : classifier_(config_path), debug_(debug)
{
  auto yaml = YAML::LoadFile(config_path); // 读取 YAML 配置

  // 各类参数初始化
  threshold_ = yaml["threshold"].as<double>(); // 二值化阈值
  max_angle_error_ = yaml["max_angle_error"].as<double>() / 57.3;  // 最大角度误差（弧度）
  min_lightbar_ratio_ = yaml["min_lightbar_ratio"].as<double>();    // 灯条最小长宽比
  max_lightbar_ratio_ = yaml["max_lightbar_ratio"].as<double>();    // 灯条最大长宽比
  min_lightbar_length_ = yaml["min_lightbar_length"].as<double>();  // 灯条最小长度
  min_armor_ratio_ = yaml["min_armor_ratio"].as<double>();          // 装甲板最小长宽比
  max_armor_ratio_ = yaml["max_armor_ratio"].as<double>();          // 装甲板最大长宽比
  max_side_ratio_ = yaml["max_side_ratio"].as<double>();            // 装甲板最大边比
  min_confidence_ = yaml["min_confidence"].as<double>();            // 分类最小置信度
  max_rectangular_error_ = yaml["max_rectangular_error"].as<double>() / 57.3; // 最大矩形误差（弧度）

  save_path_ = "patterns"; // 图案保存目录
  std::filesystem::create_directory(save_path_); // 若不存在则创建
}


// 主检测流程：输入BGR图像，输出装甲板列表
std::list<Armor> Detector::detect(const cv::Mat & bgr_img, int frame_count)
{
  // 1. 预处理：彩色转灰度，二值化
  cv::Mat gray_img;
  cv::cvtColor(bgr_img, gray_img, cv::COLOR_BGR2GRAY); // 灰度化

  cv::Mat binary_img;
  cv::threshold(gray_img, binary_img, threshold_, 255, cv::THRESH_BINARY); // 二值化
  if (debug_ && gui_available()) {
    cv::imshow("binary_img", binary_img); // 调试可视化
  }

  // 2. 轮廓提取，寻找所有可能的灯条区域
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(binary_img, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);

  // 3. 灯条筛选与属性提取
  std::size_t lightbar_id = 0;
  std::list<Lightbar> lightbars;
  for (const auto & contour : contours) {
    auto rotated_rect = cv::minAreaRect(contour); // 最小外接矩形
    auto lightbar = Lightbar(rotated_rect, lightbar_id); // 构造灯条对象

    if (!check_geometry(lightbar)) continue; // 几何筛选

    lightbar.color = get_color(bgr_img, contour); // 判断颜色
    lightbars.emplace_back(lightbar); // 加入灯条列表
    lightbar_id += 1;
  }

  // 4. 灯条排序（从左到右）
  lightbars.sort([](const Lightbar & a, const Lightbar & b) { return a.center.x < b.center.x; });

  // 5. 装甲板候选生成与筛选
  std::list<Armor> armors;
  for (auto left = lightbars.begin(); left != lightbars.end(); left++) {
    for (auto right = std::next(left); right != lightbars.end(); right++) {
      if (left->color != right->color) continue; // 颜色不同跳过

      auto armor = Armor(*left, *right); // 构造装甲板
      if (!check_geometry(armor)) continue; // 装甲板几何筛选

      armor.pattern = get_pattern(bgr_img, armor); // 提取装甲板图案ROI
      classifier_.classify(armor); // 分类器识别装甲板编号
      //armor.confidence = 1.0;
      //armor.name = ArmorName::four;
      if (!check_name(armor)) continue; // 名称/置信度筛选

      armor.type = get_type(armor); // 判定大小装甲板
      if (!check_type(armor)) continue; // 类型筛选

      armor.center_norm = get_center_norm(bgr_img, armor.center); // 归一化中心
      armors.emplace_back(armor); // 加入装甲板列表
    }
  }

  // 6. 去重：检查装甲板是否存在共用灯条的情况，重叠/相连时保留面积小或置信度高的
  for (auto armor1 = armors.begin(); armor1 != armors.end(); armor1++) {
    for (auto armor2 = std::next(armor1); armor2 != armors.end(); armor2++) {
      if (
        armor1->left.id != armor2->left.id && armor1->left.id != armor2->right.id &&
        armor1->right.id != armor2->left.id && armor1->right.id != armor2->right.id) {
        continue; // 没有共用灯条
      }

      // 装甲板重叠, 保留roi小的
      if (armor1->left.id == armor2->left.id || armor1->right.id == armor2->right.id) {
        auto area1 = armor1->pattern.cols * armor1->pattern.rows;
        auto area2 = armor2->pattern.cols * armor2->pattern.rows;
        if (area1 < area2)
          armor2->duplicated = true;
        else
          armor1->duplicated = true;
      }

      // 装甲板相连，保留置信度大的
      if (armor1->left.id == armor2->right.id || armor1->right.id == armor2->left.id) {
        if (armor1->confidence < armor2->confidence)
          armor1->duplicated = true;
        else
          armor2->duplicated = true;
      }
    }
  }

  // 7. 移除重复装甲板
  armors.remove_if([&](const Armor & a) { return a.duplicated; });

  // 8. 可视化调试
  if (debug_) show_result(binary_img, bgr_img, lightbars, armors, frame_count);

  return armors; // 返回检测结果
}

bool Detector::detect(Armor & armor, const cv::Mat & bgr_img)
{
  // 取得四个角点
  auto tl = armor.points[0];
  auto tr = armor.points[1];
  auto br = armor.points[2];
  auto bl = armor.points[3];
  // 计算向量和调整后的点
  auto lt2b = bl - tl;
  auto rt2b = br - tr;
  auto tl1 = (tl + bl) / 2 - lt2b;
  auto bl1 = (tl + bl) / 2 + lt2b;
  auto br1 = (tr + br) / 2 + rt2b;
  auto tr1 = (tr + br) / 2 - rt2b;
  auto tl2tr = tr1 - tl1;
  auto bl2br = br1 - bl1;
  auto tl2 = (tl1 + tr) / 2 - 0.75 * tl2tr;
  auto tr2 = (tl1 + tr) / 2 + 0.75 * tl2tr;
  auto bl2 = (bl1 + br) / 2 - 0.75 * bl2br;
  auto br2 = (bl1 + br) / 2 + 0.75 * bl2br;
  // 构造新的四个角点
  std::vector<cv::Point> points = {tl2, tr2, br2, bl2};
  auto armor_rotaterect = cv::minAreaRect(points);
  cv::Rect boundingBox = armor_rotaterect.boundingRect();
  // 检查boundingBox是否超出图像边界
  if (
    boundingBox.x < 0 || boundingBox.y < 0 || boundingBox.x + boundingBox.width > bgr_img.cols ||
    boundingBox.y + boundingBox.height > bgr_img.rows) {
    return false;
  }

  // 在图像上裁剪出这个矩形区域（ROI）
  cv::Mat armor_roi = bgr_img(boundingBox);
  if (armor_roi.empty()) {
    return false;
  }

  // 彩色图转灰度图
  cv::Mat gray_img;
  cv::cvtColor(armor_roi, gray_img, cv::COLOR_BGR2GRAY);
  // 进行二值化
  cv::Mat binary_img;
  cv::threshold(gray_img, binary_img, threshold_, 255, cv::THRESH_BINARY);
  // cv::imshow("binary_img", binary_img);
  // 获取轮廓点
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(binary_img, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
  // 获取灯条
  std::size_t lightbar_id = 0;
  std::list<Lightbar> lightbars;
  for (const auto & contour : contours) {
    auto rotated_rect = cv::minAreaRect(contour);
    auto lightbar = Lightbar(rotated_rect, lightbar_id);

    if (!check_geometry(lightbar)) continue;

    lightbar.color = get_color(bgr_img, contour);
    lightbar_points_corrector(lightbar, gray_img); //使用PCA修正灯条点
    lightbars.emplace_back(lightbar);
    lightbar_id += 1;
  }

  if (lightbars.size() < 2) return false;

  // 将灯条从左到右排序
  lightbars.sort([](const Lightbar & a, const Lightbar & b) { return a.center.x < b.center.x; });

  // 计算与 tl_roi, bl_roi 和 br_roi, tr_roi 距离最近的灯条
  Lightbar * closest_left_lightbar = nullptr;
  Lightbar * closest_right_lightbar = nullptr;
  float min_distance_tl_bl = std::numeric_limits<float>::max();
  float min_distance_br_tr = std::numeric_limits<float>::max();
  for (auto & lightbar : lightbars) {
    float distance_tl_bl =
      cv::norm(tl - (lightbar.top + cv::Point2f(boundingBox.x, boundingBox.y))) +
      cv::norm(bl - (lightbar.bottom + cv::Point2f(boundingBox.x, boundingBox.y)));
    if (distance_tl_bl < min_distance_tl_bl) {
      min_distance_tl_bl = distance_tl_bl;
      closest_left_lightbar = &lightbar;
    }
    float distance_br_tr =
      cv::norm(br - (lightbar.bottom + cv::Point2f(boundingBox.x, boundingBox.y))) +
      cv::norm(tr - (lightbar.top + cv::Point2f(boundingBox.x, boundingBox.y)));
    if (distance_br_tr < min_distance_br_tr) {
      min_distance_br_tr = distance_br_tr;
      closest_right_lightbar = &lightbar;
    }
  }

  // tools::logger()->debug(
  // "min_distance_br_tr + min_distance_tl_bl is {}", min_distance_br_tr + min_distance_tl_bl);
  // std::vector<cv::Point2f> points2f{
  //   closest_left_lightbar->top, closest_left_lightbar->bottom, closest_right_lightbar->bottom,
  //   closest_right_lightbar->top};
  // tools::draw_points(armor_roi, points2f, {0, 0, 255}, 2);
  // cv::imshow("armor_roi", armor_roi);

  if (
    closest_left_lightbar && closest_right_lightbar &&
    min_distance_br_tr + min_distance_tl_bl < 15) {
    // 将四个点从armor_roi坐标系转换到原始图像坐标系
    armor.points[0] = closest_left_lightbar->top + cv::Point2f(boundingBox.x, boundingBox.y);
    armor.points[1] = closest_right_lightbar->top + cv::Point2f(boundingBox.x, boundingBox.y);
    armor.points[2] = closest_right_lightbar->bottom + cv::Point2f(boundingBox.x, boundingBox.y);
    armor.points[3] = closest_left_lightbar->bottom + cv::Point2f(boundingBox.x, boundingBox.y);
    return true;
  }

  return false;
}

bool Detector::check_geometry(const Lightbar & lightbar) const
{
  auto angle_ok = lightbar.angle_error < max_angle_error_;
  auto ratio_ok = lightbar.ratio > min_lightbar_ratio_ && lightbar.ratio < max_lightbar_ratio_;
  auto length_ok = lightbar.length > min_lightbar_length_;
  return angle_ok && ratio_ok && length_ok;
}

bool Detector::check_geometry(const Armor & armor) const
{
  auto ratio_ok = armor.ratio > min_armor_ratio_ && armor.ratio < max_armor_ratio_;
  auto side_ratio_ok = armor.side_ratio < max_side_ratio_;
  auto rectangular_error_ok = armor.rectangular_error < max_rectangular_error_;
  return ratio_ok && side_ratio_ok && rectangular_error_ok;
}

bool Detector::check_name(const Armor & armor) const
{
  auto name_ok = (armor.name != ArmorName::not_armor);
  auto confidence_ok = armor.confidence > min_confidence_;

  // 保存不确定的图案，用于分类器的迭代
  if (name_ok && !confidence_ok) /*save(armor)*/;

  // 出现 5号 则显示 debug 信息。但不过滤。
  if (armor.name == ArmorName::five) tools::logger()->debug("See pattern 5");

  return name_ok && confidence_ok;
}

bool Detector::check_type(const Armor & armor) const
{
  auto name_ok = armor.type == ArmorType::small
                   ? (armor.name != ArmorName::one && armor.name != ArmorName::base)
                   : (armor.name == ArmorName::one || armor.name == ArmorName::base);

  // 保存异常的图案，用于分类器的迭代
  name_ok = true; // 临时放宽限制，方便采集数据
  if (!name_ok) {
    tools::logger()->debug(
      "see strange armor: {} {}", ARMOR_TYPES[armor.type], ARMOR_NAMES[armor.name]);
    //save(armor);
  }

  return name_ok;
}

Color Detector::get_color(const cv::Mat & bgr_img, const std::vector<cv::Point> & contour) const
{
  int red_sum = 0, blue_sum = 0;

  for (const auto & point : contour) {
    red_sum += bgr_img.at<cv::Vec3b>(point)[2];
    blue_sum += bgr_img.at<cv::Vec3b>(point)[0];
  }

  return blue_sum > red_sum ? Color::blue : Color::red;
}

cv::Mat Detector::get_pattern(const cv::Mat & bgr_img, const Armor & armor) const
{
  // 透视矫正：使用灯条四点将装甲板数字区域“拉正”，得到固定尺寸的正视图
  // 参考实现参数
  static const int light_length = 12;     // 图中两灯条的垂直长度
  static const int warp_height = 28;      // 矫正后图像高度
  static const int small_armor_width = 10;
  static const int large_armor_width = 32;

  // 灯条四点（左下、左上、右上、右下），与参考实现保持一致顺序
  cv::Point2f lights_vertices[4] = {
    armor.left.bottom, armor.left.top, armor.right.top, armor.right.bottom};

  // 目标平面上两灯条的垂直位置（保持与参考实现一致）
  const int top_light_y = (warp_height - light_length) / 2 - 1;
  const int bottom_light_y = top_light_y + light_length;
  const int warp_width = (armor.type == ArmorType::small) ? small_armor_width : large_armor_width;

  cv::Point2f target_vertices[4] = {
    cv::Point2f(0.f, static_cast<float>(bottom_light_y)),
    cv::Point2f(0.f, static_cast<float>(top_light_y)),
    cv::Point2f(static_cast<float>(warp_width - 1), static_cast<float>(top_light_y)),
    cv::Point2f(static_cast<float>(warp_width - 1), static_cast<float>(bottom_light_y)),
  };

  // 计算源点的紧致ROI，减少warp时的访存与插值成本
  cv::Rect src_bounds = cv::boundingRect(std::vector<cv::Point2f>{
    lights_vertices[0], lights_vertices[1], lights_vertices[2], lights_vertices[3]});
  // 适当加一点边距，防止边缘截断
  const int margin = 15;
  src_bounds.x = std::max(0, src_bounds.x - margin);
  src_bounds.y = std::max(0, src_bounds.y - margin);
  src_bounds.width = std::min(bgr_img.cols - src_bounds.x, src_bounds.width + 2 * margin);
  src_bounds.height = std::min(bgr_img.rows - src_bounds.y, src_bounds.height + 2 * margin);

  // 将源点坐标平移到ROI坐标系
  cv::Point2f offset(static_cast<float>(src_bounds.x), static_cast<float>(src_bounds.y));
  cv::Point2f src_in_roi[4] = {
    lights_vertices[0] - offset, lights_vertices[1] - offset,
    lights_vertices[2] - offset, lights_vertices[3] - offset};

  // 计算透视矩阵并进行矫正（在小ROI上warp，目标尺寸依旧小，速度更快）
  cv::Mat M = cv::getPerspectiveTransform(src_in_roi, target_vertices);
  cv::Mat warped;
  if (!M.empty()) {
    // 使用最近邻插值以进一步降低开销；对后续二值化与分类影响很小
    cv::warpPerspective(bgr_img(src_bounds), warped, M, cv::Size(warp_width, warp_height),
                        cv::INTER_NEAREST, cv::BORDER_REPLICATE);
  }

  // 若透视失败，退回到原先的矩形 ROI 策略，保证稳健性
  if (warped.empty()) {
    // 延长灯条获得装甲板角点（原逻辑）
    auto tl = armor.left.center - armor.left.top2bottom * 1.125;
    auto bl = armor.left.center + armor.left.top2bottom * 1.125;
    auto tr = armor.right.center - armor.right.top2bottom * 1.125;
    auto br = armor.right.center + armor.right.top2bottom * 1.125;

    auto roi_left = std::max<int>(std::min(tl.x, bl.x) + 50, 0);
    auto roi_top = std::max<int>(std::min(tl.y, tr.y), 0);
    auto roi_right = std::min<int>(std::max(tr.x, br.x) - 50, bgr_img.cols);
    auto roi_bottom = std::min<int>(std::max(bl.y, br.y), bgr_img.rows);
    auto roi_tl = cv::Point(roi_left, roi_top);
    auto roi_br = cv::Point(roi_right, roi_bottom);
    auto roi = cv::Rect(roi_tl, roi_br) & cv::Rect(0, 0, bgr_img.cols, bgr_img.rows);
    return bgr_img(roi);
  }

  // 从正视图中间截取 20x28 的数字 ROI
  const int roi_w = 24;
  const int roi_h = 28;
  int x0 = std::max(0, (warp_width - roi_w) / 2);
  int y0 = 0;
  // 边界保护
  if (x0 + roi_w > warped.cols) x0 = std::max(0, warped.cols - roi_w);
  if (y0 + roi_h > warped.rows) y0 = std::max(0, warped.rows - roi_h);

  cv::Rect number_roi(x0, y0, std::min(roi_w, warped.cols - x0), std::min(roi_h, warped.rows - y0));
  return warped(number_roi);
}

ArmorType Detector::get_type(const Armor & armor)
{
  /// 优先根据当前armor.ratio判断
  /// TODO: 25赛季是否还需要根据比例判断大小装甲？能否根据图案直接判断？

  if (armor.ratio > 3.0) {
    // tools::logger()->debug(
    //   "[Detector] get armor type by ratio: BIG {} {:.2f}", ARMOR_NAMES[armor.name], armor.ratio);
    return ArmorType::big;
  }

  if (armor.ratio < 2.5) {
    // tools::logger()->debug(
    //   "[Detector] get armor type by ratio: SMALL {} {:.2f}", ARMOR_NAMES[armor.name], armor.ratio);
    return ArmorType::small;
  }

  // tools::logger()->debug("[Detector] get armor type by name: {}", ARMOR_NAMES[armor.name]);

  // 英雄、基地只能是大装甲板
  if (armor.name == ArmorName::one || armor.name == ArmorName::base) {
    return ArmorType::big;
  }

  // 其他所有（工程、哨兵、前哨站、步兵）都是小装甲板
  /// TODO: 基地顶装甲是小装甲板
  return ArmorType::small;
}

cv::Point2f Detector::get_center_norm(const cv::Mat & bgr_img, const cv::Point2f & center) const
{
  auto h = bgr_img.rows;
  auto w = bgr_img.cols;
  return {center.x / w, center.y / h};
}

void Detector::save(const Armor & armor) const
{
  auto file_name = fmt::format("{:%Y-%m-%d_%H-%M-%S}", std::chrono::system_clock::now());
  auto img_path = fmt::format("{}/{}_{}.jpg", save_path_, armor.name, file_name);
  cv::imwrite(img_path, armor.pattern);
}

void Detector::show_result(
  const cv::Mat & binary_img, const cv::Mat & bgr_img, const std::list<Lightbar> & lightbars,
  const std::list<Armor> & armors, int frame_count) const
{
  auto detection = bgr_img.clone();
  tools::draw_text(detection, fmt::format("[{}]", frame_count), {10, 30}, {255, 255, 255});

  for (const auto & lightbar : lightbars) {
    auto info = fmt::format(
      "{:.1f} {:.1f} {:.1f} {}", lightbar.angle_error * 57.3, lightbar.ratio, lightbar.length,
      COLORS[lightbar.color]);
    tools::draw_text(detection, info, lightbar.top, {0, 255, 255});
    tools::draw_points(detection, lightbar.points, {0, 255, 255}, 3);
  }

  for (const auto & armor : armors) {
    auto info = fmt::format(
      "{:.2f} {:.2f} {:.1f} {:.2f} {} {}", armor.ratio, armor.side_ratio,
      armor.rectangular_error * 57.3, armor.confidence, ARMOR_NAMES[armor.name],
      ARMOR_TYPES[armor.type]);
    tools::draw_points(detection, armor.points, {0, 255, 0});
    tools::draw_text(detection, info, armor.left.bottom, {0, 255, 0});
  }

  if (debug_ && gui_available()) {
    // cv::imshow("threshold", binary_img);
    cv::imshow("detection", detection);
  }
}

void Detector::lightbar_points_corrector(Lightbar & lightbar, const cv::Mat & gray_img) const
{
  // 配置参数
  constexpr float MAX_BRIGHTNESS = 25;  // 归一化最大亮度值
  constexpr float ROI_SCALE = 0.07;     // ROI扩展比例
  constexpr float SEARCH_START = 0.4;   // 搜索起始位置比例（原0.8/2）
  constexpr float SEARCH_END = 0.6;     // 搜索结束位置比例（原1.2/2）

  // 扩展并裁剪ROI
  cv::Rect roi_box = lightbar.rotated_rect.boundingRect();
  roi_box.x -= roi_box.width * ROI_SCALE;
  roi_box.y -= roi_box.height * ROI_SCALE;
  roi_box.width += 2 * roi_box.width * ROI_SCALE;
  roi_box.height += 2 * roi_box.height * ROI_SCALE;

  // 边界约束
  roi_box &= cv::Rect(0, 0, gray_img.cols, gray_img.rows);

  // 归一化ROI
  cv::Mat roi = gray_img(roi_box);
  const float mean_val = cv::mean(roi)[0];
  roi.convertTo(roi, CV_32F);
  cv::normalize(roi, roi, 0, MAX_BRIGHTNESS, cv::NORM_MINMAX);

  // 计算质心
  const cv::Moments moments = cv::moments(roi);
  const cv::Point2f centroid(
    moments.m10 / moments.m00 + roi_box.x, moments.m01 / moments.m00 + roi_box.y);

  // 生成稀疏点云（优化性能）
  std::vector<cv::Point2f> points;
  for (int i = 0; i < roi.rows; ++i) {
    for (int j = 0; j < roi.cols; ++j) {
      const float weight = roi.at<float>(i, j);
      if (weight > 1e-3) {          // 忽略极小值提升性能
        points.emplace_back(j, i);  // 坐标相对于ROI区域
      }
    }
  }

  // PCA计算对称轴方向
  cv::PCA pca(cv::Mat(points).reshape(1), cv::Mat(), cv::PCA::DATA_AS_ROW);
  cv::Point2f axis(pca.eigenvectors.at<float>(0, 0), pca.eigenvectors.at<float>(0, 1));
  axis /= cv::norm(axis);
  if (axis.y > 0) axis = -axis;  // 统一方向

  const auto find_corner = [&](int direction) -> cv::Point2f {
    const float dx = axis.x * direction;
    const float dy = axis.y * direction;
    const float search_length = lightbar.length * (SEARCH_END - SEARCH_START);

    std::vector<cv::Point2f> candidates;

    // 横向采样多个候选线
    const int half_width = (lightbar.width - 2) / 2;
    for (int i_offset = -half_width; i_offset <= half_width; ++i_offset) {
      // 计算搜索起点
      cv::Point2f start_point(
        centroid.x + lightbar.length * SEARCH_START * dx + i_offset,
        centroid.y + lightbar.length * SEARCH_START * dy);

      // 沿轴搜索亮度跳变点
      cv::Point2f corner = start_point;
      float max_diff = 0;
      bool found = false;

      for (float step = 0; step < search_length; ++step) {
        const cv::Point2f cur_point(start_point.x + dx * step, start_point.y + dy * step);

        // 边界检查
        if (
          cur_point.x < 0 || cur_point.x >= gray_img.cols || cur_point.y < 0 ||
          cur_point.y >= gray_img.rows) {
          break;
        }

        // 计算亮度差（使用双线性插值提升精度）
        const auto prev_val = gray_img.at<uchar>(cv::Point2i(cur_point - cv::Point2f(dx, dy)));
        const auto cur_val = gray_img.at<uchar>(cv::Point2i(cur_point));
        const float diff = prev_val - cur_val;

        if (diff > max_diff && prev_val > mean_val) {
          max_diff = diff;
          corner = cur_point - cv::Point2f(dx, dy);  // 跳变发生在上一位置
          found = true;
        }
      }

      if (found) {
        candidates.push_back(corner);
      }
    }

    // 返回候选点均值
    return candidates.empty()
             ? cv::Point2f(-1, -1)
             : std::accumulate(candidates.begin(), candidates.end(), cv::Point2f(0, 0)) /
                 static_cast<float>(candidates.size());
  };

  // 并行检测顶部和底部
  lightbar.top = find_corner(1);
  lightbar.bottom = find_corner(-1);
}

}  // namespace auto_aim