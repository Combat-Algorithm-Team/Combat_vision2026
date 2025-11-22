#ifndef AUTO_AIM__TARGET_HPP
#define AUTO_AIM__TARGET_HPP

#include <Eigen/Dense>
#include <chrono>
#include <optional>
#include <queue>
#include <string>
#include <vector>

#include "armor.hpp"
#include "tools/extended_kalman_filter.hpp"

namespace auto_aim
{

class Target
{
public:
  ArmorName name;
  ArmorType armor_type;
  ArmorPriority priority;
  bool jumped;
  int last_id;  // debug only

  Target() = default;
  Target(
    const Armor & armor, std::chrono::steady_clock::time_point t, double radius, int armor_num,
    Eigen::VectorXd P0_dig);
  Target(double x, double vyaw, double radius, double h);

  void predict(std::chrono::steady_clock::time_point t);
  void predict(double dt);
  void update(const Armor & armor);

  Eigen::VectorXd ekf_x() const;
  const tools::ExtendedKalmanFilter & ekf() const;
  std::vector<Eigen::Vector4d> armor_xyza_list() const;

  bool diverged() const;

  bool convergened();

  bool isinit = false;

  bool checkinit();

  // 原始观测量（未经滤波）: [yaw, pitch, distance, angle]
  double obs_yaw = 0.0;
  double obs_pitch = 0.0;
  double obs_dist = 0.0;
  double obs_angle = 0.0;

  // 从观测反推的目标中心位置（未经滤波）
  double obs_center_x = 0.0;
  double obs_center_y = 0.0;
  double obs_center_z = 0.0;

  // 从连续观测计算的速度（数值微分）
  double obs_vx = 0.0;
  double obs_vy = 0.0;
  double obs_vz = 0.0;
  double obs_w = 0.0;  // 角速度

private:
  int armor_num_;
  int switch_count_;
  int update_count_;

  bool is_switch_, is_converged_;

  tools::ExtendedKalmanFilter ekf_;
  std::chrono::steady_clock::time_point t_;

  // 用于数值微分计算速度
  double prev_obs_x_ = 0.0;
  double prev_obs_y_ = 0.0;
  double prev_obs_z_ = 0.0;
  double prev_obs_angle_ = 0.0;
  std::chrono::steady_clock::time_point prev_obs_t_ = std::chrono::steady_clock::time_point{};

  void update_ypda(const Armor & armor, int id);  // yaw pitch distance angle

  Eigen::Vector3d h_armor_xyz(const Eigen::VectorXd & x, int id) const;
  Eigen::MatrixXd h_jacobian(const Eigen::VectorXd & x, int id) const;
};

}  // namespace auto_aim

#endif  // AUTO_AIM__TARGET_HPP