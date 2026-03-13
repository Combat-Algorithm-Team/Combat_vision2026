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

#ifndef ARMOR_DETECTOR_MODEL_DETECTOR_HPP_
#define ARMOR_DETECTOR_MODEL_DETECTOR_HPP_

// std
#include <string>
#include <vector>
// third party
#include <opencv2/core.hpp>
#include <openvino/openvino.hpp>
// project
#include "armor_detector/types.hpp"
#include "rm_utils/common.hpp"

namespace fyt::auto_aim {

// Armor detector based on deep learning model inference using OpenVINO
// Model output format per detection (22 values):
//   0-7:   4 keypoints (x1,y1,x2,y2,x3,y3,x4,y4), counterclockwise from top-left
//   8:     confidence (objectness logit)
//   9-12:  color class logits (red, blue, gray, purple)
//   13-21: number class logits (G,1,2,3,4,5,O,Bs,negative)
class ModelDetector {
public:
  ModelDetector(const std::string &model_path,
                float confidence_threshold,
                float nms_threshold,
                EnemyColor detect_color);

  std::vector<Armor> detect(const cv::Mat &input);
  void drawResults(cv::Mat &img) const;

  // Parameters
  EnemyColor detect_color;
  float confidence_threshold;
  float nms_threshold;

private:
  // Letterbox preprocessing: resize with aspect ratio preserved
  cv::Mat letterbox(const cv::Mat &src, float &scale, float &pad_x, float &pad_y);

  // Postprocess model output into Armor objects
  // layout_1_22_N = true  -> output shape [1, 22, N]
  // layout_1_22_N = false -> output shape [1, N, 22]
  std::vector<Armor> postprocess(const float *output_data,
                                 int num_candidates,
                                 float scale,
                                 float pad_x,
                                 float pad_y,
                                 bool layout_1_22_N);

  // OpenVINO inference members
  ov::Core core_;
  ov::CompiledModel compiled_model_;
  ov::InferRequest infer_request_;

  // Model input dimensions
  int input_h_;
  int input_w_;

  // Number of output values per detection
  static constexpr int NUM_KEYPOINT_VALUES = 8;    // 4 keypoints * 2
  static constexpr int NUM_CONFIDENCE = 1;
  static constexpr int NUM_COLORS = 4;             // red, blue, gray, purple
  static constexpr int NUM_NUMBERS = 9;            // G,1,2,3,4,5,O,Bs,negative
  static constexpr int NUM_OUTPUT_VALUES = NUM_KEYPOINT_VALUES + NUM_CONFIDENCE +
                                           NUM_COLORS + NUM_NUMBERS;  // 22

  // Label mappings (model class index -> string used in Armor::number)
  static const std::vector<std::string> NUMBER_NAMES;
  // Color label names
  static const std::vector<std::string> COLOR_NAMES;

  // Detected armors (for debug drawing)
  std::vector<Armor> armors_;
};

}  // namespace fyt::auto_aim

#endif  // ARMOR_DETECTOR_MODEL_DETECTOR_HPP_