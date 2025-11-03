#include <fmt/core.h>

#include <chrono>
#include <filesystem>
#include <opencv2/opencv.hpp>

#include "io/camera.hpp"
#include "tasks/auto_aim/detector.hpp"
#include "tasks/auto_aim/yolo.hpp"
#include "tools/exiter.hpp"
#include "tools/logger.hpp"
#include "tools/math_tools.hpp"

const std::string keys =
  "{help h usage ? |                        | 输出命令行参数说明 }"
  "{@config-path   | configs/sentry.yaml    | yaml配置文件的路径}"
  "{tradition t    |  false                 | 是否使用传统方法识别}"
  "{output-folder  |                        | 可选：按 s 保存图片到该目录（不提供则不保存）}";

int main(int argc, char * argv[])
{
  // 读取命令行参数
  cv::CommandLineParser cli(argc, argv, keys);
  if (cli.has("help")) {
    cli.printMessage();
    return 0;
  }
  auto config_path = cli.get<std::string>(0);
  //auto use_tradition = cli.get<bool>("tradition");
  auto use_tradition = true;
  auto output_folder = cli.get<std::string>("output-folder");
  // 如果用户提供了输出目录，确保存在
  if (!output_folder.empty()) std::filesystem::create_directories(output_folder);
  // 初始化保存计数，避免覆盖已有文件（查找目录中最大数字命名）
  int save_count_initial = 0;
  if (!output_folder.empty()) {
    for (auto &p : std::filesystem::directory_iterator(output_folder)) {
      if (!p.is_regular_file()) continue;
      auto name = p.path().filename().string();
      // 期望格式为 N.jpg
      auto dot = name.find('.');
      if (dot == std::string::npos) continue;
      auto stem = name.substr(0, dot);
      try {
        int n = std::stoi(stem);
        if (n > save_count_initial) save_count_initial = n;
      } catch (...) {
        continue;
      }
    }
  }


  tools::Exiter exiter;

  io::Camera camera(config_path);
  auto_aim::Detector detector(config_path, true);
  auto_aim::YOLO yolo(config_path, true);

  std::chrono::steady_clock::time_point timestamp;

  while (!exiter.exit()) {
    cv::Mat img;
    std::list<auto_aim::Armor> armors;

    camera.read(img, timestamp);

  if (img.empty()) break;

    auto last = std::chrono::steady_clock::now();

    if (use_tradition)
      armors = detector.detect(img);
    else
      armors = yolo.detect(img);

    auto now = std::chrono::steady_clock::now();
    auto dt = tools::delta_time(now, last);
    tools::logger()->info("{:.2f} fps", 1 / dt);

    // 显示并处理按键：q 退出，s 保存当前帧（若提供了 output-folder）
    cv::imshow("camera_detect", img);
    auto key = cv::waitKey(33);
    if (key == 'q') break;
    if (key == 's' && !output_folder.empty()) {
      // 生成递增文件名，兼容 calibrate_camera 的 1.jpg,2.jpg,... 格式
      static int save_count = 0;
      save_count++;
      auto img_path = fmt::format("{}/{}.jpg", output_folder, save_count);
      if (cv::imwrite(img_path, img))
        tools::logger()->info("Saved frame {} to {}", save_count, img_path);
      else
        tools::logger()->error("Failed to save frame to {}", img_path);
    }
  }

  return 0;
}