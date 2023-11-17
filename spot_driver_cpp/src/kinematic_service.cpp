// Copyright (c) 2023 Boston Dynamics AI Institute LLC. All rights reserved.

#include <spot_driver_cpp/kinematic_service.hpp>

#include <spot_driver_cpp/interfaces/rclcpp_logger_interface.hpp>
#include <spot_driver_cpp/kinematic_utils.hpp>

namespace spot_ros2 {

auto kServiceName = "get_inverse_kinematic_solutions";

KinematicService::KinematicService(std::shared_ptr<rclcpp::Node> node, std::shared_ptr<KinematicApi> kinematic_api,
                                   std::unique_ptr<LoggerInterfaceBase> logger)
    : node_{node}, kinematic_api_{kinematic_api}, logger_{std::move(logger)} {}

KinematicService::KinematicService(std::shared_ptr<rclcpp::Node> node, std::shared_ptr<KinematicApi> kinematic_api)
    : KinematicService{node, kinematic_api, std::make_unique<RclcppLoggerInterface>(node->get_logger())} {}

void KinematicService::init() {
  service_ = node_->create_service<GetInverseKinematicSolutions>(
      kServiceName, [this](const std::shared_ptr<GetInverseKinematicSolutions::Request> request,
                           std::shared_ptr<GetInverseKinematicSolutions::Response> response) {
        this->service_callback_(request, response);
      });
}

void KinematicService::service_callback_(const std::shared_ptr<GetInverseKinematicSolutions::Request> request,
                                         std::shared_ptr<GetInverseKinematicSolutions::Response> response) {
  auto proto_request = kinematic_utils::convert_inverse_kinematics_request_to_proto(request);
  auto expected = kinematic_api_->get_solutions(proto_request);
  if (!expected) {
    logger_->logError(std::string{"Error searching for an InverseKinematic solution: "}.append(expected.error()));
  }
  response = kinematic_utils::convert_proto_to_inverse_kinematics_response(expected.value().response);
}
}  // namespace spot_ros2
