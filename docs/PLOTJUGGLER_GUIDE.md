# PlotJuggler 实时绘制 EKF 状态曲线指南

本文档说明如何使用 PlotJuggler 实时可视化 `standard` 程序发送的 EKF 状态数据。

## 前提条件

### 1. 安装 PlotJuggler（宿主机）

在 Ubuntu 宿主机上安装：

```bash
sudo apt install plotjuggler
```

或者从 [PlotJuggler GitHub](https://github.com/facontidavide/PlotJuggler) 下载最新版本。

### 2. 确保 Docker 容器能访问宿主机网络

有两种方式：

#### 方式 A：使用 `host.docker.internal`（推荐）

Docker Desktop 或较新版本的 Docker 支持 `host.docker.internal` 作为宿主机地址。

在 `configs/sentry.yaml` 中已默认配置：

```yaml
plotter_host: "host.docker.internal"
plotter_port: 9870
```

如果您的 Docker 版本不支持该特性，可以在运行容器时添加：

```bash
docker run --add-host=host.docker.internal:host-gateway \
  --device=/dev/video0 \
  -v /dev:/dev \
  -v $(pwd):/app \
  --privileged \
  your_image_name
```

#### 方式 B：使用 `--network host`（更简单但共享所有端口）

直接让容器使用宿主机网络栈：

```bash
docker run --network host \
  --device=/dev/video0 \
  -v /dev:/dev \
  -v $(pwd):/app \
  --privileged \
  your_image_name
```

如果使用此方式，需修改 `configs/sentry.yaml`：

```yaml
plotter_host: "127.0.0.1"  # 因为容器直接在宿主机网络上
plotter_port: 9870
```

## 使用步骤

### 步骤 1：启动 PlotJuggler

在宿主机上运行：

```bash
plotjuggler
```

### 步骤 2：配置 UDP 数据源

1. 在 PlotJuggler 窗口中，点击菜单 **Streaming → Start: UDP Server**
2. 在弹出的对话框中：
   - **Port**: `9870`（与配置文件中 `plotter_port` 一致）
   - **Protocol**: 选择 **JSON**
   - 点击 **OK**

### 步骤 3：启动容器内的 standard 程序

在 Docker 容器内编译并运行：

```bash
# 编译
make -C build/ -j$(nproc)

# 运行
./build/standard configs/sentry.yaml
```

程序启动后会输出：

```
[info] Plotter configured to send UDP to host.docker.internal:9870
```

### 步骤 4：在 PlotJuggler 中查看数据

1. 左侧的 **Timeseries List** 面板会显示接收到的数据字段：
   - `ekf_x` - X 轴位置 (m)
   - `ekf_vx` - X 轴速度 (m/s)
   - `ekf_y` - Y 轴位置 (m)
   - `ekf_vy` - Y 轴速度 (m/s)
   - `ekf_z` - Z 轴位置 (m)
   - `ekf_vz` - Z 轴速度 (m/s)
   - `ekf_yaw` - 偏航角 (rad)
   - `ekf_w` - 偏航角速度 (rad/s)
   - `frame_id` - 帧 ID

2. 将感兴趣的字段拖拽到右侧绘图区域
3. PlotJuggler 会实时更新曲线

### 步骤 5：保存布局（可选）

配置好可视化布局后，可以保存以便下次使用：

1. 点击 **File → Save Layout**
2. 下次启动 PlotJuggler 时，点击 **File → Load Layout** 恢复

## 数据字段说明

根据 `src/standard.cpp` 中的注释，EKF 状态向量的含义为：

```
[x, vx, y, vy, z, vz, a, w, r, l, h]
```

当前发送到 PlotJuggler 的字段：

| 字段名 | 说明 | 单位 |
|--------|------|------|
| `ekf_x` | 目标在世界系下的 X 坐标 | m |
| `ekf_vx` | 目标在世界系下的 X 方向速度 | m/s |
| `ekf_y` | 目标在世界系下的 Y 坐标（高度） | m |
| `ekf_vy` | 目标在世界系下的 Y 方向速度 | m/s |
| `ekf_z` | 目标在世界系下的 Z 坐标 | m |
| `ekf_vz` | 目标在世界系下的 Z 方向速度 | m/s |
| `ekf_yaw` | 目标偏航角 | rad |
| `ekf_w` | 目标偏航角速度 | rad/s |
| `frame_id` | 当前帧编号 | - |

## 故障排查

### 问题 1：PlotJuggler 没有接收到数据

**检查项**：

1. 确认 PlotJuggler 的 UDP Server 已启动，端口为 9870
2. 检查防火墙是否阻止了 UDP 9870 端口：
   ```bash
   sudo ufw allow 9870/udp
   ```
3. 在容器内测试网络连通性：
   ```bash
   # 进入容器
   docker exec -it <container_id> bash
   
   # 测试 UDP 发送
   echo "test" | nc -u host.docker.internal 9870
   ```
4. 查看程序日志，确认 Plotter 配置信息

### 问题 2：host.docker.internal 无法解析

**解决方案**：

使用宿主机的实际 IP 地址，修改 `configs/sentry.yaml`：

```yaml
plotter_host: "192.168.1.100"  # 替换为您的宿主机 IP
plotter_port: 9870
```

查找宿主机 IP：

```bash
ip addr show | grep "inet "
```

### 问题 3：数据延迟或丢失

**优化建议**：

1. 降低数据发送频率（修改 `src/standard.cpp` 中的条件）
2. 增加网络缓冲区大小
3. 使用 `--network host` 减少网络开销

## 推荐的 PlotJuggler 布局

建议创建 3 个子图：

1. **位置曲线**：`ekf_x`, `ekf_y`, `ekf_z`
2. **速度曲线**：`ekf_vx`, `ekf_vy`, `ekf_vz`
3. **姿态曲线**：`ekf_yaw`, `ekf_w`

## 扩展：添加更多数据字段

如需绘制更多数据（如装甲板检测置信度、云台角度等），在 `src/standard.cpp` 中的 JSON 对象添加相应字段：

```cpp
nlohmann::json j;
j["ekf_x"] = x[0];
// ... 其他字段
j["bullet_speed"] = bullet_speed;  // 添加新字段
j["yaw_command"] = command.yaw;
plotter.plot(j);
```

## 参考资源

- [PlotJuggler 官方文档](https://github.com/facontidavide/PlotJuggler)
- [Docker 网络配置](https://docs.docker.com/network/)
