#include <fmt/core.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>

#include "io/camera.hpp"
// #include "io/cboard.hpp"
#include "io/gimbal/gimbal.hpp"
#include "tasks/auto_aim/aimer.hpp"
#include "tasks/auto_aim/armor.hpp"
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
  bool debug_display = false;
  if (yaml["debug_display"]) {
    debug_display = yaml["debug_display"].as<bool>();
  }
  double debug_display_scale = 0.5;
  if (yaml["debug_display_scale"]) {
    debug_display_scale = yaml["debug_display_scale"].as<double>();
  }
  int debug_display_wait_ms = 30;
  if (yaml["debug_display_wait_ms"]) {
    debug_display_wait_ms = yaml["debug_display_wait_ms"].as<int>();
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
    
    // 打印每个装甲板的PnP解算结果（x, y, z坐标和欧拉角）
    for (auto & armor : armors) {
      // 调用solver.solve()进行PnP解算
      solver.solve(armor);
      
      // 打印Gimbal坐标系中的解算结果
      // fmt::print(
      //   "[Armor PnP] name={}, xyz_gimbal=({:.3f}, {:.3f}, {:.3f}), "
      //   "ypr_gimbal=({:.2f}°, {:.2f}°, {:.2f}°)\n",
      //   auto_aim::ARMOR_NAMES[armor.name],
      //   armor.xyz_in_gimbal[0], armor.xyz_in_gimbal[1], armor.xyz_in_gimbal[2],
      //   armor.ypr_in_gimbal[0] * 180.0 / M_PI, armor.ypr_in_gimbal[1] * 180.0 / M_PI,
      //   armor.ypr_in_gimbal[2] * 180.0 / M_PI);
      
      // // 打印World坐标系中的解算结果
      // fmt::print(
      //   "[Armor World] name={}, xyz_world=({:.3f}, {:.3f}, {:.3f}), "
      //   "ypr_world=({:.2f}°, {:.2f}°, {:.2f}°)\n",
      //   auto_aim::ARMOR_NAMES[armor.name],
      //   armor.xyz_in_world[0], armor.xyz_in_world[1], armor.xyz_in_world[2],
      //   armor.ypr_in_world[0] * 180.0 / M_PI, armor.ypr_in_world[1] * 180.0 / M_PI,
      //   armor.ypr_in_world[2] * 180.0 / M_PI);
    }

    // 传给跟踪器
    auto targets = tracker.track(armors, t);//dsjajknklfsfkljan

    // 调试阶段：使用固定弹速 20 m/s（而非从下位机读取）
    // auto command = aimer.aim(targets, t, 20.0);
    // 使用下位机返回的弹速；若未提供有效值则回退到 20.0 m/s
    const auto gs = gimbal.state();
    // const double bullet_speed = (gs.bullet_speed > 0.1 && std::isfinite(gs.bullet_speed))
    //                               ? static_cast<double>(gs.bullet_speed)
    //                               : 20.0;
    const double bullet_speed = 26.0; // 暂时固定为 26 m/s

    auto command = aimer.aim(targets, t, bullet_speed);
    command.shoot = shooter.shoot(command, aimer, targets, ypr);

    // 复用 io::Command 的 horizon_distance
    // 字段，按现有多线程实现的方式计算水平距离
    command.horizon_distance =
      targets.empty()
        ? 0.0
        : std::sqrt(
            tools::square(targets.front().ekf_x()[0]) + tools::square(targets.front().ekf_x()[2]));

    // 通过串口发送指令（复用已有 Command 接口，内部会按 32B
    // 协议打包并带上时间戳）
    constexpr double kRad2Deg = 180.0 / M_PI;
    const double yaw_deg = command.yaw * kRad2Deg;
    const double pitch_deg = command.pitch * kRad2Deg;

    if ((frame_id % 50) == 100) {
      fmt::print(
        "[Gimbal Cmd] control: {}, shoot: {}, yaw: {:.3f} deg, pitch: {:.3f} deg, "
        "horizon_distance: {:.3f}\n",
        static_cast<int>(command.control), static_cast<int>(command.shoot), yaw_deg, pitch_deg,
        command.horizon_distance);
    }

    if (debug_display) {
      cv::Mat display_img;
      img.copyTo(display_img);

      tools::draw_text(
        display_img,
        fmt::format(
          "command is {},{:.2f},{:.2f},shoot:{}", command.control, command.yaw * kRad2Deg,
          command.pitch * kRad2Deg, command.shoot),
        {10, 60}, {154, 50, 205});

      Eigen::Quaterniond gimbal_q = q;
      const double gimbal_yaw_deg = tools::eulers(gimbal_q, 2, 1, 0)[0] * kRad2Deg;
      tools::draw_text(
        display_img, fmt::format("gimbal yaw{:.2f}", gimbal_yaw_deg), {10, 90}, {255, 255, 255});

      // 绘制检测阶段得到的装甲板角点（绿色线框）
      for (const auto & armor : armors) {
        if (armor.points.size() >= 4) {
          tools::draw_points(display_img, armor.points, {0, 255, 0});
        }
      }

      // 绘制跟踪器内部的重投影结果（绿色）
      for (const auto & target : targets) {
        auto armor_xyza_list = target.armor_xyza_list();
        for (const Eigen::Vector4d & xyza : armor_xyza_list) {
          auto image_points =
            solver.reproject_armor(xyza.head(3), xyza[3], target.armor_type, target.name);
          tools::draw_points(display_img, image_points, {0, 255, 0});
        }
      }

      // 绘制瞄准点（红色）
      if (!targets.empty()) {
        const auto & primary_target = targets.front();
        const auto & aim_point = aimer.debug_aim_point;
        if (aim_point.valid) {
          auto aim_points = solver.reproject_armor(
            aim_point.xyza.head(3), aim_point.xyza[3], primary_target.armor_type,
            primary_target.name);
          tools::draw_points(display_img, aim_points, {0, 0, 255});
        }
      }

      cv::Mat display_view;
      if (
        debug_display_scale > 0.0 && (debug_display_scale < 0.999 || debug_display_scale > 1.001)) {
        cv::resize(display_img, display_view, {}, debug_display_scale, debug_display_scale);
      } else {
        display_view = display_img;
      }

      const int wait_time = debug_display_wait_ms > 0 ? debug_display_wait_ms : 1;
      cv::imshow("auto_aim_debug", display_view);
      const int key = cv::waitKey(wait_time);
      if (key == 'q' || key == 27) {
        break;
      }
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
      const auto & target = targets.front();
      const auto & x = target.ekf_x();
      //fmt::print("[EKF] x=[");
      // for (int i = 0; i < x.size(); ++i) {
      //   fmt::print("{:.4f}{}", x[i], (i + 1 == x.size() ? "]\n" : ", "));
      // }
      // //fmt::print("        // [x, vx, y, vy, z, vz, a, w, r, l, h]\n");

      // Plot EKF states and raw observations
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
      // 原始观测量（未经滤波）
      j["obs_yaw"] = target.obs_yaw;
      j["obs_pitch"] = target.obs_pitch;
      j["obs_dist"] = target.obs_dist;
      j["obs_angle"] = target.obs_angle;

      // 从观测反推的目标中心位置和速度（未经滤波）
      j["obs_center_x"] = target.obs_center_x;
      j["obs_center_y"] = target.obs_center_y;
      j["obs_center_z"] = target.obs_center_z;
      j["obs_vx"] = target.obs_vx;
      j["obs_vy"] = target.obs_vy;
      j["obs_vz"] = target.obs_vz;
      j["obs_w"] = target.obs_w;

      j["frame_id"] = frame_id;
      plotter.plot(j);
    }

    ++frame_id;
  }

  return 0;
}
