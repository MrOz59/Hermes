/**
 * @file src/platform/linux/wayland.cpp
 * @brief Definitions for Wayland capture.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// platform includes
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-util.h>
#include <xf86drm.h>

// local includes
#include "graphics.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/round_robin.h"
#include "src/utility.h"
#include "wayland.h"

extern const wl_interface wl_output_interface;

using namespace std::literals;

// Disable warning for converting incompatible functions
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wpmf-conversions"

namespace wl {

  // Helper to call C++ method from wayland C callback
  template<class T, class Method, Method m, class... Params>
  static auto classCall(void *data, Params... params) -> decltype(((*reinterpret_cast<T *>(data)).*m)(params...)) {
    return ((*reinterpret_cast<T *>(data)).*m)(params...);
  }

#define CLASS_CALL(c, m) classCall<c, decltype(&c::m), &c::m>

  // Define buffer params listener
  static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    .created = dmabuf_t::buffer_params_created,
    .failed = dmabuf_t::buffer_params_failed
  };

  int display_t::init(const char *display_name) {
    if (!display_name) {
      display_name = std::getenv("WAYLAND_DISPLAY");
    }

    if (!display_name) {
      BOOST_LOG(error) << "Environment variable WAYLAND_DISPLAY has not been defined"sv;
      return -1;
    }

    display_internal.reset(wl_display_connect(display_name));
    if (!display_internal) {
      BOOST_LOG(error) << "Couldn't connect to Wayland display: "sv << display_name;
      return -1;
    }

    BOOST_LOG(info) << "Found display ["sv << display_name << ']';

    return 0;
  }

  void display_t::roundtrip() {
    wl_display_roundtrip(display_internal.get());
  }

  /**
   * @brief Waits up to the specified timeout to dispatch new events on the wl_display.
   * @param timeout The timeout in milliseconds.
   * @return `true` if new events were dispatched or `false` if the timeout expired.
   */
  bool display_t::dispatch(std::chrono::milliseconds timeout) {
    // Check if any events are queued already. If not, flush
    // outgoing events, and prepare to wait for readability.
    if (wl_display_prepare_read(display_internal.get()) == 0) {
      wl_display_flush(display_internal.get());

      // Wait for an event to come in
      struct pollfd pfd = {};
      pfd.fd = wl_display_get_fd(display_internal.get());
      pfd.events = POLLIN;
      if (poll(&pfd, 1, timeout.count()) == 1 && (pfd.revents & POLLIN)) {
        // Read the new event(s)
        wl_display_read_events(display_internal.get());
      } else {
        // We timed out, so unlock the queue now
        wl_display_cancel_read(display_internal.get());
        return false;
      }
    }

    // Dispatch any existing or new pending events
    wl_display_dispatch_pending(display_internal.get());
    return true;
  }

  wl_registry *display_t::registry() {
    return wl_display_get_registry(display_internal.get());
  }

  namespace {
    struct output_mode_t {
      zwlr_output_mode_v1 *proxy {nullptr};
      int width {0};
      int height {0};
      int refresh_rate {0};
      bool preferred {false};
    };

    struct output_head_t {
      zwlr_output_head_v1 *proxy {nullptr};
      std::string name;
      std::string description;
      bool enabled {false};
      output_mode_t *current_mode {nullptr};
      int x {0};
      int y {0};
      int transform {WL_OUTPUT_TRANSFORM_NORMAL};
      wl_fixed_t scale {wl_fixed_from_int(1)};
      std::vector<std::unique_ptr<output_mode_t>> modes;
    };

    struct saved_output_t {
      bool enabled {false};
      int width {0};
      int height {0};
      int refresh_rate {0};
      int x {0};
      int y {0};
      int transform {WL_OUTPUT_TRANSFORM_NORMAL};
      wl_fixed_t scale {wl_fixed_from_int(1)};
    };

    std::map<std::string, saved_output_t> saved_output_layout;
    bool saved_output_layout_available {false};

    struct output_control_t {
      display_t display;
      zwlr_output_manager_v1 *manager {nullptr};
      std::vector<std::unique_ptr<output_head_t>> heads;
      uint32_t serial {0};
      bool ready {false};
      bool manager_finished {false};
      int apply_result {0};

      ~output_control_t() {
        for (auto &head : heads) {
          for (auto &mode : head->modes) {
            if (mode->proxy) {
              zwlr_output_mode_v1_destroy(mode->proxy);
            }
          }
          if (head->proxy) {
            zwlr_output_head_v1_destroy(head->proxy);
          }
        }
        if (manager && !manager_finished) {
          zwlr_output_manager_v1_destroy(manager);
        }
      }

      bool init() {
        if (display.init()) {
          return false;
        }

        static const wl_registry_listener registry_listener {
          .global = [](void *data, wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
            auto *control = static_cast<output_control_t *>(data);
            if (std::strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
              const auto bind_version = std::min(version, 4u);
              control->manager = static_cast<zwlr_output_manager_v1 *>(
                wl_registry_bind(registry, id, &zwlr_output_manager_v1_interface, bind_version)
              );
              zwlr_output_manager_v1_add_listener(control->manager, &manager_listener, control);
            }
          },
          .global_remove = [](void *, wl_registry *, uint32_t) {},
        };

        wl_registry_add_listener(display.registry(), &registry_listener, this);
        display.roundtrip();
        return manager != nullptr;
      }

      bool collect() {
        if (!manager) {
          return false;
        }
        display.roundtrip();
        // A complete layout can legitimately be empty while the compositor is
        // waiting for a newly hotplugged DRM connector to appear.
        return ready && !manager_finished;
      }

      output_head_t *find_head(const std::string &name) {
        const auto it = std::find_if(heads.begin(), heads.end(), [&](const auto &head) {
          return head->name == name;
        });
        return it == heads.end() ? nullptr : it->get();
      }

      static void mode_size(void *data, zwlr_output_mode_v1 *, int32_t width, int32_t height) {
        auto *mode = static_cast<output_mode_t *>(data);
        mode->width = width;
        mode->height = height;
      }

      static void mode_refresh(void *data, zwlr_output_mode_v1 *, int32_t refresh_rate) {
        static_cast<output_mode_t *>(data)->refresh_rate = refresh_rate;
      }

      static void mode_preferred(void *data, zwlr_output_mode_v1 *) {
        static_cast<output_mode_t *>(data)->preferred = true;
      }

      static void mode_finished(void *, zwlr_output_mode_v1 *) {}

      static void head_name(void *data, zwlr_output_head_v1 *, const char *name) {
        static_cast<output_head_t *>(data)->name = name;
      }

      static void head_description(void *data, zwlr_output_head_v1 *, const char *description) {
        static_cast<output_head_t *>(data)->description = description;
      }

      static void head_physical_size(void *, zwlr_output_head_v1 *, int32_t, int32_t) {}

      static void head_mode(void *data, zwlr_output_head_v1 *, zwlr_output_mode_v1 *proxy) {
        auto *head = static_cast<output_head_t *>(data);
        auto mode = std::make_unique<output_mode_t>();
        mode->proxy = proxy;
        zwlr_output_mode_v1_add_listener(proxy, &mode_listener, mode.get());
        head->modes.emplace_back(std::move(mode));
      }

      static void head_enabled(void *data, zwlr_output_head_v1 *, int32_t enabled) {
        static_cast<output_head_t *>(data)->enabled = enabled != 0;
      }

      static void head_current_mode(void *data, zwlr_output_head_v1 *, zwlr_output_mode_v1 *proxy) {
        auto *head = static_cast<output_head_t *>(data);
        const auto it = std::find_if(head->modes.begin(), head->modes.end(), [&](const auto &mode) {
          return mode->proxy == proxy;
        });
        head->current_mode = it == head->modes.end() ? nullptr : it->get();
      }

      static void head_position(void *data, zwlr_output_head_v1 *, int32_t x, int32_t y) {
        auto *head = static_cast<output_head_t *>(data);
        head->x = x;
        head->y = y;
      }

      static void head_transform(void *data, zwlr_output_head_v1 *, int32_t transform) {
        static_cast<output_head_t *>(data)->transform = transform;
      }

      static void head_scale(void *data, zwlr_output_head_v1 *, wl_fixed_t scale) {
        static_cast<output_head_t *>(data)->scale = scale;
      }

      static void head_finished(void *, zwlr_output_head_v1 *) {}
      static void head_string(void *, zwlr_output_head_v1 *, const char *) {}
      static void head_adaptive_sync(void *, zwlr_output_head_v1 *, uint32_t) {}

      static void manager_head(void *data, zwlr_output_manager_v1 *, zwlr_output_head_v1 *proxy) {
        auto *control = static_cast<output_control_t *>(data);
        auto head = std::make_unique<output_head_t>();
        head->proxy = proxy;
        zwlr_output_head_v1_add_listener(proxy, &head_listener, head.get());
        control->heads.emplace_back(std::move(head));
      }

      static void manager_done(void *data, zwlr_output_manager_v1 *, uint32_t serial) {
        auto *control = static_cast<output_control_t *>(data);
        control->serial = serial;
        control->ready = true;
      }

      static void manager_protocol_finished(void *data, zwlr_output_manager_v1 *) {
        static_cast<output_control_t *>(data)->manager_finished = true;
      }

      static void configuration_succeeded(void *data, zwlr_output_configuration_v1 *) {
        static_cast<output_control_t *>(data)->apply_result = 1;
      }

      static void configuration_failed(void *data, zwlr_output_configuration_v1 *) {
        static_cast<output_control_t *>(data)->apply_result = -1;
      }

      static void configuration_cancelled(void *data, zwlr_output_configuration_v1 *) {
        static_cast<output_control_t *>(data)->apply_result = -1;
      }

      static inline const zwlr_output_mode_v1_listener mode_listener {
        .size = mode_size,
        .refresh = mode_refresh,
        .preferred = mode_preferred,
        .finished = mode_finished,
      };

      static inline const zwlr_output_head_v1_listener head_listener {
        .name = head_name,
        .description = head_description,
        .physical_size = head_physical_size,
        .mode = head_mode,
        .enabled = head_enabled,
        .current_mode = head_current_mode,
        .position = head_position,
        .transform = head_transform,
        .scale = head_scale,
        .finished = head_finished,
        .make = head_string,
        .model = head_string,
        .serial_number = head_string,
        .adaptive_sync = head_adaptive_sync,
      };

      static inline const zwlr_output_manager_v1_listener manager_listener {
        .head = manager_head,
        .done = manager_done,
        .finished = manager_protocol_finished,
      };

      static inline const zwlr_output_configuration_v1_listener configuration_listener {
        .succeeded = configuration_succeeded,
        .failed = configuration_failed,
        .cancelled = configuration_cancelled,
      };
    };

    output_mode_t *preferred_or_current_mode(output_head_t &head) {
      if (head.current_mode) {
        return head.current_mode;
      }
      const auto it = std::find_if(head.modes.begin(), head.modes.end(), [](const auto &mode) {
        return mode->preferred;
      });
      return it == head.modes.end() ? (head.modes.empty() ? nullptr : head.modes.front().get()) : it->get();
    }

    output_mode_t *matching_mode(output_head_t &head, int width, int height, int refresh_rate) {
      const auto it = std::find_if(head.modes.begin(), head.modes.end(), [&](const auto &mode) {
        return mode->width == width && mode->height == height &&
               (refresh_rate <= 0 || mode->refresh_rate == 0 || std::abs(mode->refresh_rate - refresh_rate) <= 1000);
      });
      return it == head.modes.end() ? nullptr : it->get();
    }

    bool apply_configuration(output_control_t &control, output_head_t &virtual_head, int width, int height, int refresh_rate, bool exclusive, bool restore) {
      auto *configuration = zwlr_output_manager_v1_create_configuration(control.manager, control.serial);
      if (!configuration) {
        return false;
      }

      bool valid = true;
      int right_edge = 0;
      for (const auto &head : control.heads) {
        if (head->enabled && head->current_mode) {
          right_edge = std::max(right_edge, head->x + head->current_mode->width);
        }
      }

      for (const auto &head : control.heads) {
        bool enable = head->enabled;
        output_mode_t *mode = preferred_or_current_mode(*head);
        int x = head->x;
        int y = head->y;
        int transform = head->transform;
        wl_fixed_t scale = head->scale;
        bool custom_mode = false;

        if (restore) {
          const auto saved = saved_output_layout.find(head->name);
          if (saved != saved_output_layout.end()) {
            enable = saved->second.enabled;
            mode = matching_mode(*head, saved->second.width, saved->second.height, saved->second.refresh_rate);
            x = saved->second.x;
            y = saved->second.y;
            transform = saved->second.transform;
            scale = saved->second.scale;
          }
        } else if (head.get() == &virtual_head) {
          enable = true;
          mode = matching_mode(*head, width, height, refresh_rate);
          custom_mode = mode == nullptr;
          x = exclusive ? 0 : right_edge;
          y = 0;
          transform = WL_OUTPUT_TRANSFORM_NORMAL;
          scale = wl_fixed_from_int(1);
        } else if (exclusive) {
          enable = false;
        }

        if (!enable) {
          zwlr_output_configuration_v1_disable_head(configuration, head->proxy);
          continue;
        }

        if (!mode && !custom_mode) {
          BOOST_LOG(error) << "Wayland output "sv << head->name << " has no usable mode"sv;
          valid = false;
          break;
        }

        auto *configured_head = zwlr_output_configuration_v1_enable_head(configuration, head->proxy);
        if (custom_mode) {
          zwlr_output_configuration_head_v1_set_custom_mode(configured_head, width, height, refresh_rate);
        } else {
          zwlr_output_configuration_head_v1_set_mode(configured_head, mode->proxy);
        }
        zwlr_output_configuration_head_v1_set_position(configured_head, x, y);
        zwlr_output_configuration_head_v1_set_transform(configured_head, transform);
        zwlr_output_configuration_head_v1_set_scale(configured_head, scale);
      }

      if (!valid) {
        zwlr_output_configuration_v1_destroy(configuration);
        return false;
      }

      control.apply_result = 0;
      zwlr_output_configuration_v1_add_listener(configuration, &output_control_t::configuration_listener, &control);
      zwlr_output_configuration_v1_apply(configuration);
      for (int attempt = 0; attempt < 20 && control.apply_result == 0; ++attempt) {
        control.display.dispatch(100ms);
      }
      zwlr_output_configuration_v1_destroy(configuration);
      return control.apply_result == 1;
    }
  }  // namespace

  inline monitor_t::monitor_t(wl_output *output):
      output {output},
      wl_listener {
        &CLASS_CALL(monitor_t, wl_geometry),
        &CLASS_CALL(monitor_t, wl_mode),
        &CLASS_CALL(monitor_t, wl_done),
        &CLASS_CALL(monitor_t, wl_scale),
      },
      xdg_listener {
        &CLASS_CALL(monitor_t, xdg_position),
        &CLASS_CALL(monitor_t, xdg_size),
        &CLASS_CALL(monitor_t, xdg_done),
        &CLASS_CALL(monitor_t, xdg_name),
        &CLASS_CALL(monitor_t, xdg_description)
      } {
  }

  inline void monitor_t::xdg_name(zxdg_output_v1 *, const char *name) {
    this->name = name;

    BOOST_LOG(info) << "Name: "sv << this->name;
  }

  void monitor_t::xdg_description(zxdg_output_v1 *, const char *description) {
    this->description = description;

    BOOST_LOG(info) << "Found monitor: "sv << this->description;
  }

  void monitor_t::xdg_position(zxdg_output_v1 *, std::int32_t x, std::int32_t y) {
    viewport.offset_x = x;
    viewport.offset_y = y;

    BOOST_LOG(info) << "Offset: "sv << x << 'x' << y;
  }

  void monitor_t::xdg_size(zxdg_output_v1 *, std::int32_t width, std::int32_t height) {
    BOOST_LOG(info) << "Logical size: "sv << width << 'x' << height;
  }

  void monitor_t::wl_mode(
    wl_output *wl_output,
    std::uint32_t flags,
    std::int32_t width,
    std::int32_t height,
    std::int32_t refresh
  ) {
    viewport.width = width;
    viewport.height = height;

    BOOST_LOG(info) << "Resolution: "sv << width << 'x' << height;
  }

  void monitor_t::listen(zxdg_output_manager_v1 *output_manager) {
    auto xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, output);
    zxdg_output_v1_add_listener(xdg_output, &xdg_listener, this);
    wl_output_add_listener(output, &wl_listener, this);
  }

  interface_t::interface_t() noexcept
      :
      screencopy_manager {nullptr},
      dmabuf_interface {nullptr},
      output_manager {nullptr},
      image_copy_capture_manager {nullptr},
      image_capture_source_manager {nullptr},
      listener {
        &CLASS_CALL(interface_t, add_interface),
        &CLASS_CALL(interface_t, del_interface)
      } {
  }

  void interface_t::listen(wl_registry *registry) {
    wl_registry_add_listener(registry, &listener, this);
  }

  void interface_t::add_interface(
    wl_registry *registry,
    std::uint32_t id,
    const char *interface,
    std::uint32_t version
  ) {
    BOOST_LOG(debug) << "Available interface: "sv << interface << '(' << id << ") version "sv << version;

    if (!std::strcmp(interface, wl_output_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      monitors.emplace_back(
        std::make_unique<monitor_t>(
          (wl_output *) wl_registry_bind(registry, id, &wl_output_interface, 2)
        )
      );
    } else if (!std::strcmp(interface, zxdg_output_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      output_manager = (zxdg_output_manager_v1 *) wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface, version);

      this->interface[XDG_OUTPUT] = true;
    } else if (!std::strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      screencopy_manager = (zwlr_screencopy_manager_v1 *) wl_registry_bind(registry, id, &zwlr_screencopy_manager_v1_interface, version);

      this->interface[WLR_EXPORT_DMABUF] = true;
    } else if (!std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      dmabuf_interface = (zwp_linux_dmabuf_v1 *) wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, version);

      this->interface[LINUX_DMABUF] = true;
    } else if (!std::strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      image_copy_capture_manager = (ext_image_copy_capture_manager_v1 *) wl_registry_bind(registry, id, &ext_image_copy_capture_manager_v1_interface, 1);

      this->interface[IMAGE_COPY_CAPTURE] = true;
    } else if (!std::strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      image_capture_source_manager = (ext_output_image_capture_source_manager_v1 *) wl_registry_bind(registry, id, &ext_output_image_capture_source_manager_v1_interface, 1);

      this->interface[IMAGE_CAPTURE_SOURCE] = true;
    }
  }

  void interface_t::del_interface(wl_registry *registry, uint32_t id) {
    BOOST_LOG(info) << "Delete: "sv << id;
  }

  // A capture path that cannot allocate a buffer fails every frame, and every
  // failure is reported by a different object (see dmabuf_capture_exhausted()),
  // so both the giving-up decision and the log throttling have to be kept here.
  static std::atomic<unsigned> gbm_failure_streak {0};
  static std::atomic<std::int64_t> last_gbm_failure_ns {0};

  // Long enough to ride out a compositor that is still bringing an output up,
  // short enough that nobody watches a black screen for a second while we count.
  static constexpr unsigned gbm_failure_limit = 10;

  // A reinit retries within a frame time or two, so a failure arriving after a
  // pause this long is not part of the same run. Without the gap the streak
  // would be a lifetime tally: a user who fixed their configuration and started
  // a new stream would be refused on the first frame, on the strength of a run
  // that ended minutes ago.
  static constexpr std::int64_t gbm_failure_gap_ns = 5'000'000'000;

  static void note_gbm_failure() {
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
    )
                       .count();
    const auto previous = last_gbm_failure_ns.exchange(now, std::memory_order_relaxed);

    if (previous != 0 && now - previous > gbm_failure_gap_ns) {
      gbm_failure_streak.store(1, std::memory_order_relaxed);
      return;
    }
    gbm_failure_streak.fetch_add(1, std::memory_order_relaxed);
  }

  static void clear_gbm_failures() {
    gbm_failure_streak.store(0, std::memory_order_relaxed);
    last_gbm_failure_ns.store(0, std::memory_order_relaxed);
  }

  // The first failure of a streak carries the diagnosis; the rest are the same
  // sentence at frame rate, which is how the original report ended up with tens
  // of thousands of identical lines and no explanation among them.
  static bool should_log_gbm_failure() {
    return gbm_failure_streak.load(std::memory_order_relaxed) <= 1;
  }

  bool dmabuf_capture_exhausted() {
    return gbm_failure_streak.load(std::memory_order_relaxed) >= gbm_failure_limit;
  }

  /**
   * @brief Open a render node, and prove GBM can actually allocate on it.
   *
   * Opening a render node says nothing about whether buffers can come from it.
   * Mesa loads a driver for whatever the node turns out to be, and when there is
   * none to load - a proprietary-NVIDIA node has no nouveau behind it - it falls
   * back to kms_swrast without complaint. kms_swrast allocates through
   * DRM_IOCTL_MODE_CREATE_DUMB, which render nodes refuse with EACCES, so the
   * device looks healthy right up to the moment the first buffer is asked for.
   * Asking for a throwaway buffer is what separates the two cases.
   */
  static gbm_device *open_usable_gbm_device(const char *node, int &out_fd) {
    const int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      BOOST_LOG(debug) << "GBM: cannot open "sv << node << ": "sv << std::strerror(errno);
      return nullptr;
    }

    gbm_device *device = gbm_create_device(fd);
    if (!device) {
      BOOST_LOG(debug) << "GBM: "sv << node << " has no GBM device"sv;
      close(fd);
      return nullptr;
    }

    // Screencopy buffers are ordinary render targets, so the probe asks for the
    // smallest buffer of the same kind rather than a frame-sized one.
    gbm_bo *probe = gbm_bo_create(device, 64, 64, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
    if (!probe) {
      BOOST_LOG(debug) << "GBM: "sv << node << " opened but cannot allocate buffers"sv;
      gbm_device_destroy(device);
      close(fd);
      return nullptr;
    }
    gbm_bo_destroy(probe);

    out_fd = fd;
    return device;
  }

  /**
   * @brief Pick the render node the capture buffers will be allocated on.
   *
   * These buffers are handed to the compositor to copy the screen into, so they
   * have to come from the GPU the compositor renders on. "The first render node
   * that opens" is not that GPU on any machine with two of them: renderD128 and
   * renderD129 are handed out in module load order, so on a hybrid laptop the
   * first node is as likely as not the discrete card the compositor never
   * touches. Opening it succeeds, every frame after it fails, and what reaches
   * the user is a black stream with working audio and input - the encoders all
   * probe green, because an encoder probe never touches this path.
   *
   * The node is therefore taken, in order, from what the user configured, from
   * what the wlroots compositor was told to render on, and finally from a scan
   * that keeps the first device able to produce a buffer.
   */
  bool dmabuf_t::init_gbm() {
    if (gbm_device) {
      return true;
    }

    if (!config::video.adapter_name.empty()) {
      const auto &node = config::video.adapter_name;
      gbm_device = open_usable_gbm_device(node.c_str(), drm_fd);
      if (!gbm_device) {
        BOOST_LOG(error) << "Configured adapter_name "sv << node
                         << " cannot allocate capture buffers. Point it at the render node the "sv
                         << "compositor renders on, or clear it to let Hermes search."sv;
        return false;
      }
      BOOST_LOG(info) << "GBM capture device: "sv << node << " (adapter_name)"sv;
      return true;
    }

    // A wlroots compositor renders the captured output on this node whenever the
    // session bothered to name one, which makes it the right answer before any
    // guessing - notably in the container image, whose entrypoint points sway at
    // the encode GPU precisely because the first node is the wrong one. It stays
    // a hint: a value that cannot allocate falls through to the scan.
    if (const char *wlr_node = std::getenv("WLR_RENDER_DRM_DEVICE"); wlr_node && wlr_node[0]) {
      gbm_device = open_usable_gbm_device(wlr_node, drm_fd);
      if (gbm_device) {
        BOOST_LOG(info) << "GBM capture device: "sv << wlr_node << " (WLR_RENDER_DRM_DEVICE)"sv;
        return true;
      }
      BOOST_LOG(warning) << "WLR_RENDER_DRM_DEVICE names "sv << wlr_node
                         << ", which cannot allocate capture buffers; searching the other render nodes."sv;
    }

    drmDevice *devices[16];
    const int n = drmGetDevices2(0, devices, 16);
    if (n <= 0) {
      BOOST_LOG(error) << "No DRM devices found"sv;
      return false;
    }

    std::string rejected;
    for (int i = 0; i < n && !gbm_device; i++) {
      if (!(devices[i]->available_nodes & (1 << DRM_NODE_RENDER))) {
        continue;
      }
      const char *node = devices[i]->nodes[DRM_NODE_RENDER];
      if (!node) {
        continue;
      }
      gbm_device = open_usable_gbm_device(node, drm_fd);
      if (gbm_device) {
        BOOST_LOG(info) << "GBM capture device: "sv << node;
      } else {
        rejected += rejected.empty() ? node : ", "s + node;
      }
    }
    drmFreeDevices(devices, n);

    if (!gbm_device) {
      BOOST_LOG(error) << "No render node can allocate capture buffers"sv
                       << (rejected.empty() ? ""s : " (tried "s + rejected + ")"s)
                       << ". Set adapter_name to the render node the compositor renders on."sv;
      return false;
    }

    return true;
  }

  // Cleanup GBM
  void dmabuf_t::cleanup_gbm() {
    if (current_bo) {
      gbm_bo_destroy(current_bo);
      current_bo = nullptr;
    }

    if (current_wl_buffer) {
      wl_buffer_destroy(current_wl_buffer);
      current_wl_buffer = nullptr;
    }
  }

  dmabuf_t::dmabuf_t():
      status {READY},
      frames {},
      current_frame {&frames[0]},
      listener {
        &CLASS_CALL(dmabuf_t, buffer),
        &CLASS_CALL(dmabuf_t, flags),
        &CLASS_CALL(dmabuf_t, ready),
        &CLASS_CALL(dmabuf_t, failed),
        &CLASS_CALL(dmabuf_t, damage),
        &CLASS_CALL(dmabuf_t, linux_dmabuf),
        &CLASS_CALL(dmabuf_t, buffer_done),
      } {
  }

  // Start capture
  void dmabuf_t::listen(
    zwlr_screencopy_manager_v1 *screencopy_manager,
    zwp_linux_dmabuf_v1 *dmabuf_interface,
    wl_output *output,
    bool blend_cursor
  ) {
    this->dmabuf_interface = dmabuf_interface;
    // Reset state
    shm_info.supported = false;
    dmabuf_info.supported = false;

    // Create new frame
    auto frame = zwlr_screencopy_manager_v1_capture_output(
      screencopy_manager,
      blend_cursor ? 1 : 0,
      output
    );

    // Store frame data pointer for callbacks
    zwlr_screencopy_frame_v1_set_user_data(frame, this);

    // Add listener
    zwlr_screencopy_frame_v1_add_listener(frame, &listener, this);

    status = WAITING;
  }

  dmabuf_t::~dmabuf_t() {
    icc_destroy_session();
    cleanup_gbm();

    for (auto &frame : frames) {
      frame.destroy();
    }

    if (gbm_device) {
      gbm_device_destroy(gbm_device);
      gbm_device = nullptr;
    }

    // gbm_create_device() borrows the fd rather than adopting it, so destroying
    // the device leaves it open. Every reinit built a new dmabuf_t, so a capture
    // path that kept failing leaked one descriptor per retry.
    if (drm_fd >= 0) {
      close(drm_fd);
      drm_fd = -1;
    }
  }

  // Buffer format callback
  void dmabuf_t::buffer(
    zwlr_screencopy_frame_v1 *frame,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t stride
  ) {
    shm_info.supported = true;
    shm_info.format = format;
    shm_info.width = width;
    shm_info.height = height;
    shm_info.stride = stride;

    BOOST_LOG(debug) << "Screencopy supports SHM format: "sv << format;
  }

  // DMA-BUF format callback
  void dmabuf_t::linux_dmabuf(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t format,
    std::uint32_t width,
    std::uint32_t height
  ) {
    dmabuf_info.supported = true;
    dmabuf_info.format = format;
    dmabuf_info.width = width;
    dmabuf_info.height = height;

    BOOST_LOG(debug) << "Screencopy supports DMA-BUF format: "sv << format;
  }

  // Flags callback
  void dmabuf_t::flags(zwlr_screencopy_frame_v1 *frame, std::uint32_t flags) {
    y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
    BOOST_LOG(debug) << "Frame flags: "sv << flags << (y_invert ? " (y_invert)" : "");
  }

  // DMA-BUF creation helper
  void dmabuf_t::create_and_copy_dmabuf(zwlr_screencopy_frame_v1 *frame) {
    if (!init_gbm()) {
      note_gbm_failure();
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    // Create GBM buffer
    current_bo = gbm_bo_create(gbm_device, dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, GBM_BO_USE_RENDERING);
    if (!current_bo) {
      note_gbm_failure();
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "Failed to allocate a "sv << dmabuf_info.width << 'x' << dmabuf_info.height
                         << " capture buffer in format "sv << dmabuf_info.format
                         << ". The render node this was allocated on is not the one the compositor "sv
                         << "renders on, or does not support the format it asked for."sv;
      }
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    // Get buffer info
    int fd = gbm_bo_get_fd(current_bo);
    if (fd < 0) {
      BOOST_LOG(error) << "Failed to get buffer FD"sv;
      gbm_bo_destroy(current_bo);
      current_bo = nullptr;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    uint32_t stride = gbm_bo_get_stride(current_bo);
    uint64_t modifier = gbm_bo_get_modifier(current_bo);

    // Store in surface descriptor for later use
    auto next_frame = get_next_frame();
    next_frame->sd.fds[0] = fd;
    next_frame->sd.pitches[0] = stride;
    next_frame->sd.offsets[0] = 0;
    next_frame->sd.modifier = modifier;

    // Create linux-dmabuf buffer
    auto params = zwp_linux_dmabuf_v1_create_params(dmabuf_interface);
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride, modifier >> 32, modifier & 0xffffffff);

    // Add listener for buffer creation
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, frame);

    // Create Wayland buffer (async - callback will handle copy)
    zwp_linux_buffer_params_v1_create(params, dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, 0);
  }

  // Buffer done callback - time to create buffer
  void dmabuf_t::buffer_done(zwlr_screencopy_frame_v1 *frame) {
    auto next_frame = get_next_frame();

    // Prefer DMA-BUF if supported
    if (dmabuf_info.supported && dmabuf_interface) {
      // Store format info first
      next_frame->sd.fourcc = dmabuf_info.format;
      next_frame->sd.width = dmabuf_info.width;
      next_frame->sd.height = dmabuf_info.height;

      // Create and start copy
      create_and_copy_dmabuf(frame);
    } else if (shm_info.supported) {
      // Retrying will not grow a code path that was never written, so this is
      // counted as a failure rather than left to spin: the compositor offered
      // only the shared-memory buffer, and Hermes has no capture for it.
      note_gbm_failure();
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "The compositor offers screencopy only as a shared-memory buffer, "sv
                         << "which this capture backend does not implement. Capture needs "sv
                         << "zwp_linux_dmabuf_v1 on this output."sv;
      }
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
    } else {
      note_gbm_failure();
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "The compositor offered no buffer type for this frame."sv;
      }
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
    }
  }

  // Buffer params created callback
  void dmabuf_t::buffer_params_created(
    void *data,
    struct zwp_linux_buffer_params_v1 *params,
    struct wl_buffer *buffer
  ) {
    auto frame = static_cast<zwlr_screencopy_frame_v1 *>(data);
    auto self = static_cast<dmabuf_t *>(zwlr_screencopy_frame_v1_get_user_data(frame));

    // Store for cleanup
    self->current_wl_buffer = buffer;

    // Start the actual copy
    zwlr_screencopy_frame_v1_copy(frame, buffer);
  }

  // Buffer params failed callback
  void dmabuf_t::buffer_params_failed(
    void *data,
    struct zwp_linux_buffer_params_v1 *params
  ) {
    auto frame = static_cast<zwlr_screencopy_frame_v1 *>(data);
    auto self = static_cast<dmabuf_t *>(zwlr_screencopy_frame_v1_get_user_data(frame));

    note_gbm_failure();
    if (should_log_gbm_failure()) {
      BOOST_LOG(error) << "The compositor refused the capture buffer. It was allocated on a device "sv
                       << "the compositor cannot import from."sv;
    }
    self->cleanup_gbm();

    zwlr_screencopy_frame_v1_destroy(frame);
    self->status = REINIT;
  }

  // Ready callback
  void dmabuf_t::ready(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t tv_sec_hi,
    std::uint32_t tv_sec_lo,
    std::uint32_t tv_nsec
  ) {
    BOOST_LOG(debug) << "Frame ready"sv;

    // A frame arrived, so whatever went wrong before was transient.
    clear_gbm_failures();

    // Frame is ready for use, GBM buffer now contains screen content
    current_frame->destroy();
    current_frame = get_next_frame();

    // Keep the GBM buffer alive but destroy the Wayland objects
    if (current_wl_buffer) {
      wl_buffer_destroy(current_wl_buffer);
      current_wl_buffer = nullptr;
    }

    cleanup_gbm();

    zwlr_screencopy_frame_v1_destroy(frame);
    status = READY;
  }

  // Failed callback
  void dmabuf_t::failed(zwlr_screencopy_frame_v1 *frame) {
    BOOST_LOG(error) << "Frame capture failed"sv;

    // Clean up resources
    cleanup_gbm();
    auto next_frame = get_next_frame();
    next_frame->destroy();

    zwlr_screencopy_frame_v1_destroy(frame);
    status = REINIT;
  }

  void dmabuf_t::damage(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height
  ) {};

  // ---------------------------------------------------------------------------
  // ext-image-copy-capture
  //
  // Adapted from Dregu's draft for upstream Sunshine
  // (LizardByte/Sunshine#4788), which is where the protocol sequence below was
  // worked out. It differs in three places, each noted at the site: the render
  // device the compositor names is validated before it is trusted, the modifier
  // lists are owned rather than malloc'd, and buffer failures feed the same
  // streak counter the screencopy path uses so that a session which can never
  // produce a frame ends instead of retrying at frame rate.
  // ---------------------------------------------------------------------------

  static const struct zwp_linux_buffer_params_v1_listener icc_params_listener = {
    .created = CLASS_CALL(dmabuf_t, icc_buffer_params_created),
    .failed = CLASS_CALL(dmabuf_t, icc_buffer_params_failed),
  };

  static const struct ext_image_copy_capture_session_v1_listener icc_session_listener = {
    .buffer_size = CLASS_CALL(dmabuf_t, icc_session_buffer_size),
    .shm_format = CLASS_CALL(dmabuf_t, icc_session_shm_format),
    .dmabuf_device = CLASS_CALL(dmabuf_t, icc_session_dmabuf_device),
    .dmabuf_format = CLASS_CALL(dmabuf_t, icc_session_dmabuf_format),
    .done = CLASS_CALL(dmabuf_t, icc_session_done),
    .stopped = CLASS_CALL(dmabuf_t, icc_session_stopped),
  };

  static const struct ext_image_copy_capture_frame_v1_listener icc_frame_listener = {
    .transform = CLASS_CALL(dmabuf_t, icc_frame_transform),
    .damage = CLASS_CALL(dmabuf_t, icc_frame_damage),
    .presentation_time = CLASS_CALL(dmabuf_t, icc_frame_presentation_time),
    .ready = CLASS_CALL(dmabuf_t, icc_frame_ready),
    .failed = CLASS_CALL(dmabuf_t, icc_frame_failed),
  };

  void dmabuf_t::icc_destroy_session() {
    if (icc_session.frame) {
      ext_image_copy_capture_frame_v1_destroy(icc_session.frame);
      icc_session.frame = nullptr;
    }
    if (icc_session.session) {
      ext_image_copy_capture_session_v1_destroy(icc_session.session);
      icc_session.session = nullptr;
    }
    icc_session = {};
  }

  bool dmabuf_t::icc_listen(
    ext_image_copy_capture_manager_v1 *manager,
    ext_output_image_capture_source_manager_v1 *source_manager,
    zwp_linux_dmabuf_v1 *dmabuf_interface,
    wl_output *output,
    bool blend_cursor
  ) {
    if (!manager || !source_manager || !dmabuf_interface) {
      return false;
    }

    this->dmabuf_interface = dmabuf_interface;
    icc_destroy_session();
    icc_session.cursor = blend_cursor;

    auto source = ext_output_image_capture_source_manager_v1_create_source(source_manager, output);
    if (!source) {
      BOOST_LOG(error) << "Could not name the Wayland output as an image capture source"sv;
      return false;
    }

    auto session = ext_image_copy_capture_manager_v1_create_session(
      manager,
      source,
      blend_cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0
    );

    // The session holds its own reference to the source, so the source object
    // is of no further use here.
    ext_image_capture_source_v1_destroy(source);

    if (!session) {
      BOOST_LOG(error) << "Could not open an ext-image-copy-capture session"sv;
      return false;
    }

    ext_image_copy_capture_session_v1_set_user_data(session, this);
    ext_image_copy_capture_session_v1_add_listener(session, &icc_session_listener, this);
    icc_session.session = session;
    status = WAITING;

    return true;
  }

  void dmabuf_t::icc_capture(bool blend_cursor) {
    if (!icc_session.session || blend_cursor != icc_session.cursor) {
      // A cursor-blending change is a different session, not a different frame.
      status = REINIT;
      return;
    }

    if (!icc_session.done || icc_session.frame) {
      return;
    }

    auto frame = ext_image_copy_capture_session_v1_create_frame(icc_session.session);
    if (!frame) {
      note_gbm_failure();
      status = REINIT;
      return;
    }

    icc_session.frame = frame;
    ext_image_copy_capture_frame_v1_set_user_data(frame, this);
    ext_image_copy_capture_frame_v1_add_listener(frame, &icc_frame_listener, this);
    icc_create_and_copy_dmabuf(frame);
    status = WAITING;
  }

  void dmabuf_t::icc_create_and_copy_dmabuf(ext_image_copy_capture_frame_v1 *frame) {
    auto give_up = [&]() {
      note_gbm_failure();
      ext_image_copy_capture_frame_v1_destroy(frame);
      icc_session.frame = nullptr;
      status = REINIT;
    };

    if (!init_gbm()) {
      give_up();
      return;
    }

    if (!icc_session.width || !icc_session.height || !icc_session.format) {
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "The capture session negotiated no usable buffer format"sv;
      }
      give_up();
      return;
    }

    // Modifiers first: a compositor that offers them is telling us the layouts
    // it can write into, and an implicit-modifier buffer is only correct when it
    // offers none.
    if (auto it = icc_session.modifiers.find(icc_session.format);
        it != icc_session.modifiers.end() && !it->second.empty()) {
      current_bo = gbm_bo_create_with_modifiers(
        gbm_device,
        icc_session.width,
        icc_session.height,
        icc_session.format,
        it->second.data(),
        it->second.size()
      );
    }

    if (!current_bo) {
      current_bo = gbm_bo_create(gbm_device, icc_session.width, icc_session.height, icc_session.format, GBM_BO_USE_RENDERING);
    }

    if (!current_bo) {
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "Failed to allocate a "sv << icc_session.width << 'x' << icc_session.height
                         << " capture buffer in format "sv << icc_session.format
                         << ". The render node this was allocated on is not the one the compositor "sv
                         << "renders on, or does not support the format it asked for."sv;
      }
      give_up();
      return;
    }

    // Allocating with modifiers can hand back a buffer of several planes - an
    // AMD compression modifier does exactly that - and a compositor offered
    // only the first plane of one cannot use it. That failure arrives as a bare
    // "unknown reason" on the frame, with nothing to say a plane was missing.
    const std::uint64_t modifier = gbm_bo_get_modifier(current_bo);
    const int planes = gbm_bo_get_plane_count(current_bo);

    if (planes < 1 || planes > 4) {
      BOOST_LOG(error) << "Capture buffer has "sv << planes << " planes, which cannot be shared"sv;
      gbm_bo_destroy(current_bo);
      current_bo = nullptr;
      give_up();
      return;
    }

    auto next_frame = get_next_frame();
    auto params = zwp_linux_dmabuf_v1_create_params(dmabuf_interface);

    for (int plane = 0; plane < planes; ++plane) {
      int fd = gbm_bo_get_fd_for_plane(current_bo, plane);
      if (fd < 0) {
        BOOST_LOG(error) << "Failed to get buffer FD for plane "sv << plane;
        zwp_linux_buffer_params_v1_destroy(params);
        next_frame->destroy();
        gbm_bo_destroy(current_bo);
        current_bo = nullptr;
        give_up();
        return;
      }

      const std::uint32_t stride = gbm_bo_get_stride_for_plane(current_bo, plane);
      const std::uint32_t offset = gbm_bo_get_offset(current_bo, plane);

      next_frame->sd.fds[plane] = fd;
      next_frame->sd.pitches[plane] = stride;
      next_frame->sd.offsets[plane] = offset;

      zwp_linux_buffer_params_v1_add(params, fd, plane, offset, stride, modifier >> 32, modifier & 0xffffffff);
    }

    next_frame->sd.modifier = modifier;
    next_frame->sd.fourcc = icc_session.format;
    next_frame->sd.width = icc_session.width;
    next_frame->sd.height = icc_session.height;

    BOOST_LOG(debug) << "Capture buffer: "sv << icc_session.width << 'x' << icc_session.height
                     << ", "sv << planes << " plane(s), modifier "sv << std::hex << modifier << std::dec;

    zwp_linux_buffer_params_v1_set_user_data(params, this);
    zwp_linux_buffer_params_v1_add_listener(params, &icc_params_listener, this);
    zwp_linux_buffer_params_v1_create(params, icc_session.width, icc_session.height, icc_session.format, 0);
  }

  void dmabuf_t::icc_buffer_params_created(zwp_linux_buffer_params_v1 *params, struct wl_buffer *buffer) {
    current_wl_buffer = buffer;
    zwp_linux_buffer_params_v1_destroy(params);

    if (!icc_session.frame) {
      return;
    }

    ext_image_copy_capture_frame_v1_attach_buffer(icc_session.frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(icc_session.frame, 0, 0, icc_session.width, icc_session.height);
    ext_image_copy_capture_frame_v1_capture(icc_session.frame);
  }

  void dmabuf_t::icc_buffer_params_failed(zwp_linux_buffer_params_v1 *params) {
    note_gbm_failure();
    if (should_log_gbm_failure()) {
      BOOST_LOG(error) << "The compositor refused the capture buffer offered to it"sv;
    }

    zwp_linux_buffer_params_v1_destroy(params);
    cleanup_gbm();

    if (icc_session.frame) {
      ext_image_copy_capture_frame_v1_destroy(icc_session.frame);
      icc_session.frame = nullptr;
    }
    status = REINIT;
  }

  void dmabuf_t::icc_session_buffer_size(ext_image_copy_capture_session_v1 *session, std::uint32_t width, std::uint32_t height) {
    icc_session.width = width;
    icc_session.height = height;
    BOOST_LOG(debug) << "Capture session buffer size: "sv << width << 'x' << height;
  }

  void dmabuf_t::icc_session_shm_format(ext_image_copy_capture_session_v1 *session, std::uint32_t format) {
    // Recorded for the same reason the screencopy path records it: to be able to
    // say the compositor offered only shared memory, which nothing here can use.
    shm_info.supported = true;
    shm_info.format = format;
    BOOST_LOG(debug) << "Capture session SHM format: "sv << format;
  }

  void dmabuf_t::icc_session_dmabuf_device(ext_image_copy_capture_session_v1 *session, struct wl_array *device) {
    // This is the one thing screencopy never tells us: the compositor names the
    // device it renders on, which is the answer init_gbm() otherwise has to
    // guess at. Trust it, but prove it the same way - a named device that cannot
    // allocate is still no use, and falling through to the search leaves the
    // hybrid-GPU case working rather than trading one wrong node for another.
    if (gbm_device) {
      return;
    }

    if (device->size != sizeof(dev_t)) {
      BOOST_LOG(warning) << "Capture session named a device of "sv << device->size
                         << " bytes, expected "sv << sizeof(dev_t) << "; searching instead."sv;
      return;
    }

    dev_t device_id;
    std::memcpy(&device_id, device->data, sizeof(dev_t));

    drmDevice *drm_device {nullptr};
    if (drmGetDeviceFromDevId(device_id, 0, &drm_device) != 0) {
      BOOST_LOG(warning) << "Capture session named a DRM device that could not be opened; searching instead."sv;
      return;
    }

    const char *node {nullptr};
    if (drm_device->available_nodes & (1 << DRM_NODE_RENDER)) {
      node = drm_device->nodes[DRM_NODE_RENDER];
    } else if (drm_device->available_nodes & (1 << DRM_NODE_PRIMARY)) {
      node = drm_device->nodes[DRM_NODE_PRIMARY];
    }

    if (node) {
      gbm_device = open_usable_gbm_device(node, drm_fd);
      if (gbm_device) {
        BOOST_LOG(info) << "GBM capture device: "sv << node << " (named by the compositor)"sv;
      } else {
        BOOST_LOG(warning) << "The compositor renders on "sv << node
                           << ", which cannot allocate capture buffers; searching the other render nodes."sv;
      }
    }

    drmFreeDevice(&drm_device);
  }

  void dmabuf_t::icc_session_dmabuf_format(ext_image_copy_capture_session_v1 *session, std::uint32_t format, struct wl_array *modifiers) {
    icc_session.dmabuf_supported = true;

    auto &list = icc_session.modifiers[format];
    list.assign(
      (const std::uint64_t *) modifiers->data,
      (const std::uint64_t *) modifiers->data + modifiers->size / sizeof(std::uint64_t)
    );

    // The compositor sends its preferred format first, so the first one that
    // arrives is the one to allocate in.
    if (!icc_session.format) {
      icc_session.format = format;
    }

    char fourcc[5] {};
    std::memcpy(fourcc, &format, 4);
    BOOST_LOG(debug) << "Capture session DMA-BUF format: "sv << fourcc << " with "sv << list.size() << " modifier(s)"sv;
  }

  void dmabuf_t::icc_session_done(ext_image_copy_capture_session_v1 *session) {
    if (!icc_session.session) {
      status = REINIT;
      return;
    }

    if (!icc_session.width || !icc_session.height) {
      BOOST_LOG(error) << "Capture session negotiated a zero-sized buffer"sv;
      status = REINIT;
      return;
    }

    if (!icc_session.dmabuf_supported) {
      BOOST_LOG(error) << "The compositor offers capture only as a shared-memory buffer, "sv
                       << "which this capture backend does not implement."sv;
      status = REINIT;
      return;
    }

    icc_session.done = true;
    BOOST_LOG(debug) << "Capture session ready"sv;
  }

  void dmabuf_t::icc_session_stopped(ext_image_copy_capture_session_v1 *session) {
    BOOST_LOG(warning) << "The compositor stopped the capture session"sv;
    icc_destroy_session();
    status = REINIT;
  }

  void dmabuf_t::icc_frame_transform(ext_image_copy_capture_frame_v1 *frame, std::uint32_t transform) {
    BOOST_LOG(debug) << "Capture frame transform: "sv << transform;
  }

  void dmabuf_t::icc_frame_damage(
    ext_image_copy_capture_frame_v1 *frame,
    std::int32_t x,
    std::int32_t y,
    std::int32_t width,
    std::int32_t height
  ) {}

  void dmabuf_t::icc_frame_presentation_time(
    ext_image_copy_capture_frame_v1 *frame,
    std::uint32_t tv_sec_hi,
    std::uint32_t tv_sec_lo,
    std::uint32_t tv_nsec
  ) {}

  void dmabuf_t::icc_frame_ready(ext_image_copy_capture_frame_v1 *frame) {
    clear_gbm_failures();

    current_frame->destroy();
    current_frame = get_next_frame();

    if (current_wl_buffer) {
      wl_buffer_destroy(current_wl_buffer);
      current_wl_buffer = nullptr;
    }
    cleanup_gbm();

    ext_image_copy_capture_frame_v1_destroy(frame);
    icc_session.frame = nullptr;
    status = READY;
  }

  void dmabuf_t::icc_frame_failed(ext_image_copy_capture_frame_v1 *frame, std::uint32_t reason) {
    note_gbm_failure();

    ext_image_copy_capture_frame_v1_destroy(frame);
    icc_session.frame = nullptr;

    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "The capture session stopped while a frame was in flight"sv;
      }
      cleanup_gbm();
      get_next_frame()->destroy();
      icc_destroy_session();
      status = REINIT;
      return;
    }

    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
      // The negotiated constraints no longer describe the output - a mode change,
      // most often. The session renegotiates them, so drop what we cached.
      if (should_log_gbm_failure()) {
        BOOST_LOG(warning) << "Capture buffer no longer meets the session's constraints; renegotiating"sv;
      }
      cleanup_gbm();
      status = REINIT;
      return;
    }

    if (should_log_gbm_failure()) {
      BOOST_LOG(error) << "The compositor failed the capture frame for an unknown reason ("sv << reason << ')';
    }
    status = REINIT;
  }

  void frame_t::destroy() {
    for (auto x = 0; x < 4; ++x) {
      if (sd.fds[x] >= 0) {
        close(sd.fds[x]);

        sd.fds[x] = -1;
      }
    }
  }

  frame_t::frame_t() {
    // File descriptors aren't open
    std::fill_n(sd.fds, 4, -1);
  };

  std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name) {
    display_t display;

    if (display.init(display_name)) {
      return {};
    }

    interface_t interface;
    interface.listen(display.registry());

    display.roundtrip();

    if (!interface[interface_t::XDG_OUTPUT]) {
      BOOST_LOG(error) << "Missing Wayland wire XDG_OUTPUT"sv;
      return {};
    }

    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }

    display.roundtrip();

    return std::move(interface.monitors);
  }

  bool render_node_can_allocate(const char *node) {
    if (!node || !node[0]) {
      return false;
    }
    int fd = -1;
    gbm_device *device = open_usable_gbm_device(node, fd);
    if (!device) {
      return false;
    }
    gbm_device_destroy(device);
    // gbm_create_device() borrows the fd rather than adopting it.
    ::close(fd);
    return true;
  }

  bool configure_virtual_output(const std::string &output_name, int width, int height, int refresh_rate, bool exclusive) {
    for (int attempt = 0; attempt < 5; ++attempt) {
      output_control_t control;
      if (!control.init()) {
        BOOST_LOG(warning) << "Wayland compositor does not expose wlr-output-management; cannot activate virtual output "sv << output_name;
        return false;
      }

      if (!control.collect()) {
        // Not a verdict on the layout, just a layout we have not been told about
        // yet: collect() only succeeds once the manager has sent its first done
        // event. A compositor sitting at zero outputs — a headless session, or a
        // container that has just come up — can still be assembling that state in
        // the roundtrip we make here. Returning now would abandon the activation
        // in the same millisecond the connector was created, before the compositor
        // could possibly have processed the hotplug, so retry on the same footing
        // as a head that has not appeared yet.
        std::this_thread::sleep_for(100ms);
        continue;
      }

      auto *virtual_head = control.find_head(output_name);
      if (!virtual_head) {
        // Virtual DRM connectors are published asynchronously. Give the compositor
        // a short window to process the hotplug before declaring it unavailable.
        std::this_thread::sleep_for(100ms);
        continue;
      }

      std::map<std::string, saved_output_t> previous_layout;
      if (exclusive) {
        for (const auto &head : control.heads) {
          if (head.get() == virtual_head) {
            continue;
          }
          const auto *mode = preferred_or_current_mode(*head);
          previous_layout[head->name] = {
            .enabled = head->enabled,
            .width = mode ? mode->width : 0,
            .height = mode ? mode->height : 0,
            .refresh_rate = mode ? mode->refresh_rate : 0,
            .x = head->x,
            .y = head->y,
            .transform = head->transform,
            .scale = head->scale,
          };
        }
      }

      const bool applied = apply_configuration(control, *virtual_head, width, height, refresh_rate, exclusive, false);
      if (applied && exclusive) {
        saved_output_layout = std::move(previous_layout);
        saved_output_layout_available = true;
      }
      if (!applied) {
        BOOST_LOG(warning) << "Wayland compositor rejected the virtual output layout for "sv << output_name;
      }
      return applied;
    }

    BOOST_LOG(warning) << "Virtual connector "sv << output_name << " did not appear in the Wayland output layout."sv;
    return false;
  }

  bool restore_virtual_output_layout() {
    if (!saved_output_layout_available) {
      return true;
    }

    output_control_t control;
    if (!control.init() || !control.collect() || control.heads.empty()) {
      BOOST_LOG(warning) << "Cannot restore the Wayland physical output layout."sv;
      return false;
    }

    const bool restored = apply_configuration(control, *control.heads.front(), 0, 0, 0, false, true);
    if (restored) {
      saved_output_layout.clear();
      saved_output_layout_available = false;
      BOOST_LOG(info) << "Restored Wayland physical output layout."sv;
    } else {
      BOOST_LOG(warning) << "Wayland compositor rejected restoration of the physical output layout."sv;
    }
    return restored;
  }

  bool output_management_supported() {
    output_control_t control;
    return control.init() && control.collect();
  }

  session_protocols_t probe_protocols() {
    session_protocols_t protocols {};

    display_t display;
    if (display.init() != 0) {
      // Not a failure worth logging as an error: an X11 session reaching the
      // diagnostics path is the ordinary case, and `connected` says so.
      return protocols;
    }
    protocols.connected = true;

    // A probe must not require bindings for protocols Hermes does not
    // implement, so the two ext-* globals are matched by name. They are staging
    // protocols whose names are part of their wire identity and cannot change
    // without a new global, which is exactly what makes the string safe here.
    static constexpr auto image_copy_capture_name = "ext_image_copy_capture_manager_v1"sv;
    static constexpr auto output_capture_source_name = "ext_output_image_capture_source_manager_v1"sv;

    struct probe_state_t {
      session_protocols_t *protocols;
      wl_registry_listener listener;
    };

    probe_state_t state {
      .protocols = &protocols,
      .listener = {
        .global = [](void *data, wl_registry *, std::uint32_t, const char *interface, std::uint32_t) {
          auto *found = static_cast<probe_state_t *>(data)->protocols;
          const std::string_view name {interface};
          if (name == zwlr_output_manager_v1_interface.name) {
            found->output_management = true;
          } else if (name == zwlr_screencopy_manager_v1_interface.name) {
            found->screencopy = true;
          } else if (name == zwp_linux_dmabuf_v1_interface.name) {
            found->linux_dmabuf = true;
          } else if (name == zxdg_output_manager_v1_interface.name) {
            found->xdg_output = true;
          } else if (name == image_copy_capture_name) {
            found->image_copy_capture = true;
          } else if (name == output_capture_source_name) {
            found->output_capture_source = true;
          }
        },
        .global_remove = [](void *, wl_registry *, std::uint32_t) {},
      },
    };

    wl_registry_add_listener(display.registry(), &state.listener, &state);
    display.roundtrip();

    return protocols;
  }

  static bool validate() {
    display_t display;

    return display.init() == 0;
  }

  int init() {
    static bool validated = validate();

    return !validated;
  }

}  // namespace wl

#pragma GCC diagnostic pop
