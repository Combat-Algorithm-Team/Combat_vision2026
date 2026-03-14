# ============================
# Base: ROS2 Humble 官方镜像
# ============================
FROM ros:humble

SHELL ["/bin/bash", "-c"]
ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC
ENV LANG=en_US.UTF-8
ENV LC_ALL=en_US.UTF-8

# ============================
# 基础工具 + C++开发工具
# ============================
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl wget gnupg2 lsb-release dirmngr apt-transport-https \
    vim tmux zsh fzf htop unzip p7zip-full \
    usbutils net-tools iputils-ping \
    build-essential gcc g++ \
    file bsdmainutils procps git \
    software-properties-common \
    python3 python3-pip python3-venv \
    locales \
    && locale-gen en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

# ============================
# 安装最新 CMake
# ============================
RUN wget https://github.com/Kitware/CMake/releases/download/v3.27.7/cmake-3.27.7-linux-x86_64.sh \
    -O /tmp/cmake.sh && \
    sh /tmp/cmake.sh --skip-license --prefix=/usr/local && \
    rm /tmp/cmake.sh

# ============================
# 添加 ROS2 Humble apt 源 & 初始化 rosdep
# ============================
RUN apt-get update && apt-get install -y curl gnupg2 lsb-release software-properties-common python3-rosdep python3-rosinstall-generator python3-vcstool build-essential \
    && curl -sSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.asc | apt-key add - \
    && echo "deb http://packages.ros.org/ros2/ubuntu $(lsb_release -cs) main" > /etc/apt/sources.list.d/ros2.list \
    && apt-get update \
    && rosdep init || true \
    && rosdep update

# ============================
# 安装 C++ 依赖库（不安装 libceres-dev，使用旧版源码编译）
# ============================
RUN apt-get update && apt-get install -y \
    libfmt-dev libeigen3-dev libspdlog-dev libsuitesparse-dev \
    qtdeclarative5-dev qt5-qmake libqglviewer-dev-qt5 \
    libgoogle-glog-dev libgflags-dev \
    && rm -rf /var/lib/apt/lists/*

# ============================
# 下载并编译旧版 Ceres (1.14.0)
# ============================
RUN cd /tmp && \
    wget http://ceres-solver.org/ceres-solver-1.14.0.tar.gz && \
    tar xzf ceres-solver-1.14.0.tar.gz && \
    cd ceres-solver-1.14.0 && mkdir build && cd build && \
    cmake .. -DBUILD_TESTING=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/ceres-solver-1.14.0*

# ============================
# 复制本地源码到镜像
# ============================
COPY Sophus /tmp/Sophus
COPY g2o /tmp/g2o

# ============================
# 编译 Sophus
# ============================
RUN cd /tmp/Sophus && mkdir build && cd build && \
    cmake .. && make -j$(nproc) && make install && \
    rm -rf /tmp/Sophus

# ============================
# 编译 g2o
# ============================
RUN cd /tmp/g2o && mkdir build && cd build && \
    cmake .. && make -j$(nproc) && make install && \
    rm -rf /tmp/g2o

# ============================
# 下载并安装 OpenVINO
# ============================
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

# ============================
# Zsh 自动加载 ROS2
# ============================
RUN echo "source /opt/ros/humble/setup.zsh" >> /root/.zshrc && \
    echo "source /opt/ros/humble/setup.bash" >> /root/.bashrc && \
    chsh -s /usr/bin/zsh || true

# ============================
# 安装 ROS2 Humble 系统依赖
# ============================
RUN apt-get update && apt-get install -y \
    ros-humble-image-transport \
    ros-humble-image-transport-plugins \
    ros-humble-camera-info-manager \
    ros-humble-camera-calibration \
    ros-humble-xacro \
    ros-humble-ament-cmake-clang-format \
    ros-humble-cv-bridge \
    ros-humble-vision-opencv \
    ros-humble-serial-driver \
    ros-humble-angles \
    && rm -rf /var/lib/apt/lists/*

# ============================
# 默认进入 zsh
# ============================
WORKDIR /root
CMD ["/usr/bin/zsh"]
