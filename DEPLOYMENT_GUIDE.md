# OpenVINO 部署详细步骤

## 概述

本文档详细说明如何在另一台机器上部署 OpenVINO 装甲板检测模型。假设目标机器与当前机器配置相同（Intel CPU i7-13620H, Ubuntu 22.04）。

---

## 一、部署环境要求

| 项目 | 最低要求 |
|------|----------|
| 操作系统 | Ubuntu 22.04 LTS |
| CPU | Intel (支持 AVX2) |
| 内存 | 8GB+ |
| Docker | 已安装 |

---

## 二、新增/修改的文件清单

### 新增文件

| 文件 | 说明 |
|------|------|
| `dockerfile` | Docker 构建文件，包含 OpenVINO 安装 |
| `vv/src/rm_auto_aim/armor_detector/include/armor_detector/model_detector.hpp` | OpenVINO 检测器头文件 |
| `vv/src/rm_auto_aim/armor_detector/src/model_detector.cpp` | OpenVINO 检测器实现 |
| `vv/src/rm_auto_aim/armor_detector/model/0526.onnx` | 训练好的 ONNX 模型 |

### 修改的文件

| 文件 | 修改内容 |
|------|----------|
| `vv/src/rm_auto_aim/armor_detector/CMakeLists.txt` | 添加 OpenVINO 链接 |
| `vv/src/rm_bringup/config/node_params/armor_detector_params.yaml` | 启用模型检测器 |
| `vv/src/rm_auto_aim/armor_detector/src/armor_detector_node.cpp` | 模型路径修改 |

---

## 三、容器内安装的依赖（Dockerfile 关键部分）

### 1. OpenVINO 2024.6.0

```dockerfile
# 下载并安装 OpenVINO
RUN mkdir -p /opt/intel && \
    cd /tmp && \
    curl -L --fail --show-error --retry 5 --retry-delay 10 --connect-timeout 60 \
    https://storage.openvinotoolkit.org/repositories/openvino/packages/2024.6/linux/l_openvino_toolkit_ubuntu22_2024.6.0.17404.4c0f47d2335_x86_64.tgz \
    -o openvino.tgz && \
    tar -xzf openvino.tgz && \
    mv l_openvino_toolkit_ubuntu22_2024.6.0.17404.4c0f47d2335_x86_64 /opt/intel/openvino_2024.6.0 && \
    rm openvino.tgz

# 安装 OpenVINO 依赖
RUN yes | (cd /opt/intel/openvino_2024.6.0 && ./install_dependencies/install_openvino_dependencies.sh)

# 设置 OpenVINO 环境变量
ENV OPENVINO_DIR=/opt/intel/openvino_2024.6.0
ENV LD_LIBRARY_PATH="${OPENVINO_DIR}/runtime/lib/intel64:${LD_LIBRARY_PATH}"
RUN echo "source ${OPENVINO_DIR}/setupvars.sh" >> /root/.bashrc && \
    echo "${OPENVINO_DIR}/runtime/lib/intel64" >> /etc/ld.so.conf.d/openvino.conf && ldconfig
```

### 2. 系统依赖（已包含在基础镜像中）

```dockerfile
# 编译工具
build-essential gcc g++ cmake

# OpenCV 相关（ROS2 humble 自带）
ros-humble-cv-bridge
ros-humble-vision-opencv
```

---

## 四、部署步骤

### 步骤 1: 构建 Docker 镜像

```bash
cd /home/jwj-rune/auto_aim_runtime
docker build -t rm_auto_aim:openvino .
```

**注意**: 如果没有 g2o 和 Sophus 源码目录，需要先准备：
```bash
# 确保当前目录有 g2o 和 Sophus 文件夹
ls -la
# 应该看到 g2o  Sophus  dockerfile  vv
```

### 步骤 2: 运行容器

```bash
docker run -it --name rm_auto_aim \
    --network host \
    --device /dev/video0:/dev/video0 \
    --device /dev/ttyUSB0:/dev/ttyUSB0 \
    rm_auto_aim:openvino
```

### 步骤 3: 设置环境变量

在容器内执行：

```bash
# OpenVINO 环境变量（已在 .bashrc 中配置）
source /opt/intel/openvino_2024.6.0/setupvars.sh

# 验证 OpenVINO
${OPENVINO_DIR}/runtime/bin/intel64/benchmark_app -h
```

### 步骤 4: 编译 ROS2 包

```bash
cd /workspace
source /opt/ros/humble/setup.bash
colcon build --symlink-install --packages-select armor_detector
```

### 步骤 5: 配置参数

检查 `armor_detector_params.yaml`：

```yaml
# 文件路径: vv/src/rm_bringup/config/node_params/armor_detector_params.yaml
detector:
  model_path: "package://armor_detector/model/0526.onnx"  # 或绝对路径
  use_model_detector: true  # 确保为 true
  confidence_threshold: 0.5
  nms_threshold: 0.3
```

### 步骤 6: 运行

```bash
cd /workspace
source install/setup.bash
ros2 launch rm_bringup bringup.launch.py
```

---

## 五、验证 OpenVINO 加载

运行时日志应显示：

```
Using model-based detector (OpenVINO)
```

或者通过主题验证：

```bash
# 订阅检测结果
ros2 topic echo /auto_aim/armors
```

---

## 六、关键代码解析

### ModelDetector 构造函数（model_detector.cpp:50-84）

```cpp
ModelDetector::ModelDetector(const std::string &model_path,
                             float confidence_threshold,
                             float nms_threshold,
                             EnemyColor detect_color)
: detect_color(detect_color),
  confidence_threshold(confidence_threshold),
  nms_threshold(nms_threshold) {
  // 1. 加载模型
  auto model = core_.read_model(model_path);

  // 2. 设置 PrePostProcessor 自动预处理
  ov::preprocess::PrePostProcessor ppp(model);
  ppp.input().tensor()
      .set_element_type(ov::element::u8)        // 输入 u8
      .set_layout("NHWC")                       // NHWC 布局
      .set_color_format(ov::preprocess::ColorFormat::BGR);
  ppp.input().preprocess()
      .convert_element_type(ov::element::f32)  // 转换为 FP32
      .convert_color(ov::preprocess::ColorFormat::RGB)
      .scale({255.0f, 255.0f, 255.0f});        // 自动归一化
  ppp.input().model().set_layout("NCHW");
  ppp.output().tensor().set_element_type(ov::element::f32);

  // 3. 编译模型（CPU）
  model = ppp.build();
  compiled_model_ = core_.compile_model(model, "CPU");
  infer_request_ = compiled_model_.create_infer_request();
}
```

### 检测推理（model_detector.cpp:114-169）

```cpp
std::vector<Armor> ModelDetector::detect(const cv::Mat &input) {
  // 1. 预处理：letterbox
  float scale, pad_x, pad_y;
  cv::Mat padded = letterbox(input, scale, pad_x, pad_y);

  // 2. 设置输入
  auto input_tensor = infer_request_.get_input_tensor();
  uchar *input_data = input_tensor.data<uchar>();
  std::memcpy(input_data, padded.data, padded.total() * padded.elemSize());

  // 3. 推理
  infer_request_.infer();

  // 4. 获取输出
  auto output_tensor = infer_request_.get_output_tensor();
  const float *output_data = output_tensor.data<float>();

  // 5. 后处理
  armors_ = postprocess(output_data, num_candidates, scale, pad_x, pad_y);
  return armors_;
}
```

---

## 七、CMakeLists.txt 修改

```cmake
# 文件: vv/src/rm_auto_aim/armor_detector/CMakeLists.txt

# 找到 OpenVINO
find_package(OpenVINO REQUIRED)

# 手动设置 OpenVINO 路径（避免 find_package 问题）
set(OpenVINO_DIR /opt/intel/openvino_2024.6.0)
set(OpenVINO_INCLUDE_DIRS ${OpenVINO_DIR}/runtime/include)
set(OpenVINO_LIBRARY_DIRS ${OpenVINO_DIR}/runtime/lib/intel64)

# 链接 OpenVINO
target_link_libraries(${PROJECT_NAME}
    ${OpenVINO_LIBRARY_DIRS}/libopenvino.so
    ${OpenVINO_LIBRARY_DIRS}/libopenvino_runtime.so
    ...
)
```

---

## 八、常见问题排查

### 1. 模型加载失败

```bash
# 检查模型文件是否存在
ls -la /workspace/install/armor_detector/share/armor_detector/model/
```

### 2. OpenVINO 找不到

```bash
# 验证 OpenVINO 安装
ls -la /opt/intel/openvino_2024.6.0/runtime/lib/intel64/
# 应该看到 .so 文件
```

### 3. 推理速度慢

OpenVINO CPU 推理已经是优化过的。可以尝试：
- 使用更大的 confidence_threshold 减少检测数量
- 使用 NPU（如果有）

### 4. 检测结果异常

- 检查输入图像是否正常
- 检查 detect_color 参数是否正确（RED/BLUE）
- 检查 confidence_threshold 是否过低

---

## 九、性能参考

| 指标 | 数值 |
|------|------|
| 输入分辨率 | 640x640 |
| 输出维度 | 25200 x 22 |
| CPU 推理时间 | ~15-30ms/帧 |
| 内存占用 | ~200MB |

---

## 十、快速部署命令汇总

```bash
# 1. 构建镜像
cd /path/to/project
docker build -t rm_auto_aim:openvino .

# 2. 运行容器
docker run -it --name rm_auto_aim \
    --network host \
    --device /dev/video0:/dev/video0 \
    --device /dev/ttyUSB0:/dev/ttyUSB0 \
    rm_auto_aim:openvino

# 3. 编译（容器内）
cd /workspace
source /opt/ros/humble/setup.bash
source /opt/intel/openvino_2024.6.0/setupvars.sh
colcon build --symlink-install

# 4. 运行
source install/setup.bash
ros2 launch rm_bringup bringup.launch.py
```
