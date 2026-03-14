docker run -it  --network=host \
  --privileged \
  -v /dev:/dev \
  -v /home/jwj-rune/auto_aim_runtime/vv:/workspace  \
  --name rm_rune_infantry0_v0 \
  rm_rune_new_new:latest \
  /bin/bash

sudo rm -r build install log

colcon build --symlink-install --parallel-workers  4

以下命令一次性启动容器＋进入容器＋进入工作空间＋source环境变量：
docker start 493a3dc7fab1 && docker exec -it -w /workspace 493a3dc7fab1 bash -c "source install/setup.bash && exec bash"


docker start 493a3dc7fab1 && docker exec -it 493a3dc7fab1 /bin/bash 
cd /workspace
source install/setup.bash
ros2 launch rm_bringup bringup.launch.py

ros2 launch foxglove_bridge foxglove_bridge_launch.xml