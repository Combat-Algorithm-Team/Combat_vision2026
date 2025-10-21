#include <fmt/core.h>

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <opencv2/opencv.hpp>

#include "io/camera.hpp"
//#include "io/cboard.hpp"
#include "io/gimbal/gimbal.hpp"
#include "tools/img_tools.hpp"
#include "tools/logger.hpp"
#include "tools/math_tools.hpp"

const std::string keys =
  "{help h usage ?  |                          | 输出命令行参数说明}"
  "{@config-path c  | configs/calibration.yaml | yaml配置文件路径 }"
  "{output-folder o |      assets/img_with_q   | 输出文件夹路径   }";

void write_q(const std::string q_path, const Eigen::Quaterniond & q)
{
  std::ofstream q_file(q_path);
  Eigen::Vector4d xyzw = q.coeffs();
  // 输出顺序为wxyz
  q_file << fmt::format("{} {} {} {}", xyzw[3], xyzw[0], xyzw[1], xyzw[2]);
  q_file.close();
}

void capture_loop(
  const std::string & config_path, const std::string & can, const std::string & output_folder)
{
  io::Gimbal gimbal(config_path);
  io::Camera camera(config_path);
  cv::Mat img;
  std::chrono::steady_clock::time_point timestamp;

  int count = 0;
  // 显式创建窗口，便于某些环境（例如部分桌面/容器）下窗口管理
  const std::string kWinName = "Press s to save, q to quit";
  // 在某些容器或远程环境下，先启动窗口线程可以提高窗口创建成功率
  cv::startWindowThread();
  cv::namedWindow(kWinName, cv::WINDOW_NORMAL);
  cv::resizeWindow(kWinName, 960, 540);
  // 诊断：检查窗口是否创建成功（-1 表示窗口不存在/未创建）
  {
    double prop = cv::getWindowProperty(kWinName, cv::WND_PROP_VISIBLE);
    if (prop < 0) {
      tools::logger()->warn(
        "窗口 '{}' 未创建成功，可能缺少 GUI 后端或未正确映射 X11。", kWinName);
    }
  }
  while (true) {
    camera.read(img, timestamp);
    if (img.empty()) {
      // 没拿到画面，通常是相机未就绪/被占用，或者容器内未正确映射设备
      tools::logger()->warn("未获取到摄像头图像帧(img.empty)。请检查相机连接/权限/配置。");
      // 避免 busy loop
      cv::waitKey(10);
      continue;
    }
    Eigen::Quaterniond q = gimbal.q(timestamp);

    // 在图像上显示欧拉角，用来判断imuabs系的xyz正方向，同时判断imu是否存在零漂
    auto img_with_ypr = img.clone();
    Eigen::Vector3d zyx = tools::eulers(q, 2, 1, 0) * 57.3;  // degree
    tools::draw_text(img_with_ypr, fmt::format("Z {:.2f}", zyx[0]), {40, 40}, {0, 0, 255});
    tools::draw_text(img_with_ypr, fmt::format("Y {:.2f}", zyx[1]), {40, 80}, {0, 0, 255});
    tools::draw_text(img_with_ypr, fmt::format("X {:.2f}", zyx[2]), {40, 120}, {0, 0, 255});

    std::vector<cv::Point2f> centers_2d;
    auto success = cv::findChessboardCorners(img, cv::Size(10, 7), centers_2d);  // 默认棋盘格
    cv::drawChessboardCorners(img_with_ypr, cv::Size(10, 7), centers_2d, success);  // 显示识别结果
    cv::resize(img_with_ypr, img_with_ypr, {}, 0.5, 0.5);  // 显示时缩小图片尺寸

    // 按“s”保存图片和对应四元数，按“q”退出程序
    cv::imshow(kWinName, img_with_ypr);
    auto key = cv::waitKey(1);
    if (key == 'q')
      break;
    else if (key != 's')
      continue;

    // 保存图片和四元数
    count++;
    auto img_path = fmt::format("{}/{}.jpg", output_folder, count);
    auto q_path = fmt::format("{}/{}.txt", output_folder, count);
    cv::imwrite(img_path, img);
    write_q(q_path, q);
    tools::logger()->info("[{}] Saved in {}", count, output_folder);
  }

  // 离开该作用域时，camera和cboard会自动关闭
}

int main(int argc, char * argv[])
{
  // 读取命令行参数
  cv::CommandLineParser cli(argc, argv, keys);
  if (cli.has("help")) {
    cli.printMessage();
    return 0;
  }
  auto config_path = cli.get<std::string>(0);
  auto output_folder = cli.get<std::string>("output-folder");

  // 新建输出文件夹
  std::filesystem::create_directory(output_folder);

  // 诊断：打印 DISPLAY 环境变量，便于排查容器内 GUI 是否映射
  const char *display = std::getenv("DISPLAY");
  tools::logger()->info("DISPLAY={}", display ? display : "(null)");

  tools::logger()->info("默认标定板尺寸为10列7行");
  // 主循环，保存图片和对应四元数
  capture_loop(config_path, "can0", output_folder);

  tools::logger()->warn("注意四元数输出顺序为wxyz");

  return 0;
}
