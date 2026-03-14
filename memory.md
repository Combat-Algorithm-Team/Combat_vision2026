# OpenVINO 部署与问题记录

## 概述

本项目将远程仓库的 OpenVINO 装甲板检测模型推理代码合并到本地 `/home/jwj-rune/auto_aim_runtime/vv/` 目录。

## 环境信息

- **ROS2 版本**: Humble
- **Docker 容器**: 493a3dc7fab1
- **OpenVINO 版本**: 2024.6.0
- **安装路径**: `/opt/intel/openvino_2024.6.0/`

## 已完成的工作

### 1. 合并远程代码

从 `origin/copilot/add-armor-detection-inference` 分支合并了装甲板检测相关文件到 `vv/src/`。

### 2. Docker 容器内安装 OpenVINO

在 Dockerfile 中添加了以下步骤：
```dockerfile
# 下载并安装 OpenVINO
RUN mkdir -p /opt/intel && \
    cd /tmp && \
    curl -L ... \
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
```

### 3. 代码修改

#### CMakeLists.txt 修改
- 手动设置 G2O 库路径
- 手动设置 OpenVINO 路径

文件位置: `vv/src/rm_auto_aim/armor_detector/CMakeLists.txt`

```cmake
# 手动设置 G2O 库路径
set(G2O_LIBRARIES
    /usr/local/lib/libg2o_core.so
    /usr/local/lib/libg2o_stuff.so
    /usr/local/lib/libg2o_solver_csparse.so
    /usr/local/lib/libg2o_types_sba.so
    /usr/local/lib/libg2o_types_slam3d.so
    /usr/local/lib/libg2o_solver_dense.so
)
set(G2O_INCLUDE_DIRS /usr/local/include)

# 手动设置 OpenVINO 路径
set(OpenVINO_DIR /opt/intel/openvino_2024.6.0)
set(OpenVINO_INCLUDE_DIRS ${OpenVINO_DIR}/runtime/include)
set(OpenVINO_LIBRARY_DIRS ${OpenVINO_DIR}/runtime/lib/intel64)
```

#### armor_detector_node.cpp 修改
- 模型路径: `mlp.onnx` → `0526.onnx`

文件位置: `vv/src/rm_auto_aim/armor_detector/src/armor_detector_node.cpp:290`

#### armor_detector_params.yaml 修改
- 修复拼写错误: `0526.onnxx` → `0526.onnx`

文件位置: `vv/src/rm_bringup/config/node_params/armor_detector_params.yaml:11`

#### model_detector.cpp 修改
- 添加 FP16 输入支持

文件位置: `vv/src/rm_auto_aim/armor_detector/src/model_detector.cpp`

## 模型信息

- **模型文件**: `0526.onnx`
- **模型路径**: `/home/jwj-rune/auto_aim_runtime/vv/src/rm_auto_aim/armor_detector/model/0526.onnx`
- **输入名称**: `images`
- **输出名称**: `output`
- **输入形状**: [1, 3, 640, 640]
- **输入数据类型**: float16 (FP16)
- **输出形状**: [1, 25200, 22] 或 [1, 22, 25200]
- **输出数据类型**: float32

### 22维输出格式

| 索引 | 内容 |
|------|------|
| 0-7 | 4个关键点坐标 (x1,y1,x2,y2,x3,y3,x4,y4) |
| 8 | objectness (需要sigmoid) |
| 9-12 | 颜色分类 (红、蓝、灰、紫) |
| 13-21 | 数字分类 (G,1,2,3,4,5,O,Bs,Bb) |

### OpenVINO 加载方式

```cpp
ov::Core core;
auto model = core.read_model("model.onnx");  // 直接用 core.read_model() 即可
ov::CompiledModel compiled_model = core.compile_model(model, "CPU");
ov::InferRequest infer_request = compiled_model.create_infer_request();
```

### 预处理说明 (重要)

当前 TensorRT 流程:
1. resize 到 640x640
2. /255.0 归一化到 [0,1]
3. HWC->CHW，R/G/B 分离存储
4. (可选) FP32→FP16

OpenVINO 注意点:
- OpenVINO 的 preprocess API 可以直接在推理引擎中做 resize + 归一化，但不支持自定义的 HWC→CHW 分离存储
- **建议**: 在 CPU 端用 OpenCV 预处理成 [1,3,640,640] 的连续 CHW float 数组，再输入 OpenVINO

```cpp
// OpenVINO 预处理示例
ov::preprocess::PrePostProcessor ppp(model);
ppp.input().tensor()
    .set_layout("NHWC")
    .set_color_format(ov::preprocess::ColorFormat::BGR)
    .set_spatial_dynamic_shape();
ppp.input().preprocess()
    .convert_element_type(ov::element::f32)
    .scale(255.0f);
ppp.input().model().set_layout("NCHW");
```

### 后处理说明

22维输出解码逻辑可直接复用，与 TensorRT 版本完全相同。



| OpenVINO |
|----------|----------|
| ov::Tensor (内部管理) |
|  infer() 同步，或 create_infer_request() 异步 |
|  InferRequest 队列 |

### 精度说明

- **OpenVINO 默认 FP32**
- **CPU 不支持 FP16**（这是关键点！）
- 如需 FP16 加速需要使用 Intel GPU (integrated) 或 NPU
- 可通过 `compile_model(model, "GPU", opts)` 启用 GPU 插件

## 待解决问题

### FP16 输入处理问题 (已解决思路)

**关键发现**: 模型输入是 FP16，但 **CPU 不支持 FP16**！

当前代码尝试用 FP16 输入但失败了。解决方案：**强制使用 FP32 输入**。

#### 方案1: 让 OpenVINO 自动转换

最简单的方法是在模型编译时指定设备为 CPU，OpenVINO 会自动处理输入类型转换。代码不需要特别处理 FP16，OpenVINO 会自动将 float32 输入转换为模型需要的格式。

#### 方案2: 使用 PrePostProcessor (推荐)

```cpp
// 在 ModelDetector 构造函数中添加预处理
ov::preprocess::PrePostProcessor ppp(compiled_model_);
ppp.input().tensor()
    .set_layout("NCHW");
ppp.input().preprocess()
    .convert_element_type(ov::element::f32)
    .scale(255.0f, {1.0f, 1.0f, 1.0f});
ppp.input().model()
    .set_layout("NCHW");
```

#### 方案3: 强制使用 FP32 输入 (最简单)

在编译模型时强制使用 FP32:

```cpp
// 在 ModelDetector 构造函数中
ov::Core core;
auto model = core.read_model(model_path);

// 创建一个 FP32 输入的模型副本
for (auto& input : model->inputs()) {
    input.get_node()->set_element_type(ov::element::f32);
}

compiled_model_ = core.compile_model(model, "CPU");
```

### 当前代码问题

当前代码尝试将 float 数据写入 FP16 tensor，但方法不正确:

```cpp
// 当前错误代码
ov::Tensor tensor_fp16(ov::element::f16, input_shape);
float* dst_ptr = tensor_fp16.data<float>();  // 错误！FP16 tensor 不能用 float* 访问
```

**正确做法**: 直接使用 FP32 输入，让 OpenVINO 自动处理，或者使用 PrePostProcessor 进行预处理。

### 参考实现 (别人写的版本)

这是一个经过验证的完整实现方案：

```cpp
#include "OpenvinoInfer.h"

OpenvinoInfer::OpenvinoInfer(string model_path_xml, string model_path_bin, string device){
    input_shape = {1, static_cast<unsigned long>(IMAGE_HEIGHT), static_cast<unsigned long>(IMAGE_WIDTH), 3};
    model = core.read_model(model_path_xml, model_path_bin);

    // Step . Initialize Preprocessing for the model
    ppp = new ov::preprocess::PrePostProcessor(model);

    // Specify input image format
    ppp->input().tensor()
        .set_element_type(ov::element::u8)  // 使用 u8 输入
        .set_layout("NHWC")
        .set_color_format(ov::preprocess::ColorFormat::BGR);

    // Specify preprocess pipeline
    ppp->input().preprocess()
        .convert_element_type(ov::element::f32)
        .convert_color(ov::preprocess::ColorFormat::RGB)
        .scale({255., 255., 255.});  // 自动归一化

    // Specify model's input layout
    ppp->input().model().set_layout("NCHW");

    // Specify output results format
    ppp->output().tensor().set_element_type(ov::element::f32);

    // Embed above steps in the graph
    model = ppp->build();
    compiled_model = core.compile_model(model, device);
}

void OpenvinoInfer::infer(Mat img, int detect_color){
    // 直接使用 cv::Mat 数据创建输入 tensor
    uchar* input_data = (uchar *)img.data;
    ov::Tensor input_tensor = ov::Tensor(
        compiled_model.input().get_element_type(),
        compiled_model.input().get_shape(),
        input_data
    );

    // 推理
    ov::InferRequest infer_request = compiled_model.create_infer_request();
    infer_request.set_input_tensor(input_tensor);
    infer_request.infer();

    // 获取输出
    auto output = infer_request.get_output_tensor(0);
    ov::Shape output_shape = output.get_shape();

    // 输出格式: [1, 25200, 22] 或 [1, 22, 25200]
    cv::Mat output_buffer(output_shape[1], output_shape[2], CV_32F, output.data());

    // 后处理 (22维输出解析)
    float conf_threshold = 0.65;
    float nms_threshold = 0.45;

    for (int i = 0; i < output_buffer.rows; i++) {
        float confidence = sigmoid(output_buffer.at<float>(i, 8));
        if (confidence < conf_threshold) continue;

        // 颜色分类 (索引 9-12)
        cv::Mat color_scores = output_buffer.row(i).colRange(9, 13);
        cv::Point color_id;
        double score_color;
        cv::minMaxLoc(color_scores, NULL, &score_color, NULL, &color_id);

        // 数字分类 (索引 13-22)
        cv::Mat classes_scores = output_buffer.row(i).colRange(13, 22);
        cv::Point class_id;
        double score_num;
        cv::minMaxLoc(classes_scores, NULL, &score_num, NULL, &class_id);

        // 颜色过滤逻辑
        if(color_id.x == 2 || color_id.x == 3) continue;  // 跳过 gray/purple
        if(detect_color == 0 && color_id.x == 1) continue;  // 检测蓝色
        if(detect_color == 1 && color_id.x == 0) continue;  // 检测红色

        // 关键点 (索引 0-7)
        float x1 = output_buffer.at<float>(i, 0);
        float y1 = output_buffer.at<float>(i, 1);
        // ... 其他关键点
    }

    // NMS
    cv::dnn::NMSBoxes(boxes, confidences, conf_threshold, nms_threshold, indices);
}
```

**关键要点**:
1. **输入使用 u8**: `set_element_type(ov::element::u8)`，而不是 FP16 或 FP32
2. **PrePostProcessor 自动处理**: 归一化、类型转换都在 OpenVINO 内部完成
3. **直接用 cv::Mat 数据**: `ov::Tensor(element_type, shape, mat.data)`
4. **输出是 FP32**: 直接用 `output.data()` 获取 float 指针

## 编译与运行

### 编译命令
```bash
export OPENVINO_DIR=/opt/intel/openvino_2024.6.0
export LD_LIBRARY_PATH=${OPENVINO_DIR}/runtime/lib/intel64:${LD_LIBRARY_PATH}
cd /workspace
source /opt/ros/humble/setup.bash
colcon build --symlink-install --packages-select armor_detector
```

### 运行命令
```bash
cd /workspace
source install/setup.bash
ros2 launch rm_bringup bringup.launch.py
```

### 验证 OpenVINO 加载
日志中应显示: `Using model-based detector (OpenVINO)`

## 相关文件路径

| 文件 | 路径 |
|------|------|
| 模型 | `/home/jwj-rune/auto_aim_runtime/vv/src/rm_auto_aim/armor_detector/model/0526.onnx` |
| 源代码 | `/home/jwj-rune/auto_aim_runtime/vv/src/rm_auto_aim/armor_detector/src/model_detector.cpp` |
| CMakeLists | `/home/jwj-rune/auto_aim_runtime/vv/src/rm_auto_aim/armor_detector/CMakeLists.txt` |
| Dockerfile | `/home/jwj-rune/auto_aim_runtime/dockerfile` |
| 参数配置 | `/home/jwj-rune/auto_aim_runtime/vv/src/rm_bringup/config/node_params/armor_detector_params.yaml` |

## 验证 OpenVINO 可用性的方法

### 1. Python 验证
```bash
docker exec 493a3dc7fab1 bash -c "
source /opt/intel/openvino_2024.6.0/setupvars.sh
python3 -c \"
import openvino as ov
core = ov.Core()
model = core.read_model('/workspace/src/rm_auto_aim/armor_detector/model/0526.onnx')
print('Inputs:')
for i in model.inputs:
    print(f'  {i.get_any_name()}: shape={i.get_shape()}, type={i.get_element_type()}')
print('Outputs:')
for o in model.outputs:
    print(f'  {o.get_any_name()}: shape={o.get_shape()}, type={o.get_element_type()}')
\"
"
```

### 2. 查看模型详细信息
```bash
docker exec 493a3dc7fab1 bash -c "
source /opt/intel/openvino_2024.6.0/setupvars.sh
${OPENVINO_DIR}/runtime/bin/intel64/benchmark_app -help
"
```

## 注意事项

1. 运行时需要设置环境变量:
   ```bash
   export OPENVINO_DIR=/opt/intel/openvino_2024.6.0
   export LD_LIBRARY_PATH=${OPENVINO_DIR}/runtime/lib/intel64:${LD_LIBRARY_PATH}
   ```

2. 模型文件 0526.onnx 需要在 `install/armor_detector/share/armor_detector/model/` 目录下

3. 参数配置中 `use_model_detector: true` 需要设置为 true 才能启用模型检测

## 调试建议

### 设置环境变量查看优化信息
```bash
export OV_PRINT_PO=1
```

### 使用 benchmark_app 测试吞吐
```bash
${OPENVINO_DIR}/runtime/bin/intel64/benchmark_app -model model.onnx -d CPU
```

### ONNX 输入布局确认
代码里是动态检测的，确保输入是 NCHW 格式。

设备检测结果
项目	信息
CPU	Intel Core i7-13620H (13代)
核心数	16 核
内存	31GB
操作系统	Ubuntu 22.04.5 LTS

openVINO 可用设备

Available devices: 
  - CPU
