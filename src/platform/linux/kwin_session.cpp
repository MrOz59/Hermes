/**
 * @file src/platform/linux/kwin_session.cpp
 * @brief Private KWin Wayland transport; no implicit host connection.
 */
#include "kwin_session.h"

#include "fake-input.h"
#include "session_socket.h"
#include "zkde-screencast-unstable-v1.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace platf::kwin {
  namespace {
    using clock_t = std::chrono::steady_clock;
    constexpr auto setup_timeout = std::chrono::seconds(5);

    int remaining_ms(clock_t::time_point deadline) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_t::now()).count();
      return static_cast<int>(std::clamp<std::int64_t>(left, 0, std::numeric_limits<int>::max()));
    }

    bool fixed_valid(double value) {
      return std::isfinite(value) && value >= -8388608.0 && value < 8388608.0;
    }
  }  // namespace

  struct connection_t::impl_t {
    struct monitor_t {
      wl_output *proxy {};
      std::uint32_t global {};
      output_t info;
      bool removed {};
    };

    mutable std::mutex mutex;
    wl_display *display {};
    wl_registry *registry {};
    org_kde_kwin_fake_input *input {};
    zkde_screencast_unstable_v1 *capture {};
    zkde_screencast_stream_unstable_v1 *stream {};
    std::vector<std::unique_ptr<monitor_t>> monitors;
    monitor_t *selected {};
    output_t geometry;
    std::uint32_t capture_global {};
    std::uint32_t input_global {};
    std::uint64_t serial {};
    bool node_ready {};
    bool dead {};
    std::string failure;

    ~impl_t() {
      if (stream) {
        zkde_screencast_stream_unstable_v1_close(stream);
      }
      if (input) {
        org_kde_kwin_fake_input_destroy(input);
      }
      if (capture) {
        zkde_screencast_unstable_v1_destroy(capture);
      }
      for (const auto &monitor : monitors) {
        wl_output_destroy(monitor->proxy);
      }
      if (registry) {
        wl_registry_destroy(registry);
      }
      if (display) {
        wl_display_disconnect(display);
      }
    }

    void fail(std::string message) {
      if (!dead) {
        failure = std::move(message);
      }
      dead = true;
    }

    bool flush() {
      if (dead) {
        return false;
      }
      if (wl_display_flush(display) < 0 && errno != EAGAIN) {
        fail("Private Wayland connection failed: " + std::string(std::strerror(errno)));
      }
      return !dead;
    }

    bool pump(std::chrono::milliseconds timeout) {
      if (dead) {
        return false;
      }
      const auto deadline = clock_t::now() + timeout;
      while (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) < 0) {
          fail("Private Wayland event dispatch failed");
          return false;
        }
        if (dead) {
          return false;
        }
      }
      short events = POLLIN;
      if (wl_display_flush(display) < 0) {
        if (errno == EAGAIN) {
          events |= POLLOUT;
        } else {
          wl_display_cancel_read(display);
          fail("Private Wayland flush failed");
          return false;
        }
      }
      pollfd event {wl_display_get_fd(display), events, 0};
      int result;
      do {
        result = ::poll(&event, 1, remaining_ms(deadline));
      } while (result < 0 && errno == EINTR && clock_t::now() < deadline);
      if (result < 0 || (event.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        wl_display_cancel_read(display);
        fail("Private compositor connection closed");
        return false;
      }
      if (event.revents & POLLIN) {
        if (wl_display_read_events(display) < 0) {
          fail("Private Wayland read failed");
          return false;
        }
      } else {
        wl_display_cancel_read(display);
      }
      if ((event.revents & POLLOUT) && !flush()) {
        return false;
      }
      if (wl_display_dispatch_pending(display) < 0) {
        fail("Private Wayland protocol error");
      }
      if (selected && (selected->info.width != geometry.width || selected->info.height != geometry.height ||
                       selected->info.x != geometry.x || selected->info.y != geometry.y || selected->info.scale != geometry.scale)) {
        fail("Private output geometry changed; a new capture session is required");
      }
      return !dead;
    }

    bool sync() {
      bool complete = false;
      auto callback = wl_display_sync(display);
      if (!callback) {
        fail("Could not create private Wayland synchronization request");
        return false;
      }
      static const wl_callback_listener listener {
        [](void *data, wl_callback *, std::uint32_t) {
          *static_cast<bool *>(data) = true;
        },
      };
      wl_callback_add_listener(callback, &listener, &complete);
      const auto deadline = clock_t::now() + setup_timeout;
      while (!complete && !dead && clock_t::now() < deadline) {
        pump(std::chrono::milliseconds(remaining_ms(deadline)));
      }
      wl_callback_destroy(callback);
      if (!complete && !dead) {
        fail("Private compositor synchronization timed out");
      }
      return complete && !dead;
    }

    static void announced(void *data, wl_registry *registry, std::uint32_t name, const char *interface, std::uint32_t version) {
      auto &self = *static_cast<impl_t *>(data);
      if (std::strcmp(interface, "wl_output") == 0 && version >= 2) {
        auto monitor = std::make_unique<monitor_t>();
        monitor->global = name;
        monitor->proxy = static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4u)));
        static const wl_output_listener listener {
          [](void *data, wl_output *, int x, int y, int, int, int, const char *, const char *, int) {
            auto &info = static_cast<monitor_t *>(data)->info;
            info.x = x;
            info.y = y;
          },
          [](void *data, wl_output *, std::uint32_t flags, int width, int height, int) {
            if (flags & WL_OUTPUT_MODE_CURRENT) {
              auto &info = static_cast<monitor_t *>(data)->info;
              info.width = width;
              info.height = height;
            }
          },
          [](void *, wl_output *) {
          },
          [](void *data, wl_output *, int scale) {
            static_cast<monitor_t *>(data)->info.scale = scale;
          },
          [](void *data, wl_output *, const char *name) {
            static_cast<monitor_t *>(data)->info.name = name;
          },
          [](void *, wl_output *, const char *) {
          },
        };
        wl_output_add_listener(monitor->proxy, &listener, monitor.get());
        self.monitors.emplace_back(std::move(monitor));
      } else if (std::strcmp(interface, "org_kde_kwin_fake_input") == 0 && version >= 5 && !self.input) {
        self.input_global = name;
        self.input = static_cast<org_kde_kwin_fake_input *>(wl_registry_bind(registry, name, &org_kde_kwin_fake_input_interface, std::min(version, 6u)));
      } else if (std::strcmp(interface, "zkde_screencast_unstable_v1") == 0 && version >= 6 && !self.capture) {
        self.capture_global = name;
        // Object serials avoid reconnecting to a recycled PipeWire node ID.
        self.capture = static_cast<zkde_screencast_unstable_v1 *>(wl_registry_bind(registry, name, &zkde_screencast_unstable_v1_interface, 6));
      }
    }

    static void removed(void *data, wl_registry *, std::uint32_t name) {
      auto &self = *static_cast<impl_t *>(data);
      for (auto &monitor : self.monitors) {
        if (monitor->global == name) {
          monitor->removed = true;
          if (self.selected == monitor.get()) {
            self.fail("Private output was removed");
          }
        }
      }
      if (name == self.capture_global || name == self.input_global) {
        self.fail("Private compositor removed a required protocol");
      }
    }
  };

  connection_t::connection_t(std::unique_ptr<impl_t> state):
      impl(std::move(state)) {}

  connection_t::~connection_t() = default;

  std::unique_ptr<connection_t> connection_t::open(const endpoint_t &endpoint, bool capture, bool input, std::string &error) {
    error.clear();
    auto state = std::make_unique<impl_t>();
    int fd = connect_session_socket(endpoint.wayland_socket, error);
    if (fd < 0) {
      return nullptr;
    }
    // libwayland takes ownership, including on connection-construction failure.
    state->display = wl_display_connect_to_fd(fd);
    if (!state->display) {
      error = "Could not initialize private Wayland connection";
      return nullptr;
    }
    state->registry = wl_display_get_registry(state->display);
    if (!state->registry) {
      error = "Could not create private Wayland registry";
      return nullptr;
    }
    static const wl_registry_listener listener {impl_t::announced, impl_t::removed};
    wl_registry_add_listener(state->registry, &listener, state.get());
    if (!state->sync() || !state->sync()) {
      error = state->failure;
      return nullptr;
    }
    if ((capture && !state->capture) || (input && !state->input)) {
      error = "Private KWin capture/input protocol unavailable or executable not authorized";
      return nullptr;
    }
    std::vector<impl_t::monitor_t *> matches;
    for (auto &monitor : state->monitors) {
      if (!monitor->removed && (endpoint.output_name.empty() || monitor->info.name == endpoint.output_name)) {
        matches.push_back(monitor.get());
      }
    }
    if (matches.size() != 1 || matches.front()->info.width <= 0 || matches.front()->info.height <= 0 || matches.front()->info.scale <= 0) {
      error = "Private output selection is missing, ambiguous, or has no valid mode";
      return nullptr;
    }
    state->selected = matches.front();
    state->geometry = state->selected->info;
    if (input) {
      org_kde_kwin_fake_input_authenticate(state->input, "Hermes", "Remote input for this detached desktop");
      if (!state->sync()) {
        error = state->failure;
        return nullptr;
      }
    }
    return std::unique_ptr<connection_t>(new connection_t(std::move(state)));
  }

  const output_t &connection_t::output() const {
    return impl->geometry;
  }

  bool connection_t::start_capture(bool cursor, std::uint64_t &serial, std::string &error) {
    std::lock_guard lock(impl->mutex);
    if (impl->dead || !impl->capture || impl->stream) {
      error = impl->dead ? impl->failure : "Private capture is unavailable or already started";
      return false;
    }
    impl->stream = zkde_screencast_unstable_v1_stream_output(impl->capture, impl->selected->proxy, cursor ? ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED : ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN);
    if (!impl->stream) {
      impl->fail("Could not create private capture request");
      error = impl->failure;
      return false;
    }
    static const zkde_screencast_stream_unstable_v1_listener listener {
      [](void *data, zkde_screencast_stream_unstable_v1 *) {
        static_cast<impl_t *>(data)->fail("Private capture stream closed");
      },
      [](void *, zkde_screencast_stream_unstable_v1 *, std::uint32_t) {
      },
      [](void *data, zkde_screencast_stream_unstable_v1 *, const char *message) {
        static_cast<impl_t *>(data)->fail("Private capture failed: " + std::string(message));
      },
      [](void *data, zkde_screencast_stream_unstable_v1 *, std::uint32_t high, std::uint32_t low) {
        auto &self = *static_cast<impl_t *>(data);
        self.serial = (static_cast<std::uint64_t>(high) << 32) | low;
        self.node_ready = true;
      },
    };
    zkde_screencast_stream_unstable_v1_add_listener(impl->stream, &listener, impl.get());
    const auto deadline = clock_t::now() + setup_timeout;
    while (!impl->node_ready && !impl->dead && clock_t::now() < deadline) {
      impl->pump(std::chrono::milliseconds(remaining_ms(deadline)));
    }
    if (!impl->node_ready && !impl->dead) {
      impl->fail("Private capture stream creation timed out");
    }
    if (impl->dead) {
      error = impl->failure;
      return false;
    }
    serial = impl->serial;
    return true;
  }

  bool connection_t::dispatch(std::chrono::milliseconds timeout) {
    std::lock_guard lock(impl->mutex);
    return impl->pump(std::min(timeout, std::chrono::milliseconds(50)));
  }

  bool connection_t::pointer_motion(double dx, double dy) {
    std::lock_guard lock(impl->mutex);
    if (!impl->pump(std::chrono::milliseconds(0)) || !impl->input || !fixed_valid(dx) || !fixed_valid(dy)) {
      return false;
    }
    org_kde_kwin_fake_input_pointer_motion(impl->input, wl_fixed_from_double(dx), wl_fixed_from_double(dy));
    return impl->flush();
  }

  bool connection_t::pointer_absolute(double x, double y) {
    std::lock_guard lock(impl->mutex);
    if (!impl->pump(std::chrono::milliseconds(0)) || !impl->input || !fixed_valid(x) || !fixed_valid(y)) {
      return false;
    }
    org_kde_kwin_fake_input_pointer_motion_absolute(impl->input, wl_fixed_from_double(x), wl_fixed_from_double(y));
    return impl->flush();
  }

  bool connection_t::button(std::uint32_t button, bool pressed) {
    std::lock_guard lock(impl->mutex);
    if (!impl->pump(std::chrono::milliseconds(0)) || !impl->input) {
      return false;
    }
    org_kde_kwin_fake_input_button(impl->input, button, pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    return impl->flush();
  }

  bool connection_t::axis(bool horizontal, double value) {
    std::lock_guard lock(impl->mutex);
    if (!impl->pump(std::chrono::milliseconds(0)) || !impl->input || !fixed_valid(value)) {
      return false;
    }
    org_kde_kwin_fake_input_axis(impl->input, horizontal ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(value));
    return impl->flush();
  }

  bool connection_t::key(std::uint32_t key, bool pressed) {
    std::lock_guard lock(impl->mutex);
    if (!impl->pump(std::chrono::milliseconds(0)) || !impl->input) {
      return false;
    }
    org_kde_kwin_fake_input_keyboard_key(impl->input, key, pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    return impl->flush();
  }

  bool connection_t::touch_down(std::uint32_t id, double x, double y) {
    std::lock_guard lock(impl->mutex);
    if (!impl->pump(std::chrono::milliseconds(0)) || !impl->input || !fixed_valid(x) || !fixed_valid(y)) {
      return false;
    }
    org_kde_kwin_fake_input_touch_down(impl->input, id, wl_fixed_from_double(x), wl_fixed_from_double(y));
    org_kde_kwin_fake_input_touch_frame(impl->input);
    return impl->flush();
  }

  bool connection_t::touch_motion(std::uint32_t id, double x, double y) {
    std::lock_guard lock(impl->mutex);
    if (!impl->pump(std::chrono::milliseconds(0)) || !impl->input || !fixed_valid(x) || !fixed_valid(y)) {
      return false;
    }
    org_kde_kwin_fake_input_touch_motion(impl->input, id, wl_fixed_from_double(x), wl_fixed_from_double(y));
    org_kde_kwin_fake_input_touch_frame(impl->input);
    return impl->flush();
  }

  bool connection_t::touch_up(std::uint32_t id) {
    std::lock_guard lock(impl->mutex);
    if (!impl->pump(std::chrono::milliseconds(0)) || !impl->input) {
      return false;
    }
    org_kde_kwin_fake_input_touch_up(impl->input, id);
    org_kde_kwin_fake_input_touch_frame(impl->input);
    return impl->flush();
  }

  bool connection_t::keysym(std::uint32_t symbol, bool pressed) {
    std::lock_guard lock(impl->mutex);
    if (!impl->pump(std::chrono::milliseconds(0)) || !impl->input || org_kde_kwin_fake_input_get_version(impl->input) < 6) {
      return false;
    }
    org_kde_kwin_fake_input_keyboard_keysym(impl->input, symbol, pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    return impl->flush();
  }

  std::string connection_t::error() const {
    std::lock_guard lock(impl->mutex);
    return impl->failure;
  }
}  // namespace platf::kwin
