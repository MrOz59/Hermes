/**
 * @file src/encoder_backend.h
 * @brief Injectable encoder factory for Hermes video pipelines.
 */
#pragma once

#include "video.h"

namespace video::encoder_backend {

  /**
   * @brief Factory boundary for encoder devices and sessions.
   *
   * H1 retains encoder_t as the capability/policy descriptor,
   * platf::encode_device_t as the capture-to-encoder interop ABI, and
   * encode_session_t as the per-stream hot-path ABI. Implementations may be
   * called concurrently by independent session threads.
   */
  class IEncoderBackend {
  public:
    virtual ~IEncoderBackend() = default;

    [[nodiscard]] virtual std::unique_ptr<platf::encode_device_t>
      create_device(
        platf::display_t &display,
        const encoder_t &encoder,
        const config_t &config
      ) = 0;

    [[nodiscard]] virtual std::unique_ptr<encode_session_t>
      create_session(
        platf::display_t &display,
        const encoder_t &encoder,
        const config_t &config,
        int input_width,
        int input_height,
        std::unique_ptr<platf::encode_device_t> encode_device
      ) = 0;
  };

  /**
   * @brief Adapter preserving the current FFmpeg and native NVENC paths.
   */
  class legacy_video_encoder_backend_t final:
      public IEncoderBackend {
  public:
    [[nodiscard]] std::unique_ptr<platf::encode_device_t>
      create_device(
        platf::display_t &display,
        const encoder_t &encoder,
        const config_t &config
      ) override;

    [[nodiscard]] std::unique_ptr<encode_session_t>
      create_session(
        platf::display_t &display,
        const encoder_t &encoder,
        const config_t &config,
        int input_width,
        int input_height,
        std::unique_ptr<platf::encode_device_t> encode_device
      ) override;
  };

}  // namespace video::encoder_backend
