/**
 * @file src/platform/linux/encoder_probe_display.cpp
 * @brief A display with no screen behind it, for probing encoders.
 */
// standard includes
#include <algorithm>
#include <memory>
#include <vector>

// local includes
#include "cuda.h"
#include "misc.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "vaapi.h"

namespace platf {
  namespace {
    struct probe_image_t final: img_t {
      std::vector<std::uint8_t> pixels;
    };

    /**
     * Encoder validation only ever encodes the synthetic frames dummy_img()
     * fills in, so whether an encoder works is a property of the GPU and its
     * driver, not of any screen. This display stands in for one: it captures
     * nothing, and frames reach the encoder from system memory.
     *
     * VAAPI is opened on the GPU a Hermes-KMS stream encodes on, so what the
     * probe finds is what such a stream can do.
     */
    class encoder_probe_display_t final: public display_t {
    public:
      encoder_probe_display_t(mem_type_e memory, const video::config_t &config):
          memory_type {memory} {
        width = config.width;
        height = config.height;
        env_width = width;
        env_height = height;
      }

      std::shared_ptr<img_t> alloc_img() override {
        auto image = std::make_shared<probe_image_t>();
        image->width = width;
        image->height = height;
        image->pixel_pitch = 4;
        image->row_pitch = width * 4;
        image->pixels.resize(static_cast<std::size_t>(image->row_pitch) * height);
        image->data = image->pixels.data();
        return image;
      }

      int dummy_img(img_t *image) override {
        auto *probe = dynamic_cast<probe_image_t *>(image);
        if (!probe) {
          return -1;
        }
        std::fill(probe->pixels.begin(), probe->pixels.end(), 0);
        return 0;
      }

      capture_e capture(const push_captured_image_cb_t &, const pull_free_image_cb_t &, bool *) override {
        BOOST_LOG(error) << "The encoder probe display has nothing to capture";
        return capture_e::error;
      }

      std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e) override {
#ifdef SUNSHINE_BUILD_VAAPI
        if (memory_type == mem_type_e::vaapi) {
          file_t card {open_encode_render_node()};
          if (card.el < 0) {
            BOOST_LOG(error) << "The encoder probe found no GPU to encode on";
            return nullptr;
          }
          return va::make_avcodec_encode_device(width, height, std::move(card), 0, 0, false);
        }
#endif
#ifdef SUNSHINE_BUILD_CUDA
        if (memory_type == mem_type_e::cuda) {
          return cuda::make_avcodec_encode_device(width, height, false);
        }
#endif
        return std::make_unique<avcodec_encode_device_t>();
      }

    private:
      mem_type_e memory_type;
    };
  }  // namespace

  std::shared_ptr<display_t> encoder_probe_display(mem_type_e memory, const video::config_t &config) {
    if (config.width <= 0 || config.height <= 0) {
      return nullptr;
    }
    return std::make_shared<encoder_probe_display_t>(memory, config);
  }
}  // namespace platf
