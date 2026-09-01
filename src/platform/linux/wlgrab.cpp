/**
 * @file src/platform/linux/wlgrab.cpp
 * @brief Definitions for wlgrab capture.
 */
// standard includes
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>

// local includes
#include "cuda.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "virtual_display.h"
#include "src/video.h"
#include "vaapi.h"
#include "wayland.h"

using namespace std::literals;

namespace wl {
  static int env_width;
  static int env_height;

  struct img_t: public platf::img_t {
    ~img_t() override {
      delete[] data;
      data = nullptr;
    }
  };

  class wlr_t: public platf::display_t {
  public:
    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      delay = std::chrono::nanoseconds {1s} / config.framerate;
      mem_type = hwdevice_type;

      if (display.init()) {
        return -1;
      }

      interface.listen(display.registry());

      display.roundtrip();

      if (!interface[wl::interface_t::XDG_OUTPUT]) {
        BOOST_LOG(error) << "Missing Wayland wire for xdg_output"sv;
        return -1;
      }

      // Two capture protocols, and which one a session speaks is the whole
      // difference between a stream and nothing at all. wlroots compositors
      // speak wlr-screencopy; KWin and GNOME speak only ext-image-copy-capture,
      // which is why a KDE or GNOME desktop was not capturable here before.
      //
      // Where both exist, screencopy stays the default: it is the path every
      // working deployment is on today, and nothing is gained by moving them.
      // ext-image-copy-capture fills the gap rather than replacing anything.
      // HERMES_WAYLAND_CAPTURE forces one for testing the other on a session
      // that offers both.
      const bool have_screencopy = interface[wl::interface_t::WLR_EXPORT_DMABUF];
      const bool have_icc = interface[wl::interface_t::IMAGE_COPY_CAPTURE] &&
                            interface[wl::interface_t::IMAGE_CAPTURE_SOURCE] &&
                            interface[wl::interface_t::LINUX_DMABUF];

      const char *forced = std::getenv("HERMES_WAYLAND_CAPTURE");
      if (forced && !std::strcmp(forced, "icc")) {
        use_icc = have_icc;
        if (!use_icc) {
          BOOST_LOG(error) << "HERMES_WAYLAND_CAPTURE=icc, but this compositor does not offer "sv
                           << "ext-image-copy-capture with linux-dmabuf."sv;
          return -1;
        }
      } else if (forced && !std::strcmp(forced, "screencopy")) {
        use_icc = false;
        if (!have_screencopy) {
          BOOST_LOG(error) << "HERMES_WAYLAND_CAPTURE=screencopy, but this compositor does not offer "sv
                           << "wlr-screencopy."sv;
          return -1;
        }
      } else {
        use_icc = !have_screencopy && have_icc;
      }

      if (!have_screencopy && !have_icc) {
        BOOST_LOG(error) << "This compositor offers neither wlr-screencopy nor ext-image-copy-capture; "sv
                         << "there is no way to capture it."sv;
        return -1;
      }

      BOOST_LOG(info) << "Wayland capture protocol: "sv
                      << (use_icc ? "ext-image-copy-capture"sv : "wlr-screencopy"sv);

      if (interface.monitors.empty()) {
        BOOST_LOG(error) << "Wayland compositor did not advertise any enabled outputs."sv;
        return -1;
      }

      // A monitor's name and description arrive on xdg_output, not on the
      // registry, so every monitor has to be listened to before any of them can
      // be matched by name. Doing this per selected monitor - as this used to -
      // meant the match below always ran against empty names: the only path
      // that reached it selected by numeric index, where the name is never
      // read. Listening is not repeatable either, since it also adds the
      // wl_output listener, and wayland-client aborts on a second one.
      for (auto &candidate : interface.monitors) {
        candidate->listen(interface.output_manager);
      }
      display.roundtrip();

      auto monitor = interface.monitors[0].get();

      if (!display_name.empty() && display_name.rfind("VIRTUAL-", 0) != 0) {
        // Not a virtual display, parse as numeric index
        auto streamedMonitor = util::from_view(display_name);

        if (streamedMonitor >= 0 && streamedMonitor < interface.monitors.size()) {
          monitor = interface.monitors[streamedMonitor].get();
        }
      } else if (display_name.rfind("VIRTUAL-", 0) == 0 || VDISPLAY::isHermesKmsDisplay(display_name)) {
        // A Hyprland headless output is named by the compositor, not by a DRM
        // connector, so it resolves first and by a different route - but from
        // here on it is matched, selected and captured like any other output.
        auto connector = VDISPLAY::getHyprlandOutputName(display_name);
        const char *backend_label = "Hyprland headless";
        if (connector.empty()) {
          connector = VDISPLAY::getHermesKmsConnectorName(display_name);
          backend_label = "Hermes-KMS";
        }
        if (connector.empty()) {
          connector = VDISPLAY::getEvdiConnectorName(display_name);
          backend_label = "EVDI";
        }
        const auto monitor_it = std::find_if(interface.monitors.begin(), interface.monitors.end(), [&](const auto &candidate) {
          return candidate->name == connector || candidate->description.find(connector) != std::string::npos;
        });
        if (connector.empty() || monitor_it == interface.monitors.end()) {
          BOOST_LOG(error) << "Wayland "sv << backend_label << " output " << display_name
                           << " is not enabled by the compositor; refusing to capture a physical monitor."sv;
          return -1;
        }
        monitor = monitor_it->get();
        BOOST_LOG(info) << "Selected Wayland "sv << backend_label << " output " << connector;
      }

      output = monitor->output;

      offset_x = monitor->viewport.offset_x;
      offset_y = monitor->viewport.offset_y;
      width = monitor->viewport.width;
      height = monitor->viewport.height;

      this->env_width = ::wl::env_width;
      this->env_height = ::wl::env_height;

      BOOST_LOG(info) << "Selected monitor ["sv << monitor->description << "] for streaming"sv;
      BOOST_LOG(debug) << "Offset: "sv << offset_x << 'x' << offset_y;
      BOOST_LOG(debug) << "Resolution: "sv << width << 'x' << height;
      BOOST_LOG(debug) << "Desktop Resolution: "sv << env_width << 'x' << env_height;

      return 0;
    }

    int dummy_img(platf::img_t *img) override {
      return 0;
    }

    inline platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      auto to = std::chrono::steady_clock::now() + timeout;

      // Dispatch events until we get a new frame or the timeout expires.
      //
      // The two protocols are asked for a frame differently. Screencopy is asked
      // once per frame and negotiates the buffer each time. An ICC session
      // negotiates once and then hands out frames, so the session is opened on
      // the first snapshot and reused; icc_capture() does nothing until the
      // compositor has finished sending the constraints, which is why it is also
      // called from inside the dispatch loop - that is where "done" arrives.
      if (use_icc) {
        if (!dmabuf.icc_active() &&
            !dmabuf.icc_listen(
              interface.image_copy_capture_manager,
              interface.image_capture_source_manager,
              interface.dmabuf_interface,
              output,
              cursor
            )) {
          return platf::capture_e::error;
        }
        dmabuf.icc_capture(cursor);
      } else {
        dmabuf.listen(interface.screencopy_manager, interface.dmabuf_interface, output, cursor);
      }

      do {
        auto remaining_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(to - std::chrono::steady_clock::now());
        if (remaining_time_ms.count() < 0 || !display.dispatch(remaining_time_ms)) {
          return platf::capture_e::timeout;
        }
        // Only while still waiting: icc_capture() arms a frame whenever the
        // session is idle, so calling it after the status has already become
        // READY throws the finished frame away and asks for another, and the
        // loop never ends. REINIT is guarded for the same reason - the caller
        // below decides what a failed session means, not the next iteration.
        if (use_icc && dmabuf.status == dmabuf_t::WAITING) {
          dmabuf.icc_capture(cursor);
        }
      } while (dmabuf.status == dmabuf_t::WAITING);

      auto current_frame = dmabuf.current_frame;

      if (
        dmabuf.status == dmabuf_t::REINIT ||
        current_frame->sd.width != width ||
        current_frame->sd.height != height
      ) {
        // Reinit rebuilds this display and tries the same path again, which is
        // right for a compositor that is still settling and wrong for a machine
        // whose capture path cannot work at all. In the second case the retries
        // never end, and because audio and input do not run through capture,
        // what the client gets is an interactive black screen - a failure with
        // no symptom to report. Once the failures have run long enough to rule
        // out bad luck, end the capture with the reason instead.
        if (wl::dmabuf_capture_exhausted()) {
          BOOST_LOG(error) << "Wayland capture has failed on every frame and is being stopped. "sv
                           << "Streaming would have continued as a black screen. See the errors "sv
                           << "above; on a machine with more than one GPU, set adapter_name to the "sv
                           << "render node the compositor renders on."sv;
          return platf::capture_e::error;
        }
        return platf::capture_e::reinit;
      }

      return platf::capture_e::ok;
    }

    platf::mem_type_e mem_type;

    std::chrono::nanoseconds delay;

    wl::display_t display;
    interface_t interface;
    dmabuf_t dmabuf;

    wl_output *output;
    bool use_icc {false};
  };

  class wlr_ram_t: public wlr_t {
  public:
    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto next_frame = std::chrono::steady_clock::now();

      sleep_overshoot_logger.reset();

      while (true) {
        auto now = std::chrono::steady_clock::now();

        if (next_frame > now) {
          std::this_thread::sleep_for(next_frame - now);
          sleep_overshoot_logger.first_point(next_frame);
          sleep_overshoot_logger.second_point_now_and_log();
        }

        next_frame += delay;
        if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
          next_frame = now + delay;
        }

        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
        switch (status) {
          case platf::capture_e::reinit:
          case platf::capture_e::error:
          case platf::capture_e::interrupted:
            return status;
          case platf::capture_e::timeout:
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return platf::capture_e::ok;
            }
            break;
          case platf::capture_e::ok:
            if (!push_captured_image_cb(std::move(img_out), true)) {
              return platf::capture_e::ok;
            }
            break;
          default:
            BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
            return status;
        }
      }

      return platf::capture_e::ok;
    }

    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      auto status = wlr_t::snapshot(pull_free_image_cb, img_out, timeout, cursor);
      if (status != platf::capture_e::ok) {
        return status;
      }
      auto frame_timestamp = std::chrono::steady_clock::now();

      auto current_frame = dmabuf.current_frame;

      auto rgb_opt = egl::import_source(egl_display.get(), current_frame->sd);

      if (!rgb_opt) {
        return platf::capture_e::reinit;
      }

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }

      gl::ctx.BindTexture(GL_TEXTURE_2D, (*rgb_opt)->tex[0]);

      // Don't remove these lines, see https://github.com/LizardByte/Sunshine/issues/453
      int w, h;
      gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
      gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
      BOOST_LOG(debug) << "width and height: w "sv << w << " h "sv << h;

      gl::ctx.GetTextureSubImage((*rgb_opt)->tex[0], 0, 0, 0, 0, width, height, 1, GL_BGRA, GL_UNSIGNED_BYTE, img_out->height * img_out->row_pitch, img_out->data);
      gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
      img_out->frame_timestamp = frame_timestamp;

      return platf::capture_e::ok;
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      if (wlr_t::init(hwdevice_type, display_name, config)) {
        return -1;
      }

      egl_display = egl::make_display(display.get());
      if (!egl_display) {
        return -1;
      }

      auto ctx_opt = egl::make_ctx(egl_display.get());
      if (!ctx_opt) {
        return -1;
      }

      ctx = std::move(*ctx_opt);

      return 0;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, false);
      }
#endif

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        return cuda::make_avcodec_encode_device(width, height, false);
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<img_t>();
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = img->pixel_pitch * width;
      img->data = new std::uint8_t[height * img->row_pitch];

      return img;
    }

    egl::display_t egl_display;
    egl::ctx_t ctx;
  };

  class wlr_vram_t: public wlr_t {
  public:
    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto next_frame = std::chrono::steady_clock::now();

      sleep_overshoot_logger.reset();

      while (true) {
        auto now = std::chrono::steady_clock::now();

        if (next_frame > now) {
          std::this_thread::sleep_for(next_frame - now);
          sleep_overshoot_logger.first_point(next_frame);
          sleep_overshoot_logger.second_point_now_and_log();
        }

        next_frame += delay;
        if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
          next_frame = now + delay;
        }

        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
        switch (status) {
          case platf::capture_e::reinit:
          case platf::capture_e::error:
          case platf::capture_e::interrupted:
            return status;
          case platf::capture_e::timeout:
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return platf::capture_e::ok;
            }
            break;
          case platf::capture_e::ok:
            if (!push_captured_image_cb(std::move(img_out), true)) {
              return platf::capture_e::ok;
            }
            break;
          default:
            BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
            return status;
        }
      }

      return platf::capture_e::ok;
    }

    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      auto status = wlr_t::snapshot(pull_free_image_cb, img_out, timeout, cursor);
      if (status != platf::capture_e::ok) {
        return status;
      }
      auto frame_timestamp = std::chrono::steady_clock::now();

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }
      auto img = (egl::img_descriptor_t *) img_out.get();
      img->reset();

      auto current_frame = dmabuf.current_frame;

      ++sequence;
      img->sequence = sequence;

      img->sd = current_frame->sd;
      img->frame_timestamp = frame_timestamp;

      // Prevent dmabuf from closing the file descriptors.
      std::fill_n(current_frame->sd.fds, 4, -1);

      return platf::capture_e::ok;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<egl::img_descriptor_t>();

      img->width = width;
      img->height = height;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->data = nullptr;

      // File descriptors aren't open
      std::fill_n(img->sd.fds, 4, -1);

      return img;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, 0, 0, true);
      }
#endif

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        return cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    int dummy_img(platf::img_t *img) override {
      // Empty images are recognized as dummies by the zero sequence number
      return 0;
    }

    std::uint64_t sequence {};
  };

}  // namespace wl

namespace platf {
  std::shared_ptr<display_t> wl_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::vaapi && hwdevice_type != platf::mem_type_e::cuda) {
      BOOST_LOG(error) << "Could not initialize display with the given hw device type."sv;
      return nullptr;
    }

    if (hwdevice_type == platf::mem_type_e::vaapi || hwdevice_type == platf::mem_type_e::cuda) {
      auto wlr = std::make_shared<wl::wlr_vram_t>();
      if (wlr->init(hwdevice_type, display_name, config)) {
        return nullptr;
      }

      return wlr;
    }

    auto wlr = std::make_shared<wl::wlr_ram_t>();
    if (wlr->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return wlr;
  }

  std::vector<std::string> wl_display_names() {
    std::vector<std::string> display_names;

    wl::display_t display;
    if (display.init()) {
      return {};
    }

    wl::interface_t interface;
    interface.listen(display.registry());

    display.roundtrip();

    if (!interface[wl::interface_t::XDG_OUTPUT]) {
      BOOST_LOG(warning) << "Missing Wayland wire for xdg_output"sv;
      return {};
    }

    if (!interface[wl::interface_t::WLR_EXPORT_DMABUF]) {
      BOOST_LOG(warning) << "Missing Wayland wire for wlr-export-dmabuf"sv;
      return {};
    }

    wl::env_width = 0;
    wl::env_height = 0;

    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }

    display.roundtrip();

    BOOST_LOG(info) << "-------- Start of Wayland monitor list --------"sv;

    for (int x = 0; x < interface.monitors.size(); ++x) {
      auto monitor = interface.monitors[x].get();

      wl::env_width = std::max(wl::env_width, (int) (monitor->viewport.offset_x + monitor->viewport.width));
      wl::env_height = std::max(wl::env_height, (int) (monitor->viewport.offset_y + monitor->viewport.height));

      BOOST_LOG(info) << "Monitor " << x << " is "sv << monitor->name << ": "sv << monitor->description;

      display_names.emplace_back(std::to_string(x));
    }

    BOOST_LOG(info) << "--------- End of Wayland monitor list ---------"sv;

    return display_names;
  }

}  // namespace platf
