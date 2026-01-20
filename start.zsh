docker run -it  --network=host \
  --privileged \
  -v /dev:/dev \
  -v /home/jwj-rune/auto_aim_runtime/vv:/workspace  \
  --name vision_sys_ver2 \
  combat_vision2026_ver2:latest \
  /bin/bash

  
docker start 493a3dc7fab1
cd /workspace
source install/setup.bash
ros2 launch rm_bringup bringup.launch.py

ros2 launch foxglove_bridge foxglove_bridge_launch.xml