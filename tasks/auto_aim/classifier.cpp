/*
这个文件是 classifier.cpp，它是 C++ 源文件，属于一个机器人视觉系统（Combat
Vision）的自动瞄准模块。文件主要实现了一个 Classifier
类，用于对装甲板（Armor）图像进行分类，识别其类型（如英雄、步兵等）。代码使用了
OpenCV 和 OpenVINO 库进行深度学习推理。
推理、文件系统操作等。 匿名命名空间：包含辅助函数，用于调试和日志输出。
Classifier 类：
构造函数：加载配置文件和模型。
classify 方法：使用 OpenCV DNN 进行分类。
ovclassify 方法：使用 OpenVINO 进行分类（优化版本）。
命名空间：代码位于 auto_aim 命名空间下。
功能：
图像预处理：将输入的装甲板图像转换为灰度图，然后二值化并缩放到 28x28 像素。
神经网络推理：使用预训练的 ONNX 模型进行分类，输出置信度和标签。
调试支持：通过环境变量启用时，保存中间图像和元数据到 logs 目录。
分类结果：更新 Armor 对象的 name 和 confidence 属性。
意图：
在自动瞄准系统中，快速识别战场上的装甲板类型，帮助机器人决定瞄准策略（如优先攻击英雄装甲）。
支持两种推理后端（OpenCV 和 OpenVINO），以适应不同硬件和性能需求。
提供调试功能，便于开发和调优模型。

*/
#include "classifier.hpp" // 包含 Classifier 类的头文件，定义了类接口和依赖类型

#include <cstdio>          // 提供 printf、fopen 等文件操作
#include <cstdlib>         // 提供 getenv 等标准库函数
#include <cstring>         // 提供 strcmp 等字符串操作
#include <sys/stat.h>      // 提供 mkdir 等文件系统操作
#include <sys/types.h>     // 提供文件类型定义
#include <yaml-cpp/yaml.h> // 用于解析 YAML 配置文件

namespace auto_aim // 定义命名空间，避免命名冲突
{
namespace { // 匿名命名空间，内部函数仅在此文件中可见

// 检查环境变量 CLASSIFIER_DEBUG_DUMP 是否启用调试模式
inline bool dump_enabled() {
  const char *env = std::getenv("CLASSIFIER_DEBUG_DUMP"); // 获取环境变量
  return env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0 ||
                 std::strcmp(env, "TRUE") == 0 || std::strcmp(env, "on") == 0 ||
                 std::strcmp(env, "ON") == 0); // 支持多种 true 值
}

// 可选：反色输入（用于训练使用了相反极性的情况）
inline bool invert_enabled() {
  const char *env = std::getenv("CLASSIFIER_INVERT");
  return env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0 ||
                 std::strcmp(env, "TRUE") == 0 || std::strcmp(env, "on") == 0 ||
                 std::strcmp(env, "ON") == 0);
}

// 确保 logs 目录存在（若不存在则创建）
inline void ensure_logs_dir() {
  // 创建 logs 目录（若已存在则忽略错误）
  ::mkdir("logs", 0755); // 使用系统调用创建目录，权限 0755
}

// 生成下一个调试转储 ID（静态变量，确保唯一性）
inline int next_dump_id() {
  static int id = 0; // 静态变量，函数调用间保持状态
  return ++id;       // 返回递增的 ID
}

// 保存图像和元数据到文件（用于调试）
inline void dump_images_and_meta(const cv::Mat &pattern, const cv::Mat &gray,
                                 const cv::Mat &inputN, const char *stage,
                                 int w, int h, double scale, int label,
                                 double conf, int x0 = -1, int y0 = -1) {
  // 仅写到 logs/ 目录（项目已存在该目录），避免创建新目录带来的依赖
  ensure_logs_dir();             // 确保目录存在
  const int id = next_dump_id(); // 获取唯一 ID
  char base[256];                // 基础文件名缓冲区
  std::snprintf(base, sizeof(base), "logs/classifier_%06d_%s", id,
                stage ? stage : "stage"); // 格式化基础文件名

  char p1[300], p2[300], p3[300], p4[300];               // 文件路径缓冲区
  std::snprintf(p1, sizeof(p1), "%s_pattern.jpg", base); // 原始图像路径
  std::snprintf(p2, sizeof(p2), "%s_gray.jpg", base);    // 灰度图像路径
  std::snprintf(p3, sizeof(p3), "%s_input.png",
                base); // 归一化后输入图像路径（自适应尺寸命名）
  std::snprintf(p4, sizeof(p4), "%s_meta.txt", base); // 元数据文件路径

  if (!pattern.empty())
    cv::imwrite(p1, pattern); // 保存原始图像（若不为空）
  if (!gray.empty())
    cv::imwrite(p2, gray); // 保存灰度图像（若不为空）
  if (!inputN.empty())
    cv::imwrite(p3, inputN); // 保存输入图像（若不为空）

  FILE *fp = std::fopen(p4, "w");              // 打开元数据文件写入
  if (fp) {                                    // 若打开成功
    std::fprintf(fp, "w=%d\n", w);             // 写入宽度
    std::fprintf(fp, "h=%d\n", h);             // 写入高度
    std::fprintf(fp, "scale=%f\n", scale);     // 写入缩放比例
    std::fprintf(fp, "label=%d\n", label);     // 写入标签
    std::fprintf(fp, "confidence=%f\n", conf); // 写入置信度
    if (x0 >= 0 && y0 >= 0) {
      std::fprintf(fp, "x0=%d\n", x0); // 居中偏移
      std::fprintf(fp, "y0=%d\n", y0);
    }
    std::fclose(fp); // 关闭文件
  }
}

// 新模型标签到 ArmorName 的映射：
// 新标签顺序为：1,2,3,4,5,outpost,sentry,base,negative
// 项目枚举顺序为：one,two,three,four,five,sentry,outpost,base,not_armor
inline ArmorName map_label_to_armor_name(int idx) {
  static const ArmorName kLabelMap[9] = {
      ArmorName::one,      // 0 -> 1
      ArmorName::two,      // 1 -> 2
      ArmorName::three,    // 2 -> 3
      ArmorName::four,     // 3 -> 4
      ArmorName::five,     // 4 -> 5
      ArmorName::outpost,  // 5 -> outpost
      ArmorName::sentry,   // 6 -> sentry
      ArmorName::base,     // 7 -> base
      ArmorName::not_armor // 8 -> negative
  };
  if (idx < 0 || idx >= 9)
    return ArmorName::not_armor;
  return kLabelMap[idx];
}
} // namespace

// Classifier 类构造函数，从配置文件加载模型
Classifier::Classifier(const std::string &config_path) {
  auto yaml = YAML::LoadFile(config_path);               // 加载 YAML 配置文件
  auto model = yaml["classify_model"].as<std::string>(); // 获取模型路径
  net_ = cv::dnn::readNetFromONNX(model); // 使用 OpenCV 加载 ONNX 模型
  auto ovmodel = core_.read_model(model); // 使用 OpenVINO 加载模型
  compiled_model_ = core_.compile_model(  // 编译模型以优化推理
      ovmodel, "AUTO",
      ov::hint::performance_mode(
          ov::hint::PerformanceMode::LATENCY)); // 设置为低延迟模式
}

// 使用 OpenCV DNN 进行分类
void Classifier::classify(Armor &armor) {
  if (armor.pattern.empty()) {         // 检查输入图像是否为空
    armor.name = ArmorName::not_armor; // 设置为非装甲
    if (dump_enabled()) {              // 若调试启用
      cv::Mat empty;                   // 创建空矩阵
      dump_images_and_meta(armor.pattern, empty, empty, "empty", 0, 0, 0.0, -1,
                           0.0); // 保存调试数据
    }
    return; // 提前返回
  }
  // 预处理：灰度 -> OTSU 二值化 -> 保持纵横比缩放并居中到 28x28（单通道）
  cv::Mat gray;                                          // 定义灰度图像
  cv::cvtColor(armor.pattern, gray, cv::COLOR_BGR2GRAY); // 转换为灰度图
  // 1. **直方图均衡化**：增强图像对比度
  cv::Mat equalized_gray;
  cv::equalizeHist(gray, equalized_gray);
  cv::Mat bin;
  cv::threshold(equalized_gray, bin, 0, 255,
                cv::THRESH_BINARY | cv::THRESH_OTSU); // OTSU 二值化
  if (invert_enabled()) cv::bitwise_not(bin, bin);

  // 中央竖条裁剪并缩放到 28x28
  // 参考实现中的数字 ROI 宽:高约为 20:28，这里不再依赖模式宽度比例，而是按图像高度推算宽度，避免不同ROI几何导致的过窄/过宽
  int crop_w = std::max(5, static_cast<int>(std::round(bin.rows * (20.0 / 28.0))))
               ;
  crop_w = std::min(crop_w, bin.cols);
  int x0 = (bin.cols - crop_w) / 2;
  cv::Rect crop_roi(x0, 0, crop_w, bin.rows);
  cv::Mat number_img = bin(crop_roi);

  cv::Mat input;
  cv::resize(number_img, input, cv::Size(28, 28), 0, 0, cv::INTER_NEAREST);

  if (dump_enabled()) {
    dump_images_and_meta(armor.pattern, bin, input, "cvdnn28", crop_w, bin.rows, 1.0, -1,
                         0.0, x0, 0);
  }

  // 归一化到 [0,1]：先转 float 再生成 blob
  cv::Mat input_f = input / 255.0;
  cv::Mat blob;
  cv::dnn::blobFromImage(input_f, blob);

  net_.setInput(blob);              // 设置网络输入
  cv::Mat outputs = net_.forward(); // 前向推理

  double confidence;     // 定义置信度
  cv::Point label_point; // 定义标签点
  cv::minMaxLoc(outputs.reshape(1, 1), nullptr, &confidence, nullptr,
                &label_point);  // 找到最大置信度和标签
  int label_id = label_point.x; // 获取标签 ID

  armor.confidence = confidence;                  // 设置装甲置信度
  armor.name = map_label_to_armor_name(label_id); // 标签映射到项目枚举

  if (dump_enabled()) { // 若调试启用
    dump_images_and_meta(armor.pattern, bin, input, "cvdnn28_post", crop_w, bin.rows, 1.0,
                         label_id, confidence);
  }
}

// 使用 OpenVINO 进行分类（优化版本）
void Classifier::ovclassify(Armor &armor) {
  if (armor.pattern.empty()) {         // 检查输入图像是否为空
    armor.name = ArmorName::not_armor; // 设置为非装甲
    if (dump_enabled()) {              // 若调试启用
      cv::Mat empty;                   // 创建空矩阵
      dump_images_and_meta(armor.pattern, empty, empty, "empty", 0, 0, 0.0, -1,
                           0.0); // 保存调试数据
    }
    return; // 提前返回
  }

  // 预处理：灰度 -> 直方图均衡化 -> OTSU 二值化 -> 中央竖条裁剪 -> 直接缩放到 28x28
  cv::Mat gray;                                          // 定义灰度图像
  cv::cvtColor(armor.pattern, gray, cv::COLOR_BGR2GRAY); // 转换为灰度图
  cv::Mat equalized_gray;
  cv::equalizeHist(gray, equalized_gray);
  cv::Mat bin;
  cv::threshold(equalized_gray, bin, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
  if (invert_enabled()) cv::bitwise_not(bin, bin);

  // 使用固定的 20:28 宽高比按高度推算裁剪宽度，减少不同ROI比例差异带来的扭曲
  int crop_w = std::max(5, static_cast<int>(std::round(bin.rows * (20.0 / 28.0))));
  crop_w = std::min(crop_w, bin.cols);
  int x0 = (bin.cols - crop_w) / 2;
  cv::Rect crop_roi(x0, 0, crop_w, bin.rows);
  cv::Mat number_img = bin(crop_roi);

  cv::Mat input_u8;
  cv::resize(number_img, input_u8, cv::Size(28, 28), 0, 0, cv::INTER_NEAREST);

  // 归一化到 [0, 1]
  cv::Mat input_f = input_u8 / 255.0f;

  ov::Tensor input_tensor(ov::element::f32, {1, 1, 28, 28}, input_f.data); // 创建 OpenVINO 输入张量

  ov::InferRequest infer_request =
      compiled_model_.create_infer_request();   // 创建推理请求
  infer_request.set_input_tensor(input_tensor); // 设置输入张量
  infer_request.infer();                        // 执行推理

  auto output_tensor = infer_request.get_output_tensor(); // 获取输出张量
  auto output_shape = output_tensor.get_shape();       // 获取输出形状（未使用）
  cv::Mat outputs(1, 9, CV_32F, output_tensor.data()); // 创建输出矩阵（9 类）

  double confidence;     // 定义置信度
  cv::Point label_point; // 定义标签点
  cv::minMaxLoc(outputs.reshape(1, 1), nullptr, &confidence, nullptr,
                &label_point);  // 找到最大置信度和标签
  int label_id = label_point.x; // 获取标签 ID

  armor.confidence = confidence;                  // 设置装甲置信度
  armor.name = map_label_to_armor_name(label_id); // 标签映射到项目枚举

  if (dump_enabled()) { // 若调试启用
    // 这里的 w/h/scale 分别记录裁剪宽度、原始高度、scale=1
    dump_images_and_meta(armor.pattern, bin, input_u8, "ov28_post", crop_w, bin.rows, 1.0,
                         label_id, confidence, x0, 0); // 保存调试数据
  }
}

} // namespace auto_aim