#!/bin/bash
# register_service.sh

USER_NAME="root" # 建议用 root 以确保 docker 操作权限，或填你的用户名
SERVICE_NAME="rm.service"
SERVICE_PATH="/etc/systemd/system/$SERVICE_NAME"

# 复制脚本到系统路径
chmod +x ./rm_clean_up.sh ./rm_watch_dog.sh
cp ./rm_clean_up.sh /usr/sbin/
cp ./rm_watch_dog.sh /usr/sbin/

# 创建 Service 文件
cat <<EOF > $SERVICE_PATH
[Unit]
Description=RoboMaster Docker Autostart
After=docker.service
Requires=docker.service

[Service]
User=$USER_NAME
Type=simple
ExecStart=/usr/sbin/rm_watch_dog.sh
ExecStop=/usr/sbin/rm_clean_up.sh
Restart=always
RestartSec=10s

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable $SERVICE_NAME
systemctl start $SERVICE_NAME

echo "Service registered and started. Use 'systemctl status $SERVICE_NAME' to check."