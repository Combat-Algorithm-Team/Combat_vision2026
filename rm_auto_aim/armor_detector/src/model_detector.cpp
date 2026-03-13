// Copyright (C) FYT Vision Group. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "armor_detector/model_detector.hpp"
// std
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>
// OpenCV
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
// 3rd party
#include <fmt/format.h>

namespace fyt::auto_aim {

const std::vector<std::string> ModelDetector::NUMBER_NAMES = {
    "sentry", "1", "2", "3", "4", "5", "outpost", "base", "negative",
};

const std::vector<std::string> ModelDetector::COLOR_NAMES = {
    "red",
    "blue",
    "gray",
    "purple",
};

static inline float fast_sigmoid(float x) {
  x = std::max(-10.0f, std::min(10.0f, x));
  return 1.0f / (1.0f + std::exp(-x));
}

ModelDetector::ModelDetector(const std::string &model_path,
                             float confidence_threshold,
                             float nms_threshold,
                             EnemyColor detect_color)
: detect_color(detect_color),
  confidence_threshold(confidence_threshold),
  nms_threshold(nms_threshold) {
  auto model = core_.read_model(model_path);

  auto input_shape = model->input().get_shape();
  input_h_ = static_cast<int>(input_shape[2]);
  input_w_ = static_cast<int>(input_shape[3]);

  ov::preprocess::PrePostProcessor ppp(model);
  ppp.input().tensor()
      .set_element_type(ov::element::u8)
      .set_layout("NHWC")
      .set_color_format(ov::preprocess::ColorFormat::BGR);
  ppp.input().preprocess()
      .convert_element_type(ov::element::f32)
      .convert_color(ov::preprocess::ColorFormat::RGB)
      .scale({255.0f, 255.0f, 255.0f});
  ppp.input().model().set_layout("NCHW");
  ppp.output().tensor().set_element_type(ov::element::f32);
  model = ppp.build();

  compiled_model_ = core_.compile_model(
      model, "CPU",
      ov::AnyMap{
        {ov::hint::performance_mode.name(), ov::hint::PerformanceMode::THROUGHPUT},
        {ov::streams::num.name(), "1"},
        {ov::inference_num_threads.name(), 6},  // 按你CPU试 4/6/8
        {ov::hint::enable_cpu_pinning.name(), true},
      });

  infer_request_ = compiled_model_.create_infer_request();
}

cv::Mat ModelDetector::letterbox(const cv::Mat &src,
                                 float &scale,
                                 float &pad_x,
                                 float &pad_y) {
  const float scale_x = static_cast<float>(input_w_) / static_cast<float>(src.cols);
  const float scale_y = static_cast<float>(input_h_) / static_cast<float>(src.rows);
  scale = std::min(scale_x, scale_y);

  const int new_w = static_cast<int>(src.cols * scale);
  const int new_h = static_cast<int>(src.rows * scale);
  pad_x = (input_w_ - new_w) * 0.5f;
  pad_y = (input_h_ - new_h) * 0.5f;

  cv::Mat resized;
  cv::resize(src, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);

  cv::Mat padded(input_h_, input_w_, CV_8UC3, cv::Scalar(114, 114, 114));
  resized.copyTo(
      padded(cv::Rect(static_cast<int>(pad_x), static_cast<int>(pad_y), new_w, new_h)));

  return padded;
}

std::vector<Armor> ModelDetector::detect(const cv::Mat &input) {
  // 1) Preprocess
  float scale = 1.0f, pad_x = 0.0f, pad_y = 0.0f;
  cv::Mat padded = letterbox(input, scale, pad_x, pad_y);

  // 2) Input tensor copy
  auto input_tensor = infer_request_.get_input_tensor();
  auto *input_data = input_tensor.data<uchar>();
  std::memcpy(input_data, padded.data, padded.total() * padded.elemSize());

  // 3) Inference
  infer_request_.infer();

  // 4) Output parse
  auto output_tensor = infer_request_.get_output_tensor();
  const auto output_shape = output_tensor.get_shape();
  const float *output_data = output_tensor.data<float>();

  int num_candidates = 0;
  bool layout_1_22_N = false;

  if (output_shape.size() == 3) {
    if (output_shape[1] == NUM_OUTPUT_VALUES) {
      // [1, 22, N]
      num_candidates = static_cast<int>(output_shape[2]);
      layout_1_22_N = true;
    } else {
      // [1, N, 22]
      num_candidates = static_cast<int>(output_shape[1]);
      layout_1_22_N = false;
    }
  } else {
    armors_.clear();
    return armors_;
  }

  // 5) Postprocess (no transpose copy)
  armors_ = postprocess(output_data, num_candidates, scale, pad_x, pad_y, layout_1_22_N);
  return armors_;
}

std::vector<Armor> ModelDetector::postprocess(const float *output_data,
                                              int num_candidates,
                                              float scale,
                                              float pad_x,
                                              float pad_y,
                                              bool layout_1_22_N) {
  std::vector<Armor> armors;
  armors.reserve(64);

  std::vector<cv::Rect2d> boxes;
  std::vector<float> scores;
  std::vector<int> indices;
  boxes.reserve(num_candidates);
  scores.reserve(num_candidates);
  indices.reserve(num_candidates);

  struct Detection {
    std::array<cv::Point2f, 4> keypoints;
    float confidence;
    int color_id;
    int number_id;
    float number_confidence;
  };
  std::vector<Detection> detections;
  detections.reserve(num_candidates);

  for (int i = 0; i < num_candidates; i++) {
    auto getv = [&](int j) -> float {
      return layout_1_22_N ? output_data[j * num_candidates + i]
                           : output_data[i * NUM_OUTPUT_VALUES + j];
    };

    // objectness
    const float objectness = fast_sigmoid(getv(8));
    if (objectness < confidence_threshold) continue;

    // color logits [9..12]
    int color_id = 0;
    float max_color_prob = getv(9);
    for (int c = 1; c < NUM_COLORS; c++) {
      const float v = getv(9 + c);
      if (v > max_color_prob) {
        max_color_prob = v;
        color_id = c;
      }
    }

    // color filter
    if (color_id >= 2) continue;  // gray/purple pass off
    if (detect_color == EnemyColor::RED && color_id != 0) continue;
    if (detect_color == EnemyColor::BLUE && color_id != 1) continue;

    // number logits [13..21]
    int number_id = 0;
    float max_num_prob = getv(13);
    for (int n = 1; n < NUM_NUMBERS; n++) {
      const float v = getv(13 + n);
      if (v > max_num_prob) {
        max_num_prob = v;
        number_id = n;
      }
    }

    const float class_prob = fast_sigmoid(max_num_prob);
    const float final_score = objectness * class_prob;
    if (final_score < confidence_threshold) continue;

    // decode keypoints
    std::array<cv::Point2f, 4> kps;
    float min_x = 1e5f, min_y = 1e5f;
    float max_x = -1e5f, max_y = -1e5f;

    for (int k = 0; k < 4; k++) {
      const float x = (getv(k * 2) - pad_x) / scale;
      const float y = (getv(k * 2 + 1) - pad_y) / scale;
      kps[k] = {x, y};
      min_x = std::min(min_x, x);
      min_y = std::min(min_y, y);
      max_x = std::max(max_x, x);
      max_y = std::max(max_y, y);
    }

    boxes.emplace_back(min_x, min_y, max_x - min_x, max_y - min_y);
    scores.emplace_back(final_score);
    detections.push_back(Detection{kps, final_score, color_id, number_id, class_prob});
  }

  if (!boxes.empty()) {
    cv::dnn::NMSBoxes(boxes, scores, confidence_threshold, nms_threshold, indices);
  }

  armors.reserve(indices.size());
  for (int idx : indices) {
    const auto &det = detections[idx];

    Light left_light;
    left_light.top = det.keypoints[0];
    left_light.bottom = det.keypoints[1];
    left_light.center = (left_light.top + left_light.bottom) * 0.5f;
    left_light.length = cv::norm(left_light.top - left_light.bottom);
    left_light.width = 0;
    if (left_light.length > 0) {
      left_light.axis = (left_light.top - left_light.bottom) /
                        static_cast<float>(left_light.length);
    }
    left_light.tilt_angle =
        std::atan2(std::abs(left_light.top.x - left_light.bottom.x),
                   std::abs(left_light.top.y - left_light.bottom.y)) /
        static_cast<float>(CV_PI) * 180.0f;
    left_light.color = det.color_id == 1 ? EnemyColor::RED : EnemyColor::BLUE;

    Light right_light;
    right_light.top = det.keypoints[3];
    right_light.bottom = det.keypoints[2];
    right_light.center = (right_light.top + right_light.bottom) * 0.5f;
    right_light.length = cv::norm(right_light.top - right_light.bottom);
    right_light.width = 0;
    if (right_light.length > 0) {
      right_light.axis = (right_light.top - right_light.bottom) /
                         static_cast<float>(right_light.length);
    }
    right_light.tilt_angle =
        std::atan2(std::abs(right_light.top.x - right_light.bottom.x),
                   std::abs(right_light.top.y - right_light.bottom.y)) /
        static_cast<float>(CV_PI) * 180.0f;
    right_light.color = det.color_id == 1 ? EnemyColor::RED : EnemyColor::BLUE;

    Armor armor(left_light, right_light);

    const float width = (cv::norm(det.keypoints[3] - det.keypoints[0]) +
                         cv::norm(det.keypoints[2] - det.keypoints[1])) * 0.5f;
    const float height = (cv::norm(det.keypoints[1] - det.keypoints[0]) +
                          cv::norm(det.keypoints[2] - det.keypoints[3])) * 0.5f;
    const float aspect_ratio = (height > 0) ? (width / height) : 0.0f;
    armor.type = aspect_ratio > 3.2f ? ArmorType::LARGE : ArmorType::SMALL;

    armor.number = NUMBER_NAMES[det.number_id];
    armor.confidence = det.number_confidence;
    armor.classfication_result =
        fmt::format("{}:{:.1f}%", armor.number, armor.confidence * 100.0f);

    if (armor.number == "negative") continue;
    armors.push_back(std::move(armor));
  }

  return armors;
}

void ModelDetector::drawResults(cv::Mat &img) const {
  for (const auto &armor : armors_) {
    cv::line(img, armor.left_light.top, armor.left_light.bottom,
             cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
    cv::line(img, armor.right_light.bottom, armor.right_light.top,
             cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
    cv::line(img, armor.left_light.top, armor.right_light.top,
             cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
    cv::line(img, armor.right_light.bottom, armor.left_light.bottom,
             cv::Scalar(0, 255, 0), 1, cv::LINE_AA);

    cv::circle(img, armor.left_light.top, 3, cv::Scalar(255, 0, 0), -1);
    cv::circle(img, armor.left_light.bottom, 3, cv::Scalar(0, 255, 0), -1);
    cv::circle(img, armor.right_light.bottom, 3, cv::Scalar(0, 0, 255), -1);
    cv::circle(img, armor.right_light.top, 3, cv::Scalar(255, 255, 0), -1);

    std::string text =
        fmt::format("{} {}", armorTypeToString(armor.type), armor.classfication_result);
    cv::putText(img, text, armor.left_light.top, cv::FONT_HERSHEY_SIMPLEX, 0.8,
                cv::Scalar(0, 255, 255), 2);
  }
}

}  // namespace fyt::auto_aim