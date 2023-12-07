// Copyright (c) 2023 Boston Dynamics AI Institute LLC. All rights reserved.

#include <spot_driver_cpp/api/default_image_client.hpp>

#include <bosdyn/api/directory.pb.h>
#include <bosdyn/api/image.pb.h>
#include <cv_bridge/cv_bridge.h>
#include <google/protobuf/duration.pb.h>
#include <builtin_interfaces/msg/time.hpp>
#include <geometry_msgs/msg/quaternion.hpp>
#include <geometry_msgs/msg/transform_stamped.hpp>
#include <geometry_msgs/msg/vector3.hpp>
#include <opencv2/imgcodecs.hpp>
#include <sensor_msgs/distortion_models.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <spot_driver_cpp/api/default_time_sync_api.hpp>
#include <spot_driver_cpp/api/spot_image_sources.hpp>
#include <spot_driver_cpp/conversions/geometry.hpp>
#include <spot_driver_cpp/types.hpp>
#include <std_msgs/msg/header.hpp>
#include <tl_expected/expected.hpp>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {

static const std::set<std::string> kExcludedStaticTfFrames{
    // We exclude the odometry frames from static transforms since they are not static. We can ignore the body
    // frame because it is a child of odom or vision depending on the preferred_odom_frame, and will be published
    // by the non-static transform publishing that is done by the state callback
    "body",
    "odom",
    "vision",

    // Special case handling for hand camera frames that reference the link "arm0.link_wr1" in their transform
    // snapshots. This name only appears in hand camera transform snapshots and is a known bug in the Spot API.
    // We exclude publishing a static transform from arm0.link_wr1 -> body here because it depends
    // on the arm's position and a static transform would fix it to its initial position.
    "arm0.link_wr1",
};

tl::expected<int, std::string> getCvPixelFormat(const bosdyn::api::Image_PixelFormat& format) {
  switch (format) {
    case bosdyn::api::Image_PixelFormat_PIXEL_FORMAT_RGB_U8: {
      return CV_8UC3;
    }
    case bosdyn::api::Image_PixelFormat_PIXEL_FORMAT_RGBA_U8: {
      return CV_8UC4;
    }
    case bosdyn::api::Image_PixelFormat_PIXEL_FORMAT_GREYSCALE_U8: {
      return CV_8UC1;
    }
    case bosdyn::api::Image_PixelFormat_PIXEL_FORMAT_GREYSCALE_U16: {
      return CV_16UC1;
    }
    case bosdyn::api::Image_PixelFormat_PIXEL_FORMAT_DEPTH_U16: {
      return CV_16UC1;
    }
    default: {
      return tl::make_unexpected("Unknown pixel format.");
    }
  }
}

tl::expected<sensor_msgs::msg::CameraInfo, std::string> toCameraInfoMsg(
    const bosdyn::api::ImageResponse& image_response, const std::string& robot_name,
    const google::protobuf::Duration& clock_skew) {
  sensor_msgs::msg::CameraInfo info_msg;
  info_msg.distortion_model = sensor_msgs::distortion_models::PLUMB_BOB;
  info_msg.height = image_response.shot().image().rows();
  info_msg.width = image_response.shot().image().cols();
  // Omit leading `/` from frame ID if robot_name is empty
  info_msg.header.frame_id =
      (robot_name.empty() ? "" : robot_name + "/") + image_response.shot().frame_name_image_sensor();
  info_msg.header.stamp = spot_ros2::applyClockSkew(image_response.shot().acquisition_time(), clock_skew);

  // We assume that the camera images have already been corrected for distortion, so the 5 distortion parameters are all
  // zero.
  info_msg.d = std::vector<double>{0.0, 0.0, 0.0, 0.0, 0.0};

  // Set the rectification matrix to identity, since this is not a stereo pair.
  info_msg.r[0] = 1.0;
  info_msg.r[1] = 0.0;
  info_msg.r[2] = 0.0;
  info_msg.r[3] = 0.0;
  info_msg.r[4] = 1.0;
  info_msg.r[5] = 0.0;
  info_msg.r[6] = 0.0;
  info_msg.r[7] = 0.0;
  info_msg.r[8] = 1.0;

  const auto& intrinsics = image_response.source().pinhole().intrinsics();

  // Create the 3x3 intrinsics matrix.
  info_msg.k[0] = intrinsics.focal_length().x();
  info_msg.k[2] = intrinsics.principal_point().x();
  info_msg.k[4] = intrinsics.focal_length().y();
  info_msg.k[5] = intrinsics.principal_point().y();
  info_msg.k[8] = 1.0;

  // All Spot cameras are functionally monocular, so Tx and Ty are not set here.
  info_msg.p[0] = intrinsics.focal_length().x();
  info_msg.p[2] = intrinsics.principal_point().x();
  info_msg.p[5] = intrinsics.focal_length().y();
  info_msg.p[6] = intrinsics.principal_point().y();
  info_msg.p[10] = 1.0;

  return info_msg;
}

tl::expected<sensor_msgs::msg::Image, std::string> toImageMsg(const bosdyn::api::ImageCapture& image_capture,
                                                              const std::string& robot_name,
                                                              const google::protobuf::Duration& clock_skew) {
  const auto& image = image_capture.image();
  auto data = image.data();

  std_msgs::msg::Header header;
  // Omit leading `/` from frame ID if robot_name is empty
  header.frame_id = (robot_name.empty() ? "" : robot_name + "/") + image_capture.frame_name_image_sensor();
  header.stamp = spot_ros2::applyClockSkew(image_capture.acquisition_time(), clock_skew);

  const auto pixel_format_cv = getCvPixelFormat(image.pixel_format());
  if (!pixel_format_cv) {
    return tl::make_unexpected("Failed to determine pixel format: " + pixel_format_cv.error());
  }

  if (image.format() == bosdyn::api::Image_Format_FORMAT_JPEG) {
    // When the image is JPEG-compressed, it is represented as a 1 x (width * height) row of bytes.
    // First we create a cv::Mat which contains the compressed image data...
    const cv::Mat img_compressed{1, image.rows() * image.cols(), CV_8UC1, &data.front()};
    // Then we decode it to extract the raw image into a new cv::Mat.
    // Note: this assumes that if an image is provided as JPEG-compressed data, then it is an RGB image.
    const cv::Mat img_bgr = cv::imdecode(img_compressed, cv::IMREAD_COLOR);
    if (!img_bgr.data) {
      return tl::make_unexpected("Failed to decode JPEG-compressed image.");
    }
    const auto image = cv_bridge::CvImage{header, "bgr8", img_bgr}.toImageMsg();
    return *image;
  } else if (image.format() == bosdyn::api::Image_Format_FORMAT_RAW) {
    // Note: as currently implemented, this assumes that the only images which will be provided as raw data will be
    // 16UC1 depth images.
    // TODO(jschornak-bdai): handle converting raw RGB and grayscale images as well.
    const cv::Mat img = cv::Mat(image.rows(), image.cols(), pixel_format_cv.value(), &data.front());
    if (!img.data) {
      return tl::make_unexpected("Failed to decode raw-formatted image.");
    }
    const auto image = cv_bridge::CvImage{header, sensor_msgs::image_encodings::TYPE_16UC1, img}.toImageMsg();
    return *image;
  } else if (image.format() == bosdyn::api::Image_Format_FORMAT_RLE) {
    return tl::make_unexpected("Conversion from FORMAT_RLE is not yet implemented.");
  } else {
    return tl::make_unexpected("Unknown image format.");
  }
}

tl::expected<std::vector<geometry_msgs::msg::TransformStamped>, std::string> getImageTransforms(
    const bosdyn::api::ImageResponse& image_response, const std::string& robot_name,
    const google::protobuf::Duration& clock_skew) {
  std::vector<geometry_msgs::msg::TransformStamped> out;
  for (const auto& [child_frame_id, transform] :
       image_response.shot().transforms_snapshot().child_to_parent_edge_map()) {
    // Do not publish static transforms for excluded frames
    if (kExcludedStaticTfFrames.count(child_frame_id) > 0) {
      continue;
    }

    // Rename the parent link "arm0.link_wr1" to "link_wr1" as it appears in robot state
    // which is used for publishing dynamic tfs elsewhere. Without this, the hand camera frame
    // positions would never properly update as no other pipelines reference "arm0.link_wr1".
    const auto parent_frame_id =
        (transform.parent_frame_name() == "arm0.link_wr1") ? "link_wr1" : transform.parent_frame_name();

    const auto tform_msg = spot_ros2::conversions::toTransformStamped(
        transform.parent_tform_child(), robot_name.empty() ? parent_frame_id : (robot_name + "/" + parent_frame_id),
        robot_name.empty() ? child_frame_id : (robot_name + "/" + child_frame_id),
        spot_ros2::applyClockSkew(image_response.shot().acquisition_time(), clock_skew));

    out.push_back(tform_msg);
  }
  return out;
}
}  // namespace

namespace spot_ros2 {

DefaultImageClient::DefaultImageClient(::bosdyn::client::ImageClient* image_client,
                                       std::shared_ptr<TimeSyncApi> time_sync_api, const std::string& robot_name)
    : image_client_{image_client}, time_sync_api_{time_sync_api}, robot_name_{robot_name} {}

tl::expected<GetImagesResult, std::string> DefaultImageClient::getImages(::bosdyn::api::GetImageRequest request) {
  std::shared_future<::bosdyn::client::GetImageResultType> get_image_result_future =
      image_client_->GetImageAsync(request);

  ::bosdyn::client::GetImageResultType get_image_result = get_image_result_future.get();
  if (!get_image_result.status) {
    return tl::make_unexpected("Failed to get images: " + get_image_result.status.DebugString());
  }

  const auto clock_skew_result = time_sync_api_->getClockSkew();
  if (!clock_skew_result) {
    return tl::make_unexpected("Failed to get latest clock skew: " + clock_skew_result.error());
  }

  GetImagesResult out;
  for (const auto& image_response : get_image_result.response.image_responses()) {
    const auto& image = image_response.shot().image();
    auto data = image.data();

    const auto image_msg = toImageMsg(image_response.shot(), robot_name_, clock_skew_result.value());
    if (!image_msg) {
      return tl::make_unexpected("Failed to convert SDK image response to ROS Image message: " + image_msg.error());
    }

    const auto info_msg = toCameraInfoMsg(image_response, robot_name_, clock_skew_result.value());
    if (!info_msg) {
      return tl::make_unexpected("Failed to convert SDK image response to ROS CameraInfo message: " + info_msg.error());
    }

    const auto& camera_name = image_response.source().name();
    const auto get_source_name_result = fromSpotImageSourceName(camera_name);
    if (get_source_name_result.has_value()) {
      out.images_.try_emplace(get_source_name_result.value(), ImageWithCameraInfo{image_msg.value(), info_msg.value()});
    } else {
      return tl::make_unexpected("Failed to convert API image source name to ImageSource: " +
                                 get_source_name_result.error());
    }

    const auto transforms_result = getImageTransforms(image_response, robot_name_, clock_skew_result.value());
    if (transforms_result.has_value()) {
      out.transforms_.insert(out.transforms_.end(), transforms_result.value().begin(), transforms_result.value().end());
    } else {
      return tl::make_unexpected("Failed to get image transforms: " + transforms_result.error());
    }
  }

  return out;
}

}  // namespace spot_ros2
