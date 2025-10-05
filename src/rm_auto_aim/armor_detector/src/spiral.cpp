#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <vector>
// ros2
#include <cv_bridge/cv_bridge.h>
#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2/convert.h>
#include <tf2/time.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/create_timer_ros.h>

#include <image_transport/image_transport.hpp>
#include <rclcpp/qos.hpp>
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>
// third party
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
// project
#include "armor_detector/bound_solver.hpp"
#include "armor_detector/armor_detector.hpp"
#include "armor_detector/ba_solver.hpp"
#include "armor_detector/armor_pose_estimator.hpp"
#include "armor_detector/types.hpp"
#include "rm_utils/assert.hpp"
#include "rm_utils/common.hpp"
#include "rm_utils/logger/log.hpp"
#include "rm_utils/math/pnp_solver.hpp"
#include "rm_utils/math/utils.hpp"
#include "rm_utils/url_resolver.hpp"
#include "rm_interfaces/msg/armors.hpp"




// camera_matrix:
//   rows: 3
//   cols: 3
//   data: [1320.127401, 0.000000, 609.902940, 0.000000, 1329.050651, 457.308236, 0.000000, 0.000000, 1.000000]

//   distortion_coefficients:
//   rows: 1
//   cols: 5
//   data: [-0.034135, 0.131210, -0.015866, -0.004433, 0.000000]
using namespace fyt;
using namespace fyt::auto_aim;
int main(){
  bool use_ba_ = true, use_bound_ = false;
  auto camera_info = std::make_shared<sensor_msgs::msg::CameraInfo>();
  camera_info->k = {3459.981717, 0.000000, 1523.076759,
        0.000000, 3469.145301, 1019.198285,
        0.000000, 0.000000, 1.000000};
  camera_info->d = {-0.091955, 0.395148, 0.003058, -0.000456, 0.000000};
  auto armor_pose_estimator_ = std::make_unique<ArmorPoseEstimator>(camera_info);
  armor_pose_estimator_->enableBA(use_ba_);
  armor_pose_estimator_->enableBound(use_bound_);
  Eigen::Matrix3d imu_to_camera_ = Eigen::Matrix3d::Identity();
  imu_to_camera_ << 2.22045e-16, 2.22045e-16, 1 
                 , -1, 0, 2.22045e-16
                 , 0, -1, 2.22045e-16;

  // auto pnp_solver_ = std::make_unique<PnPSolver>(camera_info->k, camera_info->d);
  // pnp_solver_->setObjectPoints(
  //     "small", Armor::buildObjectPoints<cv::Point3f>(SMALL_ARMOR_WIDTH,
  //                                                    SMALL_ARMOR_HEIGHT));
  // pnp_solver_->setObjectPoints(
  //     "large", Armor::buildObjectPoints<cv::Point3f>(LARGE_ARMOR_WIDTH,
  //                                                    LARGE_ARMOR_HEIGHT));

  std::string folder_path = "./src/rm_auto_aim/armor_detector/photos";
  std::vector<Armor> armors;
  std::vector<cv::Mat> images;
  EnemyColor color = EnemyColor::BLUE;
  int binary_thres = 160;
  Detector::LightParams l_params = {
      .min_ratio = 0.08,
      .max_ratio = 0.4,
      .max_angle = 40.0,
      .color_diff_thresh =25};

  Detector::ArmorParams a_params = {
      .min_light_ratio = 0.6,
      .min_small_center_distance = 0.8,
      .max_small_center_distance = 3.2,
      .min_large_center_distance = 3.2,
      .max_large_center_distance = 5.0,
      .max_angle = 35.0};

  auto detector_ = std::make_unique<Detector>(binary_thres, color, l_params, a_params);
  // std::cout << "当前工作目录: " << std::filesystem::current_path() << std::endl;
  images = readImages(folder_path);
  cv::namedWindow("armor",cv::WINDOW_NORMAL);
  
  for(auto img : images)
  {
  resizeWindow("armor", img, 50);
  armor_pose_estimator_->bound_solver_->detectBound(img);
  armors = detector_->detect(img);
  // detector_->drawResults(img);
  
  rm_interfaces::msg::Armors armors_msg_;

  armors_msg_.armors = armor_pose_estimator_->extractArmorPoses(armors, imu_to_camera_);

  // armor_pose_estimator_->bound_solver_->drawProjection(img);
  // armor_pose_estimator_->bound_solver_->drawBound(img);
  // cv::imshow("armor", img);
  // while(true){
  //   int key = cv::waitKey(0);
  //   if(key == 27)
  //   break;
  // }
  }
  return 0;
}


