// DecryptedPublisher node (CA): subscribes to encrypted frames, forwards the
// whole [IV|TAG|CIPHERTEXT] packet into the TA, and republishes the decrypted
// + grayscaled frame the TA hands back.
#include <cstring>
#include <functional>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>

#include <tee_client_api.h>

#include "prism/tee_raii.hpp"
#include "../../../../prism_optee/ta/include/prism_ta.h"

#define IMAGE_WIDTH  640
#define IMAGE_HEIGHT 480
#define IMAGE_SIZE   (IMAGE_WIDTH * IMAGE_HEIGHT * 3)

#define IV_LEN       12
#define TAG_LEN      16
#define PACKET_SIZE  (IV_LEN + TAG_LEN + IMAGE_SIZE)   // what arrives + goes to TA

class DecryptedPublisher : public rclcpp::Node {
public:
  DecryptedPublisher()
  : Node("decrypted_publisher"),
    ctx_(),
    sess_(ctx_, uuid_),
    shm_(ctx_, PACKET_SIZE, TEEC_MEM_INPUT | TEEC_MEM_OUTPUT)
  {
    subscription_ = this->create_subscription<sensor_msgs::msg::Image>(
      "/camera/color/image_raw/encrypted", rclcpp::SensorDataQoS(),
      std::bind(&DecryptedPublisher::image_callback, this, std::placeholders::_1));
    // Reliable output: rqt / rviz / `ros2 topic echo` subscribe as reliable by
    // default, and a reliable publisher serves both reliable and best-effort
    // subscribers. (The camera-facing subscription above stays best-effort.)
    publisher_ = this->create_publisher<sensor_msgs::msg::Image>(
      "/camera/color/image_raw/decrypted", rclcpp::QoS(10));

    RCLCPP_INFO(this->get_logger(), "Intialised");
  }

private:
  void image_callback(sensor_msgs::msg::Image::SharedPtr img) {
    // Incoming data is the full packet: [IV(12)][TAG(16)][CIPHERTEXT].
    if (img->data.size() != PACKET_SIZE) {
      RCLCPP_ERROR(this->get_logger(),
        "packet size %zu != expected %d (encoding '%s')",
        img->data.size(), PACKET_SIZE, img->encoding.c_str());
      return;
    }

    std::memcpy(shm_.buffer(), img->data.data(), PACKET_SIZE);

    TEEC_Operation op = {};
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_WHOLE, TEEC_NONE,
      TEEC_NONE, TEEC_NONE);
    op.params[0].memref.parent = &shm_.get();
    op.params[0].memref.offset = 0;
    op.params[0].memref.size = PACKET_SIZE;

    uint32_t err_origin = 0;
    TEEC_Result res = TEEC_InvokeCommand(&sess_.get(),
      TA_PROCESS_IMAGE_CMD, &op, &err_origin);
    if (res != TEEC_SUCCESS) {
      RCLCPP_ERROR(this->get_logger(),
        "TEEC_InvokeCommand failed 0x%x origin 0x%x", res, err_origin);
      return;
    }

    // The TA writes the decrypted+grayscaled frame to offset 0. The returned
    // memref.size is unreliable with MEMREF_WHOLE on some OP-TEE builds, so we
    // read the known frame size from offset 0 rather than trusting it.
    img->encoding     = "rgb8";               // grayscale stored as Y,Y,Y
    img->width        = IMAGE_WIDTH;
    img->height       = IMAGE_HEIGHT;
    img->step         = IMAGE_WIDTH * 3;
    img->is_bigendian = 0;
    img->data.resize(IMAGE_SIZE);
    std::memcpy(img->data.data(), shm_.buffer(), IMAGE_SIZE);

    publisher_->publish(*img);
  }

  // TEE variables
  TEEC_UUID uuid_ = TA_PRISM_UUID;
  tee::TEEContext ctx_;
  tee::TEESession sess_;
  tee::TEESharedMemory shm_;

  // ROS2 variables
  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr subscription_;
  rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr publisher_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<DecryptedPublisher>());
  rclcpp::shutdown();
  return 0;
}
