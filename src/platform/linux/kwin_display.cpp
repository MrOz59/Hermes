/** @file kwin_display.cpp
 *  @brief Feed private KWin PipeWire frames into Hermes' existing encoder path.
 */
#include "kwin_display.h"

#include "cuda.h"
#include "kwin_session.h"
#include "misc.h"
#include "pipewire_session.h"
#include "src/video.h"
#include "vaapi.h"

#include <algorithm>

namespace platf {
  namespace {
    struct kwin_image_t final: img_t {
      std::vector<std::uint8_t> pixels;
    };

    class kwin_display_t final: public display_t {
    public:
      bool init(mem_type_e memory, const video::config_t &config) {
        if (config.dynamicRange != 0 || config.framerate <= 0 ||
            (memory != mem_type_e::system && memory != mem_type_e::vaapi && memory != mem_type_e::cuda)) {
          BOOST_LOG(::error) << "Private KWin capture requires SDR and a supported mapped-frame encoder";
          return false;
        }
        memory_type = memory;
        frame_rate = config.framerate;
        pipewire_socket = config.kwin_pipewire_socket;
        std::string error;
        connection = kwin::connection_t::open(
          {config.kwin_wayland_socket, config.kwin_pipewire_socket, {}},
          true,
          false,
          error
        );
        if (!connection) {
          BOOST_LOG(::error) << "Private KWin capture: " << error;
          return false;
        }
        const auto &output = connection->output();
        if (output.width <= 0 || output.height <= 0 || output.width > 16384 || output.height > 16384) {
          BOOST_LOG(::error) << "Private KWin output dimensions are unsupported";
          return false;
        }
        width = output.width;
        height = output.height;
        // This adapter deliberately selects a single-output compositor. Its
        // input coordinates are local to that desktop, never the host envelope.
        offset_x = offset_y = 0;
        env_width = width;
        env_height = height;
        return true;
      }

      std::shared_ptr<img_t> alloc_img() override {
        auto image = std::make_shared<kwin_image_t>();
        image->width = width;
        image->height = height;
        image->pixel_pitch = 4;
        image->row_pitch = width * 4;
        image->pixels.resize(static_cast<std::size_t>(image->row_pitch) * height);
        image->data = image->pixels.data();
        return image;
      }

      int dummy_img(img_t *image) override {
        auto *mapped = dynamic_cast<kwin_image_t *>(image);
        if (!mapped) {
          return -1;
        }
        std::fill(mapped->pixels.begin(), mapped->pixels.end(), 0);
        mapped->frame_timestamp.reset();
        return 0;
      }

      capture_e capture(const push_captured_image_cb_t &push, const pull_free_image_cb_t &pull, bool *cursor) override {
        std::string error;
        std::uint64_t serial {};
        const bool capture_cursor = *cursor;
        if (!connection->start_capture(capture_cursor, serial, error)) {
          BOOST_LOG(::error) << "Private KWin capture: " << error;
          return capture_e::error;
        }
        // Declared after connection in the class so PipeWire is torn down first.
        receiver = kwin::video_receiver_t::open(pipewire_socket, serial, width, height, frame_rate, error);
        if (!receiver) {
          BOOST_LOG(::error) << "Private KWin capture: " << error;
          return capture_e::error;
        }
        for (;;) {
          if (!connection->dispatch(std::chrono::milliseconds(0))) {
            BOOST_LOG(::error) << "Private KWin capture: " << connection->error();
            return capture_e::error;
          }
          // Cursor mode is a stream property; recreate on the same explicit
          // endpoint when the client changes it. Disconnects never fall back.
          if (*cursor != capture_cursor) {
            return capture_e::reinit;
          }
          kwin::frame_t frame;
          auto result = receiver->next(frame, std::chrono::milliseconds(100), error);
          if (result == kwin::frame_result_t::failed) {
            BOOST_LOG(::error) << "Private KWin capture: " << error;
            return capture_e::error;
          }
          if (!connection->dispatch(std::chrono::milliseconds(0))) {
            BOOST_LOG(::error) << "Private KWin capture: " << connection->error();
            return capture_e::error;
          }
          std::shared_ptr<img_t> image;
          if (result == kwin::frame_result_t::timeout) {
            if (!push(std::move(image), false)) {
              return capture_e::ok;
            }
            continue;
          }
          if (!pull(image)) {
            return capture_e::interrupted;
          }
          auto *mapped = dynamic_cast<kwin_image_t *>(image.get());
          if (!mapped || frame.width != static_cast<std::uint32_t>(width) ||
              frame.height != static_cast<std::uint32_t>(height)) {
            return capture_e::error;
          }
          mapped->pixels = std::move(frame.bgrx);
          mapped->data = mapped->pixels.data();
          mapped->frame_timestamp = frame.timestamp;
          if (!push(std::move(image), true)) {
            return capture_e::ok;
          }
        }
      }

      std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e) override {
#ifdef SUNSHINE_BUILD_VAAPI
        if (memory_type == mem_type_e::vaapi) {
          // The best GPU, as the encoder probe chose it, not whichever one
          // happens to be renderD128 this boot.
          file_t card {open_encode_render_node()};
          if (card.el < 0) {
            BOOST_LOG(::error) << "Private KWin capture: no GPU to encode on";
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
      mem_type_e memory_type {mem_type_e::system};
      std::uint32_t frame_rate {};
      std::string pipewire_socket;
      std::unique_ptr<kwin::connection_t> connection;
      std::unique_ptr<kwin::video_receiver_t> receiver;
    };
  }  // namespace

  std::shared_ptr<display_t> kwin_display(mem_type_e memory, const video::config_t &config) {
    auto display = std::make_shared<kwin_display_t>();
    if (!display->init(memory, config)) {
      return nullptr;
    }
    return display;
  }
}  // namespace platf
