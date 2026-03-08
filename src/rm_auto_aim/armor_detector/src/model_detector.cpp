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
#include <vector>
// OpenCV
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
// 3rd party
#include <fmt/format.h>

namespace fyt::auto_aim {

// Map model number class indices to the string names used in the rest of the system
const std::vector<std::string> ModelDetector::NUMBER_NAMES = {
    "sentry",    // G  (哨兵)
    "1",         // 1  (一号)
    "2",         // 2  (二号)
    "3",         // 3  (三号)
    "4",         // 4  (四号)
    "5",         // 5  (五号)
    "outpost",   // O  (前哨站)
    "base",      // Bs (基地)
    "base",      // Bb (基地大装甲)
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
  compiled_model_ = core_.compile_model(model, "CPU");
  infer_request_ = compiled_model_.create_infer_request();

  // Get input shape [1, 3, H, W]
  auto input_shape = compiled_model_.input().get_shape();
  input_h_ = static_cast<int>(input_shape[2]);
  input_w_ = static_cast<int>(input_shape[3]);
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

std::vector<Armor> ModelDetector::detect(const cv::Mat &input) noexcept {
  // 1. Preprocess: letterbox + normalize + HWC->CHW
  float scale, pad_x, pad_y;
  cv::Mat padded = letterbox(input, scale, pad_x, pad_y);

  // Convert to float and normalize to [0, 1]
  cv::Mat blob;
  cv::dnn::blobFromImage(padded, blob, 1.0 / 255.0, cv::Size(), cv::Scalar(), true, false);

  // 2. Set input tensor
  auto input_tensor = infer_request_.get_input_tensor();
  auto input_shape = input_tensor.get_shape();
  float *input_data = input_tensor.data<float>();
  std::memcpy(input_data, blob.ptr<float>(), blob.total() * sizeof(float));

  // 3. Run inference
  infer_request_.infer();

  // 4. Get output tensor
  auto output_tensor = infer_request_.get_output_tensor();
  auto output_shape = output_tensor.get_shape();
  const float *output_data = output_tensor.data<float>();

  // Output shape can be [1, NUM_OUTPUT_VALUES, N] or [1, N, NUM_OUTPUT_VALUES]
  // Determine the format and get number of candidates
  int num_candidates;
  bool need_transpose;
  if (output_shape.size() == 3) {
    if (output_shape[1] == NUM_OUTPUT_VALUES) {
      // Shape: [1, 23, N]
      num_candidates = static_cast<int>(output_shape[2]);
      need_transpose = true;
    } else {
      // Shape: [1, N, 23]
      num_candidates = static_cast<int>(output_shape[1]);
      need_transpose = false;
    }
  } else {
    armors_.clear();
    return armors_;
  }

  // Transpose if needed: [1, 23, N] -> [N, 23]
  std::vector<float> transposed;
  const float *data_ptr;
  if (need_transpose) {
    transposed.resize(num_candidates * NUM_OUTPUT_VALUES);
    for (int i = 0; i < NUM_OUTPUT_VALUES; i++) {
      for (int j = 0; j < num_candidates; j++) {
        transposed[j * NUM_OUTPUT_VALUES + i] = output_data[i * num_candidates + j];
      }
    }
    data_ptr = transposed.data();
  } else {
    data_ptr = output_data;
  }

  // 5. Postprocess
  armors_ = postprocess(data_ptr, num_candidates, scale, pad_x, pad_y);

  return armors_;
}

std::vector<Armor> ModelDetector::postprocess(const float *output_data,
                                              int num_candidates,
                                              float scale,
                                              float pad_x,
                                              float pad_y) {
  std::vector<Armor> armors;
  std::vector<cv::Rect2f> boxes;
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

    // Check confidence
    float conf = row[8];
    if (conf < confidence_threshold) {
      continue;
    }

    // Decode keypoints and map back to original image coordinates
    cv::Point2f kps[4];
    for (int k = 0; k < 4; k++) {
      kps[k].x = (row[k * 2] - pad_x) / scale;
      kps[k].y = (row[k * 2 + 1] - pad_y) / scale;
    }

    // Get color class (argmax of indices 9-12)
    int color_id = 0;
    float max_color_prob = row[9];
    for (int c = 1; c < NUM_COLORS; c++) {
      if (row[9 + c] > max_color_prob) {
        max_color_prob = row[9 + c];
        color_id = c;
      }
    }

    // Filter by color: only keep red(0) or blue(1) matching detect_color
    if (color_id == 0 && detect_color != EnemyColor::RED) continue;
    if (color_id == 1 && detect_color != EnemyColor::BLUE) continue;
    if (color_id >= 2) continue;  // gray/purple: skip

    // Get number class (argmax of indices 13-22)
    int number_id = 0;
    float max_num_prob = row[13];
    for (int n = 1; n < NUM_NUMBERS; n++) {
      if (row[13 + n] > max_num_prob) {
        max_num_prob = row[13 + n];
        number_id = n;
      }
    }

    // Compute bounding box from keypoints for NMS
    float min_x = std::min({kps[0].x, kps[1].x, kps[2].x, kps[3].x});
    float min_y = std::min({kps[0].y, kps[1].y, kps[2].y, kps[3].y});
    float max_x = std::max({kps[0].x, kps[1].x, kps[2].x, kps[3].x});
    float max_y = std::max({kps[0].y, kps[1].y, kps[2].y, kps[3].y});

    boxes.emplace_back(min_x, min_y, max_x - min_x, max_y - min_y);
    scores.push_back(conf);

    Detection det;
    std::copy(kps, kps + 4, det.keypoints);
    det.confidence = conf;
    det.color_id = color_id;
    det.number_id = number_id;
    det.number_confidence = max_num_prob;
    detections.push_back(det);
  }

  // Apply NMS
  if (!boxes.empty()) {
    cv::dnn::NMSBoxes(boxes, scores, confidence_threshold, nms_threshold, indices);
  }

  // Build Armor objects from NMS results
  for (int idx : indices) {
    const auto &det = detections[idx];

    // Keypoint order: top-left(0), bottom-left(1), bottom-right(2), top-right(3)
    // Map to Light structure:
    //   left_light:  top = kp[0], bottom = kp[1]
    //   right_light: top = kp[3], bottom = kp[2]
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
    left_light.color = det.color_id == 0 ? EnemyColor::RED : EnemyColor::BLUE;

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
    right_light.color = det.color_id == 0 ? EnemyColor::RED : EnemyColor::BLUE;

    Armor armor(left_light, right_light);

    // Determine armor type from keypoint geometry
    float width = (cv::norm(det.keypoints[3] - det.keypoints[0]) +
                   cv::norm(det.keypoints[2] - det.keypoints[1])) /
                  2.0f;
    float height = (cv::norm(det.keypoints[1] - det.keypoints[0]) +
                    cv::norm(det.keypoints[2] - det.keypoints[3])) /
                   2.0f;
    float aspect_ratio = (height > 0) ? (width / height) : 0;
    // LARGE: 225/50 = 4.5, SMALL: 130/50 = 2.6, threshold ~3.2
    armor.type = aspect_ratio > 3.2f ? ArmorType::LARGE : ArmorType::SMALL;

    // Set number classification result
    armor.number = NUMBER_NAMES[det.number_id];
    armor.confidence = det.number_confidence;
    armor.classfication_result =
        fmt::format("{}:{:.1f}%", armor.number, armor.confidence * 100.0);

    // Skip "negative" class
    if (armor.number == "negative") {
      continue;
    }

    armors.push_back(armor);
  }

  return armors;
}

void ModelDetector::drawResults(cv::Mat &img) const noexcept {
  for (const auto &armor : armors_) {
    // Draw armor quadrilateral
    cv::line(img, armor.left_light.top, armor.left_light.bottom,
             cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
    cv::line(img, armor.right_light.bottom, armor.right_light.top,
             cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
    cv::line(img, armor.left_light.top, armor.right_light.top,
             cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
    cv::line(img, armor.right_light.bottom, armor.left_light.bottom,
             cv::Scalar(0, 255, 0), 1, cv::LINE_AA);

    // Draw keypoints
    cv::circle(img, armor.left_light.top, 3, cv::Scalar(255, 0, 0), -1);
    cv::circle(img, armor.left_light.bottom, 3, cv::Scalar(0, 255, 0), -1);
    cv::circle(img, armor.right_light.bottom, 3, cv::Scalar(0, 0, 255), -1);
    cv::circle(img, armor.right_light.top, 3, cv::Scalar(255, 255, 0), -1);

    // Show classification result
    std::string text =
        fmt::format("{} {}", armorTypeToString(armor.type), armor.classfication_result);
    cv::putText(img, text, armor.left_light.top, cv::FONT_HERSHEY_SIMPLEX, 0.8,
                cv::Scalar(0, 255, 255), 2);
  }
}

}  // namespace fyt::auto_aim
