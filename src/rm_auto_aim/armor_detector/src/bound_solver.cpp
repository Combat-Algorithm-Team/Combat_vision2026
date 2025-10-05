#include "armor_detector/bound_solver.hpp"

// std
#include <memory>
// g2o
#include <g2o/core/robust_kernel.h>
#include <g2o/core/robust_kernel_factory.h>
#include <g2o/core/robust_kernel_impl.h>
#include <g2o/types/slam3d/types_slam3d.h>
// 3rd party
#include <Eigen/Core>
#include <opencv2/core/eigen.hpp>
#include <sophus/se3.hpp>
#include <sophus/so3.hpp>
// project
#include "armor_detector/graph_optimizer.hpp"
#include "armor_detector/types.hpp"
#include "rm_utils/logger/log.hpp"
#include "rm_utils/math/utils.hpp"

namespace fyt::auto_aim{
BoundSolver::BoundSolver(sensor_msgs::msg::CameraInfo::SharedPtr camera_info){
  camera_matrix_ = cv::Mat(3, 3, CV_64F, const_cast<double*>(camera_info->k.data())).clone();
  distortion_coefficients_ = cv::Mat(1, camera_info->d.size(), CV_64F, 
  const_cast<double*>(camera_info->d.data())).clone();
  
  K_ = Eigen::Matrix3d::Identity();
  K_(0, 0) = camera_info->k[0];
  K_(1, 1) = camera_info->k[4];
  K_(0, 2) = camera_info->k[2];
  K_(1, 2) = camera_info->k[5];
  
  // Optimization information
  optimizer_.setVerbose(false);
  // Optimization method
  optimizer_.setAlgorithm(
      g2o::OptimizationAlgorithmFactory::instance()->construct(
          "lm_dense", solver_property_));
  // Initial step sizei
  lm_algorithm_ = dynamic_cast<g2o::OptimizationAlgorithmLevenberg *>(
      const_cast<g2o::OptimizationAlgorithm *>(optimizer_.algorithm()));
  lm_algorithm_->setUserLambdaInit(0.1);
}

Eigen::Matrix3d 
BoundSolver::solveBound(const Armor &armor, const Eigen::Vector3d &t_camera_armor,
                  const Eigen::Matrix3d &R_camera_armor,
                  const Eigen::Matrix3d &R_imu_camera){
  Eigen::Matrix3d R_imu_armor = R_imu_camera * R_camera_armor;
  Sophus::SO3d R_camera_imu = Sophus::SO3d(R_imu_camera.transpose());
  optimizer_.clear();

  // Compute the initial yaw from rotation matrix
  double initial_armor_yaw;
  auto theta_by_sin = std::asin(-R_imu_armor(0, 1));
  auto theta_by_cos = std::acos(R_imu_armor(1, 1));
  if (std::abs(theta_by_sin) > 1e-5) {
    initial_armor_yaw = theta_by_sin > 0 ? theta_by_cos : -theta_by_cos;
  } else { 
    initial_armor_yaw = R_imu_armor(1, 1) > 0 ? 0 : CV_PI;
  }
  // std::cout<<"initial yaw is:"<<initial_armor_yaw * 180 / 3.14;
  clock = initial_armor_yaw > 0 ? 1 : 0;
  // std::cout<<"initial angle is:"<<initial_armor_yaw * 180 / CV_PI<<std::endl;

  // Get the pitch angle of the armor
  double armor_pitch =
      armor.number == "outpost" ? -FIFTTEN_DEGREE_RAD : FIFTTEN_DEGREE_RAD;
  Sophus::SO3d R_pitch = Sophus::SO3d::exp(Eigen::Vector3d(0, armor_pitch, 0));

  const auto armor_size =
      armor.type == ArmorType::SMALL
          ? Eigen::Vector2d(SMALL_ARMOR_WIDTH, SMALL_ARMOR_HEIGHT)
          : Eigen::Vector2d(LARGE_ARMOR_WIDTH, LARGE_ARMOR_HEIGHT);
  const auto object_points = 
      Armor::buildBoundPoints<Eigen::Vector3d>(armor_size(0), armor_size(1), LIGHT_WIDTH,
                                               INTERNAL_DEEPTH, EXTERNAL_DEEPTH, clock);

  // reprojection(object_points, R_camera_armor, t_camera_armor);   //to get bound_solver->projected_points
  setImagePoints(clock);   //to get bound_solver->image_points
  size_t id_counter = 0;

  VertexYaw *v_yaw = new VertexYaw();
  v_yaw->setId(id_counter++);
  v_yaw->setEstimate(initial_armor_yaw);
  optimizer_.addVertex(v_yaw);

  const auto &landmarks = armor.landmarks();
  for (size_t i = 0; i < BOUNDS; i++) {
    g2o::VertexPointXYZ *v_point = new g2o::VertexPointXYZ();
    v_point->setId(id_counter++);
    v_point->setEstimate(Eigen::Vector3d(
        object_points[i].x(), object_points[i].y(), object_points[i].z()));
    v_point->setFixed(true);
    optimizer_.addVertex(v_point);

    EdgeProjection *edge =
        new EdgeProjection(R_camera_imu, R_pitch, t_camera_armor, K_);
    edge->setId(id_counter++);
    edge->setVertex(0, v_yaw);
    edge->setVertex(1, v_point);
    edge->setMeasurement(Eigen::Vector2d(image_points[i].x, image_points[i].y));
    edge->setInformation(EdgeProjection::InfoMatrixType::Identity());
    edge->setRobustKernel(new g2o::RobustKernelHuber);
    optimizer_.addEdge(edge);
  }

  // Start optimizing
  optimizer_.initializeOptimization();
  optimizer_.optimize(20);

  // Get yaw angle after optimization
  double yaw_optimized = v_yaw->estimate();
  // std::cout<<"After optimization, yaw is:"<<yaw_optimized * 180 / 3.14<<std::endl;
  if (std::isnan(yaw_optimized)) {
    FYT_ERROR("armor_detector", "Yaw angle is nan after optimization");
    return R_camera_armor;
  }

  Sophus::SO3d R_yaw = Sophus::SO3d::exp(Eigen::Vector3d(0, 0, yaw_optimized));
  return (R_camera_imu * R_yaw * R_pitch).matrix();
  // double error = calculateProjectionError();
  }
double BoundSolver::calculateProjectionError(){
  double error = 0;
  error += pow((image_points[0].x + image_points[1].x -
                projected_points[0].x - projected_points[1].x), 2);
  error += pow((image_points[2].x + image_points[3].x -
                 projected_points[2].x - projected_points[3].x), 2);
  return error;
}
void BoundSolver::setImagePoints(bool clock){
  image_points.clear();
  cv::Point2f points[4];
  for(auto boundary : boundaries){
    boundary.points(points);
    if(clock == 0){
      image_points.emplace_back(points[0]);
      image_points.emplace_back(points[1]);
    } else {
      image_points.emplace_back(points[2]);
      image_points.emplace_back(points[3]);
    }
  }
}

void BoundSolver::processImage(cv::Mat img){
  cv::Mat blurred_img, gray, binary;
  std::vector<std::vector<cv::Point>> contours;
  cv::GaussianBlur(img, blurred_img, cv::Size(3, 3), 2.0);
  cv::cvtColor(blurred_img, gray, cv::COLOR_BGR2GRAY);
  cv::threshold(gray, binary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
  cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
  boundaries.clear();
  for (const auto &contour : contours) {
    if (contour.size() < 6) continue;
    boundaries.emplace_back(minAreaRect(contour));
  }
  // auto max_contour = std::max_element(contours.begin(), contours.end(),
  //       [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
  //           return cv::contourArea(a) < cv::contourArea(b);
  //       });
  // boundary = minAreaRect(*max_contour);
}

void BoundSolver::detectBound(cv::Mat img){
  processImage(img);
  // drawBound(img);
}

void BoundSolver::setBoundPoints(const std::string& bound_type, const std::vector<cv::Point3f> bound_points){
  bound_points_map_[bound_type] = bound_points;  //无用， 可删除
}

void BoundSolver::drawBound(cv::Mat &img){
  cv::Point2f points[4];
  for(auto boundary : boundaries){
    boundary.points(points);
    for (int i = 0; i < 4; i++) {
      cv::line(img, points[i], points[(i + 1) % 4], cv::Scalar(0, 0, 255), 3);
    }
    cv::line(img, points[3], points[0], cv::Scalar(0, 0, 255), 3);
  }
}

void BoundSolver::reprojection(std::vector<Eigen::Vector3d> object_points, 
                               Eigen::Matrix3d R_camera_armor, Eigen::Vector3d t_camera_armor){
  std::vector<cv::Point3f> bound_points;
  cv::Point3f bound_point;
  cv::Mat rvec, tvec;
  for(auto point : object_points){
    bound_points.emplace_back(
        static_cast<float>(point.x()),
        static_cast<float>(point.y()), 
        static_cast<float>(point.z()));
  }
  cv::eigen2cv(R_camera_armor, rvec);
  cv::eigen2cv(t_camera_armor, tvec);
  cv::projectPoints(bound_points, rvec, tvec, camera_matrix_, distortion_coefficients_, projected_points);
}

void BoundSolver::drawProjection(cv::Mat img){
  cv::line(img, projected_points[0], projected_points[1], cv::Scalar(0, 255, 0), 3);
  cv::line(img, projected_points[2], projected_points[3], cv::Scalar(0, 255, 0), 3);
}

std::vector<cv::Mat> readImages(const std::string& folder_path){
    std::vector<cv::Mat> images;
    std::vector<std::string> file_paths;
    cv::glob(folder_path+"/*.jpg",file_paths);
    for(const auto& file_path:file_paths){
        cv::Mat image = cv::imread(file_path);
        if(!image.empty())
        images.emplace_back(image);
        else
        std::cout<<"failed to read image"<<std::endl;
    }
    return images;
}
 
void resizeWindow(const std::string& window_name, cv::Mat img, int ratio){
  int w = img.cols*ratio/100;
  int h = img.rows*ratio/100;
  cv::resizeWindow(window_name, w, h);
}


}