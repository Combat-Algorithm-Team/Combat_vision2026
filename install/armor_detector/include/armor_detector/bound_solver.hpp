#ifndef BOUND_SOLVER_HPP_
#define BOUND_SOLVER_HPP_
// std
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <unordered_map> 
#include <memory>
#include <numeric>
#include <string>
#include <vector>
#include <string>
// ros2
#include <cv_bridge/cv_bridge.h>

#include <image_transport/image_transport.hpp>
#include <rclcpp/qos.hpp>
// third party
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
// project
#include "armor_detector/types.hpp"
#include "armor_detector/graph_optimizer.hpp"
#include "armor_detector/types.hpp"
#include "rm_utils/assert.hpp"
#include "rm_utils/common.hpp"
#include "rm_utils/logger/log.hpp"
#include "rm_utils/math/pnp_solver.hpp"
#include "rm_utils/math/utils.hpp"
#include "rm_utils/url_resolver.hpp"
// g2o
#include <g2o/core/base_multi_edge.h>
#include <g2o/core/base_vertex.h>
#include <g2o/core/optimization_algorithm.h>
#include <g2o/core/optimization_algorithm_factory.h>
#include <g2o/core/optimization_algorithm_levenberg.h>
#include <g2o/core/robust_kernel.h>
#include <g2o/core/sparse_optimizer.h>

namespace fyt::auto_aim{
constexpr int BOUNDS = 4;

class BoundSolver{
public:
    BoundSolver(sensor_msgs::msg::CameraInfo::SharedPtr camera_info);
    void detectBound(cv::Mat img);
    void drawBound(cv::Mat& edge); 
    void setBoundPoints(const std::string& armor_type, const std::vector<cv::Point3f> bound_points);
    void reprojection(std::vector<Eigen::Vector3d> object_points, Eigen::Matrix3d R_camera_armor, Eigen::Vector3d t_camera_armor);
    void drawProjection(cv::Mat img);
    Eigen::Matrix3d solveBound(const Armor &armor, const Eigen::Vector3d &t_camera_armor,
                               const Eigen::Matrix3d &R_camera_armor,
                               const Eigen::Matrix3d &R_imu_camera);
    std::vector<cv::RotatedRect> boundaries;
private:
    cv::Mat camera_matrix_;
    cv::Mat distortion_coefficients_;
    
    std::vector<cv::Point3f> object_points;
    std::vector<cv::Point2f> image_points, projected_points;

    std::unordered_map<std::string, std::vector<cv::Point3f>> bound_points_map_;
    Eigen::Matrix3d K_;
    
    g2o::SparseOptimizer optimizer_;
    g2o::OptimizationAlgorithmProperty solver_property_;
    g2o::OptimizationAlgorithmLevenberg *lm_algorithm_; 
    
    void processImage(cv::Mat img);
    void setImagePoints(bool clock);
    double calculateProjectionError();
    bool clock;
};

    void resizeWindow(const std::string& window_name, cv::Mat img, int ratio);
    std::vector<cv::Mat> readImages(const std::string& folder_path);
}

#endif