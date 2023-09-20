// Copyright (c) 2023 Boston Dynamics AI Institute LLC. All rights reserved.

#include <rclcpp/executors.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/utilities.hpp>
#include <spot_driver_cpp/spot_image_publisher.hpp>

#include <memory>

int main(int argc, char* argv[])
{
  rclcpp::init(argc, argv);

  const auto node = std::make_shared<spot_ros2::SpotImagePublisherNode>();

  rclcpp::spin(node->get_node_base_interface());

  return 0;
}
