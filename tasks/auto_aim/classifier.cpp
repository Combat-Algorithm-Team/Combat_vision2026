#include "classifier.hpp"

#include <yaml-cpp/yaml.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>

namespace auto_aim
{
namespace {
inline bool dump_enabled() {
  const char *env = std::getenv("CLASSIFIER_DEBUG_DUMP");
  return env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0 ||
                 std::strcmp(env, "TRUE") == 0 || std::strcmp(env, "on") == 0 ||
                 std::strcmp(env, "ON") == 0);
}

inline void ensure_logs_dir() {
  // 创建 logs 目录（若已存在则忽略错误）
  ::mkdir("logs", 0755);
}

inline int next_dump_id() {
  static int id = 0;
  return ++id;
}

inline void dump_images_and_meta(
  const cv::Mat &pattern, const cv::Mat &gray, const cv::Mat &input32,
  const char *stage, int w, int h, double scale, int label, double conf) {
  // 仅写到 logs/ 目录（项目已存在该目录），避免创建新目录带来的依赖
  ensure_logs_dir();
  const int id = next_dump_id();
  char base[256];
  std::snprintf(base, sizeof(base), "logs/classifier_%06d_%s", id, stage ? stage : "stage");

  char p1[300], p2[300], p3[300], p4[300];
  std::snprintf(p1, sizeof(p1), "%s_pattern.jpg", base);
  std::snprintf(p2, sizeof(p2), "%s_gray.jpg", base);
  std::snprintf(p3, sizeof(p3), "%s_input32.png", base);
  std::snprintf(p4, sizeof(p4), "%s_meta.txt", base);

  if (!pattern.empty()) cv::imwrite(p1, pattern);
  if (!gray.empty()) cv::imwrite(p2, gray);
  if (!input32.empty()) cv::imwrite(p3, input32);

  FILE *fp = std::fopen(p4, "w");
  if (fp) {
    std::fprintf(fp, "w=%d\n", w);
    std::fprintf(fp, "h=%d\n", h);
    std::fprintf(fp, "scale=%f\n", scale);
    std::fprintf(fp, "label=%d\n", label);
    std::fprintf(fp, "confidence=%f\n", conf);
    std::fclose(fp);
  }
}
}  // namespace

Classifier::Classifier(const std::string & config_path)
{
  auto yaml = YAML::LoadFile(config_path);
  auto model = yaml["classify_model"].as<std::string>();
  net_ = cv::dnn::readNetFromONNX(model);
  auto ovmodel = core_.read_model(model);
  compiled_model_ = core_.compile_model(
    ovmodel, "AUTO", ov::hint::performance_mode(ov::hint::PerformanceMode::LATENCY));
}

void Classifier::classify(Armor & armor)
{
  if (armor.pattern.empty()) {
    armor.name = ArmorName::not_armor;
    if (dump_enabled()) {
      cv::Mat empty;
      dump_images_and_meta(armor.pattern, empty, empty, "empty", 0, 0, 0.0, -1, 0.0);
    }
    return;
  }

  cv::Mat gray;
  cv::cvtColor(armor.pattern, gray, cv::COLOR_BGR2GRAY);

  auto input = cv::Mat(32, 32, CV_8UC1, cv::Scalar(0));
  auto x_scale = static_cast<double>(32) / gray.cols;
  auto y_scale = static_cast<double>(32) / gray.rows;
  auto scale = std::min(x_scale, y_scale);
  auto h = static_cast<int>(gray.rows * scale);
  auto w = static_cast<int>(gray.cols * scale);

  if (h == 0 || w == 0) {
    armor.name = ArmorName::not_armor;
    if (dump_enabled()) {
      dump_images_and_meta(armor.pattern, gray, input, "cvdnn_invalid", w, h, scale, -1, 0.0);
    }
    return;
  }
  auto roi = cv::Rect(0, 0, w, h);
  cv::resize(gray, input(roi), {w, h});

  if (dump_enabled()) {
    dump_images_and_meta(armor.pattern, gray, input, "cvdnn", w, h, scale, -1, 0.0);
  }

  auto blob = cv::dnn::blobFromImage(input, 1.0 / 255.0, cv::Size(), cv::Scalar());

  net_.setInput(blob);
  cv::Mat outputs = net_.forward();

  // softmax
  float max = *std::max_element(outputs.begin<float>(), outputs.end<float>());
  cv::exp(outputs - max, outputs);
  float sum = cv::sum(outputs)[0];
  outputs /= sum;

  double confidence;
  cv::Point label_point;
  cv::minMaxLoc(outputs.reshape(1, 1), nullptr, &confidence, nullptr, &label_point);
  int label_id = label_point.x;

  armor.confidence = confidence;
  armor.name = static_cast<ArmorName>(label_id);

  if (dump_enabled()) {
    dump_images_and_meta(armor.pattern, gray, input, "cvdnn_post", w, h, scale, label_id, confidence);
  }
}

void Classifier::ovclassify(Armor & armor)
{
  if (armor.pattern.empty()) {
    armor.name = ArmorName::not_armor;
    if (dump_enabled()) {
      cv::Mat empty;
      dump_images_and_meta(armor.pattern, empty, empty, "empty", 0, 0, 0.0, -1, 0.0);
    }
    return;
  }

  cv::Mat gray;
  cv::cvtColor(armor.pattern, gray, cv::COLOR_BGR2GRAY);

  // Resize image to 32x32
  auto input = cv::Mat(32, 32, CV_8UC1, cv::Scalar(0));
  auto x_scale = static_cast<double>(32) / gray.cols;
  auto y_scale = static_cast<double>(32) / gray.rows;
  auto scale = std::min(x_scale, y_scale);
  auto h = static_cast<int>(gray.rows * scale);
  auto w = static_cast<int>(gray.cols * scale);

  if (h == 0 || w == 0) {
    armor.name = ArmorName::not_armor;
    if (dump_enabled()) {
      dump_images_and_meta(armor.pattern, gray, input, "ov_invalid", w, h, scale, -1, 0.0);
    }
    return;
  }

  auto roi = cv::Rect(0, 0, w, h);
  cv::resize(gray, input(roi), {w, h});
  cv::Mat input_u8 = input.clone();
  // Normalize the input image to [0, 1] range
  input.convertTo(input, CV_32F, 1.0 / 255.0);

  ov::Tensor input_tensor(ov::element::f32, {1, 1, 32, 32}, input.data);

  ov::InferRequest infer_request = compiled_model_.create_infer_request();
  infer_request.set_input_tensor(input_tensor);
  infer_request.infer();

  auto output_tensor = infer_request.get_output_tensor();
  auto output_shape = output_tensor.get_shape();
  cv::Mat outputs(1, 9, CV_32F, output_tensor.data());

  // Softmax
  float max = *std::max_element(outputs.begin<float>(), outputs.end<float>());
  cv::exp(outputs - max, outputs);
  float sum = cv::sum(outputs)[0];
  outputs /= sum;

  double confidence;
  cv::Point label_point;
  cv::minMaxLoc(outputs.reshape(1, 1), nullptr, &confidence, nullptr, &label_point);
  int label_id = label_point.x;

  armor.confidence = confidence;
  armor.name = static_cast<ArmorName>(label_id);

  if (dump_enabled()) {
    dump_images_and_meta(armor.pattern, gray, input_u8, "ov_post", w, h, scale, label_id, confidence);
  }
}

}  // namespace auto_aim