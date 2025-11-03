#include <fmt/core.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>

#include "io/camera.hpp"
// #include "io/cboard.hpp"
#include "io/gimbal/gimbal.hpp"
#include "tasks/auto_aim/aimer.hpp"
#include "tasks/auto_aim/detector.hpp"
#include "tasks/auto_aim/multithread/commandgener.hpp"
#include "tasks/auto_aim/planner/planner.hpp"
#include "tasks/auto_aim/shooter.hpp"
#include "tasks/auto_aim/solver.hpp"
#include "tasks/auto_aim/tracker.hpp"
#include "tools/exiter.hpp"
#include "tools/img_tools.hpp"
#include "tools/logger.hpp"
#include "tools/math_tools.hpp"
#include "tools/plotter.hpp"
#include "tools/recorder.hpp"
#include "tools/yaml.hpp"

using namespace std::chrono;

const std::string keys =
  "{help h usage ? |      | 输出命令行参数说明}"
  "{@config-path   | configs/sentry.yaml | 位置参数，yaml配置文件路径 }";

int main(int argc, char * argv[])
{
  cv::CommandLineParser cli(argc, argv, keys);
  auto config_path = cli.get<std::string>(0);
  if (cli.has("help") || config_path.empty()) {
    cli.printMessage();
    return 0;
  }

  tools::Exiter exiter;
  tools::Recorder recorder;

  auto yaml = tools::load(config_path);

  // 从配置文件读取 Plotter 地址，默认使用 host.docker.internal（Docker 宿主机）
  std::string plotter_host = "host.docker.internal";
  uint16_t plotter_port = 9870;
  if (yaml["plotter_host"]) {
    plotter_host = yaml["plotter_host"].as<std::string>();
  }
  if (yaml["plotter_port"]) {
    plotter_port = yaml["plotter_port"].as<uint16_t>();
  }
  tools::Plotter plotter(plotter_host, plotter_port);
  tools::logger()->info("Plotter configured to send UDP to {}:{}", plotter_host, plotter_port);
  double gimbal_time_offset_ms = 3.0;
  if (yaml["gimbal_time_offset_ms"]) {
    gimbal_time_offset_ms = yaml["gimbal_time_offset_ms"].as<double>();
  }
  const auto gimbal_time_offset = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
    std::chrono::duration<double, std::milli>(gimbal_time_offset_ms));

  // 使用串口云台通信替代基于 CAN 的 CBoard
  io::Gimbal gimbal(config_path);
  io::Camera camera(config_path);

  auto_aim::Detector detector(config_path, false);
  auto_aim::Solver solver(config_path);
  auto_aim::Tracker tracker(config_path, solver);
  auto_aim::Aimer aimer(config_path);
  auto_aim::Shooter shooter(config_path);
  auto_aim::Planner planner(config_path);

  cv::Mat img;
  Eigen::Quaterniond q;
  std::chrono::steady_clock::time_point t;

  auto mode = io::GimbalMode::IDLE;
  auto last_mode = io::GimbalMode::IDLE;
  std::uint64_t frame_id = 0;  // 控制台打印节流

  // 帧率统计相关变量
  int frame_count = 0;
  double fps = 0.0;
  auto fps_last_time = steady_clock::now();

  while (!exiter.exit()) {
    const auto loop_start = steady_clock::now();
    camera.read(img, t);
    q = gimbal.q(t + gimbal_time_offset);
    mode = gimbal.mode();

    if (last_mode != mode) {
      tools::logger()->info("Switch to {}", gimbal.str(mode));
      last_mode = mode;
    }

    // recorder.record(img, q, t);

    solver.set_R_gimbal2world(q);

    Eigen::Vector3d ypr = tools::eulers(solver.R_gimbal2world(), 2, 1, 0);

    auto armors = detector.detect(img);

    // 打印每个装甲板的关键信息
    // for (const auto& armor : armors) {
    //   fmt::print("Armor: name={}, type={}, center=({:.1f},{:.1f}), conf={:.2f}\n",
    //              static_cast<int>(armor.name),  // 或者用自定义的to_string(armor.name)
    //              static_cast<int>(armor.type),  // 或者用自定义的to_string(armor.type)
    //              armor.center.x, armor.center.y,
    //              armor.confidence);
    // }

    // 传给跟踪器
    auto targets = tracker.track(armors, t);

    // 调试阶段：使用固定弹速 20 m/s（而非从下位机读取）
    // auto command = aimer.aim(targets, t, 20.0);
    // 使用下位机返回的弹速；若未提供有效值则回退到 20.0 m/s
    const auto gs = gimbal.state();
    const double bullet_speed =
        (gs.bullet_speed > 0.1 && std::isfinite(gs.bullet_speed))
            ? static_cast<double>(gs.bullet_speed)
            : 20.0;

    // 基于配置与容差策略决定是否开火（auto_fire 在 Shooter 内部读取并生效）
    auto command = aimer.aim(targets, t, bullet_speed);
    command.shoot = shooter.shoot(command, aimer, targets, ypr);

    // 复用 io::Command 的 horizon_distance
    // 字段，按现有多线程实现的方式计算水平距离
    command.horizon_distance =
      targets.empty()
        ? 0.0
        : std::sqrt(
            tools::square(targets.front().ekf_x()[0]) + tools::square(targets.front().ekf_x()[2]));

    // // 观测yaw：由目标在世界(或云台)系下的水平位置计算方位角
    // // 假设状态向量次序为 [x, vx, z, vz, y, vy, yaw, yaw_rate, ...]
    // double observed_yaw = 0.0;
    // double observed_yaw_tfly = 0.0;  // t+tfly 的预测方位角（用于验证提前量）
    // if (!targets.empty()) {
    //   const auto& s = targets.front().ekf_x();
    //   if (s.size() >= 4) {
    //     const double tx = s[0];  // x
    //     const double tz = s[2];  // z
    //     const double vx = s[1];  // vx
    //     const double vz = s[3];  // vz

    //     observed_yaw = std::atan2(tx, tz);

    //     // 估算弹丸运动飞行时间 tfly ≈ 水平距离 / 子弹速度
    //     const double horiz_dist = std::hypot(tx, tz);
    //     const double tfly = (bullet_speed > 1e-3) ? (horiz_dist / bullet_speed) : 0.0;

    //     // 线性外推目标水平位置到 t+tfly，得到对应方位角
    //     const double tx_t = tx + vx * tfly;
    //     const double tz_t = tz + vz * tfly;
    //     observed_yaw_tfly = std::atan2(tx_t, tz_t);
    //   }
    // }
    // double shoot_yaw = command.yaw;

    // // 数值微分：命令yaw的速度/加速度，便于看是否“提前减速”和“限加速度”
    // static bool first = true;
    // static double prev_cmd_yaw = 0.0;
    // static double prev_cmd_vel = 0.0;
    // static double prev_obs_yaw = 0.0;
    // static std::chrono::steady_clock::time_point prev_t = t;

    // const double dt = std::max(1e-3, tools::delta_time(t, prev_t));  // s
    // const double cmd_vel = first ? 0.0 : (shoot_yaw - prev_cmd_yaw) / dt;
    // const double cmd_acc = first ? 0.0 : (cmd_vel - prev_cmd_vel) / dt;

    // // 检测“装甲切换”：观测yaw出现较大跳变（环绕处理）
    // auto angle_diff = [](double a, double b) {
    //   double d = a - b;
    //   // wrap到[-pi, pi]
    //   d = std::atan2(std::sin(d), std::cos(d));
    //   return d;
    // };
    // const double d_obs = first ? 0.0 : angle_diff(observed_yaw, prev_obs_yaw);
    // const bool switch_event = std::abs(d_obs) > 0.6;  // 阈值可调：~0.6rad≈34°

    // prev_t = t;
    // prev_cmd_yaw = shoot_yaw;
    // prev_cmd_vel = cmd_vel;
    // prev_obs_yaw = observed_yaw;
    // first = false;

    // // 发送到Plotter
    // nlohmann::json j;
    // j["observed_yaw"] = observed_yaw;
    // j["observed_yaw_tfly"] = observed_yaw_tfly;
    // j["shoot_yaw"] = shoot_yaw;
    // j["yaw_cmd_vel"] = cmd_vel;   // 命令yaw速度
    // j["yaw_cmd_acc"] = cmd_acc;   // 命令yaw加速度
    // j["yaw_error"] = shoot_yaw - observed_yaw;
    // j["yaw_error_tfly"] = shoot_yaw - observed_yaw_tfly;
    // j["switch_event"] = switch_event ? 1 : 0;  // 观察切换时刻
    // j["shoot_flag"] = static_cast<int>(command.shoot); // 开火标志
    // j["frame_id"] = frame_id;
    // plotter.plot(j);

    // 通过串口发送指令（复用已有 Command 接口，内部会按 32B
    // 协议打包并带上时间戳）
    constexpr double kRad2Deg = 180.0 / M_PI;
    const double yaw_deg = command.yaw * kRad2Deg;
    const double pitch_deg = command.pitch * kRad2Deg;

    // 控制台打印开销较大，默认每 50 帧输出一次，降低对帧率的影响
    // if ((frame_id % 50) == 0) {
    //   fmt::print(
    //     "[Planner] control: {}, fire: {}, yaw: {:.3f} deg (vel: {:.3f}, acc: {:.3f}), pitch: "
    //     "{:.3f} deg (vel: {:.3f}, acc: {:.3f})\n",
    //     plan.control, plan.fire, plan.yaw * kRad2Deg, plan.yaw_vel, plan.yaw_acc,
    //     plan.pitch * kRad2Deg, plan.pitch_vel, plan.pitch_acc);
    // }
    if ((frame_id % 50) == 0) {
      fmt::print(
        "[Gimbal Cmd] control: {}, shoot: {}, yaw: {:.3f} deg, pitch: {:.3f} deg, "
        "horizon_distance: {:.3f}\n",
        static_cast<int>(command.control), static_cast<int>(command.shoot), yaw_deg, pitch_deg,
        command.horizon_distance);
    }

    gimbal.send(command);
    const auto loop_end = steady_clock::now();

    // 帧率统计与打印
    ++frame_count;
    if ((loop_end - fps_last_time) >= std::chrono::seconds(1)) {
      fps = frame_count / std::chrono::duration<double>(loop_end - fps_last_time).count();
      fmt::print("[FPS] {:.2f}\n", fps);
      frame_count = 0;
      fps_last_time = loop_end;
    }

    if ((frame_id % 50) == 0) {
      fmt::print(
        "[Loop] elapsed: {} ms\n", duration_cast<milliseconds>(loop_end - loop_start).count());
    }

    if (!targets.empty()) {
      const auto & x = targets.front().ekf_x();
      //fmt::print("[EKF] x=[");
      // for (int i = 0; i < x.size(); ++i) {
      //   fmt::print("{:.4f}{}", x[i], (i + 1 == x.size() ? "]\n" : ", "));
      // }
      // //fmt::print("        // [x, vx, y, vy, z, vz, a, w, r, l, h]\n");

      // Plot EKF states
      nlohmann::json j;
      if (x.size() >= 6) {
        j["ekf_x"] = x[0];
        j["ekf_vx"] = x[1];
        j["ekf_y"] = x[2];
        j["ekf_vy"] = x[3];
        j["ekf_z"] = x[4];
        j["ekf_vz"] = x[5];
      }
      if (x.size() >= 8) {
        j["ekf_yaw"] = x[6];
        j["ekf_w"] = x[7];
      }
      j["frame_id"] = frame_id;
      plotter.plot(j);
    }

    ++frame_id;
  }

  return 0;
}
