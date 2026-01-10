#!/bin/bash
# 优化后的 rm_watch_dog.sh

CONTAINER_ID="493a3dc7fab1"
WORKING_DIR="/workspace"
TIMEOUT=30  # 给相机和模型加载留足时间
NODE_NAMES=("armor_detector" "armor_solver" "serial_driver" "camera_driver")

function bringup() {
    echo "Starting Container: $CONTAINER_ID"
    docker start $CONTAINER_ID
    sleep 2 # 等待容器引擎就绪
    
    # 启动程序
    docker exec $CONTAINER_ID bash -c \
        "source /opt/ros/humble/setup.bash && \
         source $WORKING_DIR/install/setup.bash && \
         ros2 daemon stop && ros2 daemon start && \
         nohup ros2 launch rm_bringup bringup.launch.py > $WORKING_DIR/screen.output 2>&1 &"
    
    echo "Waiting for nodes to initialize ($TIMEOUT s)..."
    sleep $TIMEOUT
}

function restart() {
    echo "Restarting ROS2 processes..."
    docker exec $CONTAINER_ID pkill -f ros || true
    sleep 3
    bringup
}

# --- 初始启动 ---
bringup

while true; do
    for node in "${NODE_NAMES[@]}"; do
        topic="/$node/heartbeat"
        
        # 检查节点是否存在 (增加 2>/dev/null 屏蔽异常报错)
        if docker exec $CONTAINER_ID bash -c "source /opt/ros/humble/setup.bash && ros2 topic list" 2>/dev/null | grep -q "$topic"; then
            echo "$node is OK."
        else
            echo "Error: $node topic not found. Triggering restart..."
            restart
            break 
        fi
    done
    sleep 10 # 每次巡检间隔
done