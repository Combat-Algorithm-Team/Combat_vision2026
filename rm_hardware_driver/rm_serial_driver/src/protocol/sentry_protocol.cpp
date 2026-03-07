// Created by Chengfu Zou
// Copyright (C) FYT Vision Group. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "rm_serial_driver/protocol/sentry_protocol.hpp"
// ros2
#include <geometry_msgs/msg/twist.hpp>

namespace fyt::serial_driver::protocol {
ProtocolSentry::ProtocolSentry(std::string_view port_name, bool enable_data_print) {
  auto uart_transporter = std::make_shared<UartTransporter>(std::string(port_name));
  packet_tool_ = std::make_shared<FixedPacketTool<64>>(uart_transporter);
  packet_tool_->enbaleDataPrint(enable_data_print);
}

void ProtocolSentry::send(const rm_interfaces::msg::GimbalCmd &data) {
  packet_.clear(); 
  // Byte 1: 开火建议
  packet_.loadData<uint8_t>(data.fire_advice ? 0x01 : 0x00, 1);
  
  // Byte 4 & 8: Gimbal Control
  packet_.loadData<float>(static_cast<float>(data.pitch / 180.0f * M_PI), 4);
  packet_.loadData<float>(static_cast<float>(data.yaw), 8);
  
  // Byte 12 & 16: 时间戳对齐 (下位机 vision_receive_t 结构体)
  packet_.loadData<uint32_t>(static_cast<uint32_t>(data.header.stamp.sec), 12);
  packet_.loadData<uint32_t>(static_cast<uint32_t>(data.header.stamp.nanosec), 16);
  // Byte 20, 24, 28: 这里的 vx, vy 等通常在自瞄包里传目标相对坐标
  packet_.loadData<float>(static_cast<float>(data.distance), 20); 

  // Byte 30: 协议包类型标识
  packet_.loadData<uint8_t>(0x01, 30); 
  packet_tool_->sendPacket(packet_);
}

// Nav serial send
void ProtocolSentry::send(const rm_interfaces::msg::ChassisCmd &data) {
  // packet_.loadData<unsigned char>(0x00, 1);
  // is_spin
  //第1位开火建议 0：不开火 1：开火
  packet_.loadData<unsigned char>(data.is_spining ? 0x01 : 0x00, 2);
  packet_.loadData<unsigned char>(data.is_navigating ? 0x01 : 0x00, 3);
  // gimbal control
  // packet_.loadData<float>(0, 4);
  // packet_.loadData<float>(0, 8);
  // packet_.loadData<float>(0, 12);
  // chassis control
  // linear x
  packet_.loadData<float>(data.twist.linear.x, 16);
  // linear y
  packet_.loadData<float>(data.twist.linear.y, 20);
  // angular z
  packet_.loadData<float>(data.twist.angular.z, 24);
  // useless data
  packet_.loadData<unsigned char>(0x02, 30);
  packet_tool_->sendPacket(packet_);
}

bool ProtocolSentry::receive(rm_interfaces::msg::SerialReceiveData &data) {
  FixedPacket64 packet;
  if (packet_tool_->recvPacket(packet)) {
    // Byte 1: 敌方颜色
    uint8_t enemy_color;
    packet.unloadData(enemy_color, 1);
    data.mode = (enemy_color == 2 ? 1 : 0); // 未开始 1：红色 2：蓝色（敌方颜色）

    // Byte 8: 当前血量 (uint16_t)
    uint16_t hp;
    packet.unloadData(hp, 8);
    data.judge_system_data.blood = hp;

    // Byte 12: Pitch (float)
    packet.unloadData<float>(data.pitch, 12);
    // Byte 16: Yaw (float)
    packet.unloadData<float>(data.yaw, 16);
    // Byte 20: 弹速 (float)
    packet.unloadData<float>(data.bullet_speed, 20);
    // Byte 24: 剩余时间 (uint16_t)
    uint16_t remain_time;
    packet.unloadData<uint16_t>(remain_time, 24);
    data.judge_system_data.remaining_time = remain_time;

    return true;
  }
  return false;
}

std::vector<rclcpp::SubscriptionBase::SharedPtr> ProtocolSentry::getSubscriptions(
  rclcpp::Node::SharedPtr node) {
  auto sub1 = node->create_subscription<rm_interfaces::msg::GimbalCmd>(
    "armor_solver/cmd_gimbal",
    rclcpp::SensorDataQoS(),
    [this](const rm_interfaces::msg::GimbalCmd::SharedPtr msg) { this->send(*msg); });
  auto sub2 = node->create_subscription<rm_interfaces::msg::GimbalCmd>(
    "rune_solver/cmd_gimbal",
    rclcpp::SensorDataQoS(),
    [this](const rm_interfaces::msg::GimbalCmd::SharedPtr msg) { this->send(*msg); });
  auto sub3 = node->create_subscription<rm_interfaces::msg::ChassisCmd>(
    "/cmd_chassis",
    rclcpp::SensorDataQoS(),
    [this](const rm_interfaces::msg::ChassisCmd::SharedPtr msg) { this->send(*msg); });
  return {sub1, sub2, sub3};
}

std::vector<rclcpp::Client<rm_interfaces::srv::SetMode>::SharedPtr> ProtocolSentry::getClients(
  rclcpp::Node::SharedPtr node) const {
  auto client1 = node->create_client<rm_interfaces::srv::SetMode>("armor_detector/set_mode",
                                                                  rmw_qos_profile_services_default);
  auto client2 = node->create_client<rm_interfaces::srv::SetMode>("armor_solver/set_mode",
                                                                  rmw_qos_profile_services_default);
  return {client1, client2};
}
}  // namespace fyt::serial_driver::protocol
