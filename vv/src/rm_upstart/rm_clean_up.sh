#!/bin/bash
# rm_clean_up.sh

CONTAINER_ID="493a3dc7fab1"

echo "Cleaning up ROS2 processes in Docker..."
# 杀掉容器内的所有 ROS2 进程
docker exec $CONTAINER_ID pkill -f ros
docker exec $CONTAINER_ID ros2 daemon stop

# 根据你的需求：你可以选择是否关闭容器
# 如果想让容器保持开启，就注释掉下面这一行
docker stop $CONTAINER_ID

pkill -f rm_watch_dog.sh