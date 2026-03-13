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
#include <cmath>
#include <cstring>
#include <vector>
// OpenCV
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
// 3rd party
#include <fmt/format.h>

namespace fyt::auto_aim {

// 回调为 9 个类别，匹配 22 维度的输出
const std::vector<std::string> ModelDetector::NUMBER_NAMES = {
    "sentry",    // G  (哨兵)
    "1",         // 1  (一号)
    "2",         // 2  (二号)
    "3",         // 3  (三号)
    "4",         // 4  (四号)
    "5",         // 5  (五号)
    "outpost",   // O  (前哨站)
    "base",      // Bs (基地)
    "negative",  // negative
};

const std::vector<std::string> ModelDetector::COLOR_NAMES = {
    "red",
    "blue",
    "gray",
    "purple",
};

ModelDetector::ModelDetector(const std::string &model_path,
                             float confidence_threshold,
                             float nms_threshold,
                             EnemyColor detect_color)
: detect_color(detect_color),
  confidence_threshold(confidence_threshold),
  nms_threshold(nms_threshold) {
  // Load model
  auto model = core_.read_model(model_path);

  // Get input shape [1, 3, H, W]
  auto input_shape = model->input().get_shape();
  input_h_ = static_cast<int>(input_shape[2]);
  input_w_ = static_cast<int>(input_shape[3]);

  // Set up PrePostProcessor for automatic preprocessing
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

  // Build model with preprocessing
  model = ppp.build();

  // Compile model for CPU
  compiled_model_ = core_.compile_model(model, "CPU");
  infer_request_ = compiled_model_.create_infer_request();
}

cv::Mat ModelDetector::letterbox(const cv::Mat &src,
                                 float &scale,
                                 float &pad_x,
                                 float &pad_y) {
  float scale_x = static_cast<float>(input_w_) / static_cast<float>(src.cols);
  float scale_y = static_cast<float>(input_h_) / static_cast<float>(src.rows);
  scale = std::min(scale_x, scale_y);

  int new_w = static_cast<int>(src.cols * scale);
  int new_h = static_cast<int>(src.rows * scale);
  pad_x = (input_w_ - new_w) / 2.0f;
  pad_y = (input_h_ - new_h) / 2.0f;

  cv::Mat resized;
  cv::resize(src, resized, cv::Size(new_w, new_h));

  cv::Mat padded(input_h_, input_w_, CV_8UC3, cv::Scalar(114, 114, 114));
  resized.copyTo(
      padded(cv::Rect(static_cast<int>(pad_x), static_cast<int>(pad_y), new_w, new_h)));

  return padded;
}

// Sigmoid function
static float sigmoid(float x) {
  return 1.0f / (1.0f + std::exp(-x));
}

std::vector<Armor> ModelDetector::detect(const cv::Mat &input) {
  // 1. Preprocess: letterbox
  float scale, pad_x, pad_y;
  cv::Mat padded = letterbox(input, scale, pad_x, pad_y);

  // 2. Set input tensor
  auto input_tensor = infer_request_.get_input_tensor();
  uchar *input_data = input_tensor.data<uchar>();
  std::memcpy(input_data, padded.data, padded.total() * padded.elemSize());

  // 3. Run inference
  infer_request_.infer();

  // 4. Get output tensor
  auto output_tensor = infer_request_.get_output_tensor();
  auto output_shape = output_tensor.get_shape();
  const float *output_data = output_tensor.data<float>();

  // Determine the format and get number of candidates
  int num_candidates;
  bool need_transpose;
  if (output_shape.size() == 3) {
    if (output_shape[1] == NUM_OUTPUT_VALUES) {
      // Shape: [1, 22, N]
      num_candidates = static_cast<int>(output_shape[2]);
      need_transpose = true;
    } else {
      // Shape: [1, N, 22]
      num_candidates = static_cast<int>(output_shape[1]);
      need_transpose = false;
    }
  } else {
    armors_.clear();
    return armors_;
  }

  // Transpose if needed: [1, 22, N] -> [N, 22]
  const float *data_ptr;
  if (need_transpose) {
    // 使用类成员变量避免重复分配内存
    transposed_buffer_.resize(num_candidates * NUM_OUTPUT_VALUES);
    for (int i = 0; i < NUM_OUTPUT_VALUES; i++) {
      for (int j = 0; j < num_candidates; j++) {
        transposed_buffer_[j * NUM_OUTPUT_VALUES + i] = output_data[i * num_candidates + j];
      }
    }
    data_ptr = transposed_buffer_.data();
  } else {
    data_ptr = output_data;
  }

  // 5. Postprocess (传入 scale 和 padding 进行准确反算)
  armors_ = postprocess(data_ptr, num_candidates, scale, pad_x, pad_y);

  return armors_;
}

std::vector<Armor> ModelDetector::postprocess(const float *output_data,
                                              int num_candidates,
                                              float scale,
                                              float pad_x,
                                              float pad_y) {
  std::vector<Armor> armors;
  std::vector<cv::Rect2d> boxes;
  std::vector<float> scores;
  std::vector<int> indices;

  // Temporary storage for all valid detections before NMS
  struct Detection {
    cv::Point2f keypoints[4];
    float confidence;
    int color_id;
    int number_id;
    float number_confidence;
  };
  std::vector<Detection> detections;

  for (int i = 0; i < num_candidates; i++) {
    const float *row = output_data + i * NUM_OUTPUT_VALUES;

    // 1. Objectness score (index 8)
    float objectness = sigmoid(row[8]);
    if (objectness < confidence_threshold) {
      continue;
    }

    // 2. Color class (indices 9-12): 0=Blue, 1=Red, 2=Gray, 3=Purple
    int color_id = 0;
    float max_color_prob = row[9];
    for (int c = 1; c < NUM_COLORS; c++) {
      if (row[9 + c] > max_color_prob) {
        max_color_prob = row[9 + c];
        color_id = c;
      }
    }

    // Filter
    if (color_id >= 2) continue;
    if (detect_color == EnemyColor::RED && color_id != 0) continue;
    if (detect_color == EnemyColor::BLUE && color_id != 1) continue;

    // 3. Number class (indices 13-21)
    int number_id = 0;
    float max_num_prob = row[13];
    for (int n = 1; n < NUM_NUMBERS; n++) {
      if (row[13 + n] > max_num_prob) {
        max_num_prob = row[13 + n];
        number_id = n;
      }
    }

    float class_prob = sigmoid(max_num_prob);
    float final_score = objectness * class_prob;
    if (final_score < confidence_threshold) {
      continue;
    }

    // 4. Decode keypoints and map back to original image coordinates
    cv::Point2f kps[4];
    float min_x = 1e5f, min_y = 1e5f;
    float max_x = -1e5f, max_y = -1e5f;
    for (int k = 0; k < 4; k++) {
      // 修复：先减去 padding 再除以 scale
      kps[k].x = (row[k * 2] - pad_x) / scale;
      kps[k].y = (row[k * 2 + 1] - pad_y) / scale;
      
      min_x = std::min(min_x, kps[k].x);
      min_y = std::min(min_y, kps[k].y);
      max_x = std::max(max_x, kps[k].x);
      max_y = std::max(max_y, kps[k].y);
    }

    boxes.emplace_back(min_x, min_y, max_x - min_x, max_y - min_y);
    scores.push_back(final_score);

    Detection det;
    std::copy(kps, kps + 4, det.keypoints);
    det.confidence = final_score;
    det.color_id = color_id;
    det.number_id = number_id;
    det.number_confidence = class_prob;
    detections.push_back(det);
  }

  // Apply NMS
  if (!boxes.empty()) {
    cv::dnn::NMSBoxes(boxes, scores, confidence_threshold, nms_threshold, indices);
  }

  // Build Armor objects
  for (int idx : indices) {
    const auto &det = detections[idx];

    Light left_light;
    left_light.top = det.keypoints[0];
    left_light.bottom = det.keypoints[1];
    left_light.center = (left_light.top + left_light.bottom) / 2;
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
    right_light.center = (right_light.top + right_light.bottom) / 2;
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

    float width = (cv::norm(det.keypoints[3] - det.keypoints[0]) +
                   cv::norm(det.keypoints[2] - det.keypoints[1])) /
                  2.0f;
    float height = (cv::norm(det.keypoints[1] - det.keypoints[0]) +
                    cv::norm(det.keypoints[2] - det.keypoints[3])) /
                   2.0f;
    float aspect_ratio = (height > 0) ? (width / height) : 0;
    armor.type = aspect_ratio > 3.2f ? ArmorType::LARGE : ArmorType::SMALL;

    armor.number = NUMBER_NAMES[det.number_id];
    armor.confidence = det.number_confidence;
    armor.classfication_result =
        fmt::format("{}:{:.1f}%", armor.number, armor.confidence * 100.0);

    if (armor.number == "negative") {
      continue;
    }

    armors.push_back(armor);
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