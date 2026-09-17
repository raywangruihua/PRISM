// EncryptedPublisher node: subscribes to the raw camera Image, encrypts the
// raw pixel buffer with AES-128-GCM, and republishes a sensor_msgs/Image whose
// data is [IV:12][TAG:16][CIPHERTEXT]. This matches what DecryptedPublisher
// (CA) forwards into the TA, and the key matches the TA's key.
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>

#define IMAGE_WIDTH  640
#define IMAGE_HEIGHT 480
#define IMAGE_SIZE   (IMAGE_WIDTH * IMAGE_HEIGHT * 3)

namespace {

// Hardcoded keys for demo only
constexpr std::array<uint8_t, 16> kAesKey = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

constexpr size_t kIvLen  = 12;
constexpr size_t kTagLen = 16;

// Encrypt `len` bytes -> [IV(12)][TAG(16)][CIPHERTEXT(len)].
std::vector<uint8_t> aes_gcm_encrypt(const uint8_t* pt, size_t len) {
  std::array<uint8_t, kIvLen> iv{};
  if (RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1)
    throw std::runtime_error("RAND_bytes failed");   // fresh nonce per frame

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
  struct Guard { EVP_CIPHER_CTX* c; ~Guard() { EVP_CIPHER_CTX_free(c); } } g{ctx};

  if (EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(kIvLen), nullptr) != 1 ||
      EVP_EncryptInit_ex(ctx, nullptr, nullptr, kAesKey.data(), iv.data()) != 1)
    throw std::runtime_error("EncryptInit failed");

  std::vector<uint8_t> ct(len);
  int out_len = 0, total = 0;
  if (len > 0) {
    if (EVP_EncryptUpdate(ctx, ct.data(), &out_len, pt,
                          static_cast<int>(len)) != 1)
      throw std::runtime_error("EncryptUpdate failed");
    total = out_len;
  }
  if (EVP_EncryptFinal_ex(ctx, ct.data() + total, &out_len) != 1)
    throw std::runtime_error("EncryptFinal failed");
  total += out_len;
  ct.resize(static_cast<size_t>(total));

  std::array<uint8_t, kTagLen> tag{};
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                          static_cast<int>(kTagLen), tag.data()) != 1)
    throw std::runtime_error("get tag failed");

  std::vector<uint8_t> out;
  out.reserve(kIvLen + kTagLen + ct.size());
  out.insert(out.end(), iv.begin(), iv.end());
  out.insert(out.end(), tag.begin(), tag.end());
  out.insert(out.end(), ct.begin(), ct.end());
  return out;
}

}  // namespace

class EncryptedPublisher : public rclcpp::Node {
public:
  EncryptedPublisher() : Node("encrypted_publisher") {
    sub_ = create_subscription<sensor_msgs::msg::Image>(
      "/camera/color/image_raw", rclcpp::SensorDataQoS(),
      std::bind(&EncryptedPublisher::on_image, this, std::placeholders::_1));
    pub_ = create_publisher<sensor_msgs::msg::Image>(
      "/camera/color/image_raw/encrypted", rclcpp::SensorDataQoS());
    RCLCPP_INFO(get_logger(),
      "EncryptedPublisher running: /camera/color/image_raw -> "
      "/camera/color/image_raw/encrypted (AES-128-GCM)");
  }

private:
  void on_image(sensor_msgs::msg::Image::ConstSharedPtr img) {
    if (img->data.size() != IMAGE_SIZE) {
      RCLCPP_ERROR(get_logger(),
        "raw frame %zu != expected %d (encoding '%s')",
        img->data.size(), IMAGE_SIZE, img->encoding.c_str());
      return;
    }

    try {
      auto packet = aes_gcm_encrypt(img->data.data(), img->data.size());

      sensor_msgs::msg::Image out;
      out.header   = img->header;      // preserve stamp / frame_id
      out.width    = img->width;       // envelope kept for debugging; the CA
      out.height   = img->height;      // overrides these after decrypt anyway
      out.encoding = img->encoding;
      out.is_bigendian = img->is_bigendian;
      out.step     = img->step;
      out.data     = std::move(packet);   // [IV][TAG][CIPHERTEXT]

      pub_->publish(std::move(out));
    } catch (const std::exception& e) {
      RCLCPP_ERROR(get_logger(), "encryption failed: %s", e.what());
    }
  }

  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr sub_;
  rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr pub_;
};

int main(int argc, char** argv) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<EncryptedPublisher>());
  rclcpp::shutdown();
  return 0;
}
