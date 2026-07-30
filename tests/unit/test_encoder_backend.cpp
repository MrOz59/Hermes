/**
 * @file tests/unit/test_encoder_backend.cpp
 * @brief Headless tests for the injectable Hermes encoder boundary.
 */

#include "../tests_common.h"

#include <chrono>
#include <memory>
#include <optional>
#include <src/encoder_backend.h>

namespace {

  class fake_image_t final:
      public platf::img_t {
  };

  class fake_display_t final:
      public platf::display_t {
  public:
    platf::capture_e capture(
      const push_captured_image_cb_t &,
      const pull_free_image_cb_t &,
      bool *
    ) override {
      return platf::capture_e::ok;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      return nullptr;
    }

    int dummy_img(platf::img_t *) override {
      return 0;
    }
  };

  class fake_encode_device_t final:
      public platf::encode_device_t {
  public:
    int convert(platf::img_t &img) override {
      ++convert_calls;
      last_image = &img;
      return convert_result;
    }

    platf::img_t *last_image = nullptr;
    int convert_result = 9;
    int convert_calls = 0;
  };

  class fake_encode_session_t final:
      public video::encode_session_t {
  public:
    explicit fake_encode_session_t(
      std::unique_ptr<platf::encode_device_t> device
    ):
        device {std::move(device)} {
    }

    int convert(platf::img_t &img) override {
      ++convert_calls;
      return device ? device->convert(img) : -1;
    }

    int encode_frame(
      int64_t frame_number,
      safe::mail_raw_t::queue_t<video::packet_t> &packets,
      void *channel_data,
      std::optional<std::chrono::steady_clock::time_point> frame_timestamp
    ) override {
      ++encode_calls;
      last_frame_number = frame_number;
      last_packets = &packets;
      last_channel_data = channel_data;
      last_frame_timestamp = frame_timestamp;
      return encode_result;
    }

    void request_idr_frame() override {
      ++idr_requests;
    }

    void request_normal_frame() override {
      ++normal_requests;
    }

    void invalidate_ref_frames(
      int64_t first_frame,
      int64_t last_frame
    ) override {
      ++invalidation_requests;
      invalidation_first = first_frame;
      invalidation_last = last_frame;
    }

    std::unique_ptr<platf::encode_device_t> device;
    safe::mail_raw_t::queue_t<video::packet_t> *last_packets = nullptr;
    void *last_channel_data = nullptr;
    std::optional<std::chrono::steady_clock::time_point>
      last_frame_timestamp;
    int64_t last_frame_number = 0;
    int64_t invalidation_first = 0;
    int64_t invalidation_last = 0;
    int encode_result = 17;
    int convert_calls = 0;
    int encode_calls = 0;
    int idr_requests = 0;
    int normal_requests = 0;
    int invalidation_requests = 0;
  };

  class fake_encoder_backend_t final:
      public video::encoder_backend::IEncoderBackend {
  public:
    std::unique_ptr<platf::encode_device_t> create_device(
      platf::display_t &display,
      const video::encoder_t &encoder,
      const video::config_t &config
    ) override {
      ++create_device_calls;
      last_display = &display;
      last_encoder = &encoder;
      last_config = &config;

      auto device = std::make_unique<fake_encode_device_t>();
      last_created_device = device.get();
      return device;
    }

    std::unique_ptr<video::encode_session_t> create_session(
      platf::display_t &display,
      const video::encoder_t &encoder,
      const video::config_t &config,
      int input_width,
      int input_height,
      std::unique_ptr<platf::encode_device_t> encode_device
    ) override {
      ++create_session_calls;
      last_display = &display;
      last_encoder = &encoder;
      last_config = &config;
      last_input_width = input_width;
      last_input_height = input_height;
      last_received_device = encode_device.get();

      auto session =
        std::make_unique<fake_encode_session_t>(
          std::move(encode_device)
        );
      last_created_session = session.get();
      return session;
    }

    platf::display_t *last_display = nullptr;
    const video::encoder_t *last_encoder = nullptr;
    const video::config_t *last_config = nullptr;
    fake_encode_device_t *last_created_device = nullptr;
    platf::encode_device_t *last_received_device = nullptr;
    fake_encode_session_t *last_created_session = nullptr;
    int last_input_width = 0;
    int last_input_height = 0;
    int create_device_calls = 0;
    int create_session_calls = 0;
  };

}  // namespace

TEST(EncoderBackendTest, FactoryForwardsConfigurationAndTransfersDevice) {
  fake_display_t display;
  fake_encoder_backend_t fake;
  video::encoder_backend::IEncoderBackend &backend = fake;
  video::config_t config {};
  config.width = 1920;
  config.height = 1080;

  auto device =
    backend.create_device(display, video::software, config);
  auto *expected_device = device.get();
  auto session = backend.create_session(
    display,
    video::software,
    config,
    2560,
    1440,
    std::move(device)
  );

  ASSERT_NE(session, nullptr);
  EXPECT_EQ(device, nullptr);
  EXPECT_EQ(fake.create_device_calls, 1);
  EXPECT_EQ(fake.create_session_calls, 1);
  EXPECT_EQ(fake.last_display, &display);
  EXPECT_EQ(fake.last_encoder, &video::software);
  EXPECT_EQ(fake.last_config, &config);
  EXPECT_EQ(fake.last_input_width, 2560);
  EXPECT_EQ(fake.last_input_height, 1440);
  EXPECT_EQ(fake.last_received_device, expected_device);
  EXPECT_EQ(fake.last_created_device, expected_device);
}

TEST(EncoderBackendTest, SessionAbiSupportsFakeHotPathWithoutGpu) {
  fake_display_t display;
  fake_encoder_backend_t backend;
  const video::config_t config {};
  auto device =
    backend.create_device(display, video::software, config);
  auto session = backend.create_session(
    display,
    video::software,
    config,
    1280,
    720,
    std::move(device)
  );
  ASSERT_NE(session, nullptr);

  fake_image_t image;
  safe::mail_raw_t::queue_t<video::packet_t> packets;
  int channel = 41;
  const auto timestamp =
    std::chrono::steady_clock::time_point {
      std::chrono::milliseconds {123}
    };

  EXPECT_EQ(session->convert(image), 9);
  session->request_idr_frame();
  EXPECT_EQ(
    session->encode_frame(73, packets, &channel, timestamp),
    17
  );
  session->invalidate_ref_frames(11, 19);
  session->request_normal_frame();

  auto *fake_session = backend.last_created_session;
  ASSERT_NE(fake_session, nullptr);
  EXPECT_EQ(fake_session->convert_calls, 1);
  EXPECT_EQ(fake_session->encode_calls, 1);
  EXPECT_EQ(fake_session->last_frame_number, 73);
  EXPECT_EQ(fake_session->last_packets, &packets);
  EXPECT_EQ(fake_session->last_channel_data, &channel);
  EXPECT_EQ(fake_session->last_frame_timestamp, timestamp);
  EXPECT_EQ(fake_session->idr_requests, 1);
  EXPECT_EQ(fake_session->normal_requests, 1);
  EXPECT_EQ(fake_session->invalidation_requests, 1);
  EXPECT_EQ(fake_session->invalidation_first, 11);
  EXPECT_EQ(fake_session->invalidation_last, 19);

  auto *fake_device = static_cast<fake_encode_device_t *>(
    fake_session->device.get()
  );
  ASSERT_NE(fake_device, nullptr);
  EXPECT_EQ(fake_device->convert_calls, 1);
  EXPECT_EQ(fake_device->last_image, &image);
}
