/**
 * @file src/platform/linux/virtual_display.cpp
 * @brief Virtual display implementation for Linux using EVDI.
 *
 * This implementation provides virtual display support on Linux using
 * EVDI (Extensible Virtual Display Interface) for creating true virtual
 * displays that are separate from physical monitors.
 *
 * When EVDI is unavailable, virtual-display sessions are disabled rather than
 * silently capturing an unrelated physical monitor.
 */

// standard includes
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

// third-party includes
#include <nlohmann/json.hpp>

// platform includes
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>
#ifdef SUNSHINE_BUILD_SDBUS
  #include <systemd/sd-bus.h>
#endif
#include <xf86drm.h>
#include <xf86drmMode.h>

// local includes
#include "card_broker.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/utility.h"
#include "virtual_display.h"
#ifdef SUNSHINE_BUILD_WAYLAND
  #include "wayland.h"
#endif

using namespace std::literals;
namespace fs = std::filesystem;

namespace VDISPLAY {

  // ============================================================================
  // EVDI Types and Function Pointers (loaded dynamically)
  // ============================================================================

  // EVDI structures (matching evdi_lib.h)
  struct evdi_lib_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
  };

  struct evdi_device_context;
  typedef struct evdi_device_context *evdi_handle;

  enum evdi_device_status {
    EVDI_AVAILABLE,
    EVDI_UNRECOGNIZED,
    EVDI_NOT_PRESENT
  };

  struct evdi_mode {
    int width;
    int height;
    int refresh_rate;
    int bits_per_pixel;
    unsigned int pixel_format;
  };

  struct evdi_rect {
    int x1, y1, x2, y2;
  };

  struct evdi_buffer {
    int id;
    void *buffer;
    int width;
    int height;
    int stride;
    struct evdi_rect *rects;
    int rect_count;
  };

  struct evdi_cursor_set {
    int32_t hot_x;
    int32_t hot_y;
    uint32_t width;
    uint32_t height;
    uint8_t enabled;
    uint32_t buffer_length;
    uint32_t *buffer;
    uint32_t pixel_format;
    uint32_t stride;
  };

  struct evdi_cursor_move {
    int32_t x;
    int32_t y;
  };

  struct evdi_ddcci_data {
    uint16_t address;
    uint16_t flags;
    uint32_t buffer_length;
    uint8_t *buffer;
  };

  struct evdi_event_context {
    void (*dpms_handler)(int dpms_mode, void *user_data);
    void (*mode_changed_handler)(struct evdi_mode mode, void *user_data);
    void (*update_ready_handler)(int buffer_to_be_updated, void *user_data);
    void (*crtc_state_handler)(int state, void *user_data);
    void (*cursor_set_handler)(struct evdi_cursor_set cursor_set, void *user_data);
    void (*cursor_move_handler)(struct evdi_cursor_move cursor_move, void *user_data);
    void (*ddcci_data_handler)(struct evdi_ddcci_data ddcci_data, void *user_data);
    void *user_data;
  };

  // EVDI function pointer types
  typedef evdi_device_status (*fn_evdi_check_device)(int device);
  typedef evdi_handle (*fn_evdi_open)(int device);
  typedef int (*fn_evdi_add_device)(void);
  typedef void (*fn_evdi_close)(evdi_handle handle);
  typedef void (*fn_evdi_connect)(evdi_handle handle, const unsigned char *edid,
                                   const unsigned int edid_length,
                                   const uint32_t sku_area_limit);
  typedef void (*fn_evdi_disconnect)(evdi_handle handle);
  typedef void (*fn_evdi_grab_pixels)(evdi_handle handle, struct evdi_rect *rects, int *num_rects);
  typedef void (*fn_evdi_register_buffer)(evdi_handle handle, struct evdi_buffer buffer);
  typedef void (*fn_evdi_unregister_buffer)(evdi_handle handle, int bufferId);
  typedef bool (*fn_evdi_request_update)(evdi_handle handle, int bufferId);
  typedef void (*fn_evdi_handle_events)(evdi_handle handle, struct evdi_event_context *evtctx);
  typedef int (*fn_evdi_get_event_ready)(evdi_handle handle);
  typedef void (*fn_evdi_get_lib_version)(struct evdi_lib_version *version);

  // EVDI function pointers (loaded at runtime)
  static struct {
    void *lib_handle;
    fn_evdi_check_device check_device;
    fn_evdi_open open;
    fn_evdi_add_device add_device;
    fn_evdi_close close;
    fn_evdi_connect connect;
    fn_evdi_disconnect disconnect;
    fn_evdi_grab_pixels grab_pixels;
    fn_evdi_register_buffer register_buffer;
    fn_evdi_unregister_buffer unregister_buffer;
    fn_evdi_request_update request_update;
    fn_evdi_handle_events handle_events;
    fn_evdi_get_event_ready get_event_ready;
    fn_evdi_get_lib_version get_lib_version;
    bool loaded;
  } evdi = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false};

  // ============================================================================
  // Standard 1920x1080 EDID (used for virtual display)
  // ============================================================================

  // EDID for a generic 1920x1080@60Hz monitor
  static const unsigned char default_edid[] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,  // Header
    0x1E, 0x6D,  // Manufacturer ID (LG Display)
    0x00, 0x00,  // Product code
    0x01, 0x01, 0x01, 0x01,  // Serial number
    0x00, 0x1D,  // Week/Year of manufacture
    0x01, 0x04,  // EDID version 1.4
    0xB5,        // Video input (digital, 8-bit color depth, DisplayPort)
    0x3C, 0x22,  // Width/height in cm (60x34 = approx 27")
    0x78,        // Gamma 2.2
    0x3A,        // Features (RGB, preferred timing)
    // Chromaticity
    0xFC, 0x81, 0xA4, 0x55, 0x4D, 0x9D, 0x25, 0x12, 0x50, 0x54,
    // Established timings
    0x21, 0x08, 0x00,
    // Standard timings
    0xD1, 0xC0,  // 1920x1080@60Hz
    0x81, 0x80,  // 1280x1024@60Hz
    0x81, 0xC0,  // 1280x720@60Hz
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    // Detailed timing descriptor: 1920x1080@60Hz
    0x02, 0x3A,  // Pixel clock: 148.5 MHz
    0x80, 0x18, 0x71, 0x38, 0x2D, 0x40,
    0x58, 0x2C, 0x45, 0x00,
    0x56, 0x50, 0x21, 0x00, 0x00, 0x1E,
    // Display name descriptor
    0x00, 0x00, 0x00, 0xFC, 0x00,
    'A', 'P', 'O', 'L', 'L', 'O', ' ', 'V', 'D', 'I', 'S', 'P', '\n',
    // Display range limits
    0x00, 0x00, 0x00, 0xFD, 0x00,
    0x32, 0x4B, 0x1E, 0x51, 0x11, 0x00, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    // Extension flag and checksum (calculated)
    0x00, 0x00
  };

  // ============================================================================
  // Global State
  // ============================================================================

  static std::mutex vdisplay_mutex;
  static DRIVER_STATUS driver_status = DRIVER_STATUS::UNKNOWN;
  static std::atomic<bool> watchdog_running {false};
  // Guards closeVDisplayDevice() against running its teardown more than once.
  static std::atomic<bool> device_open {false};
  static std::thread watchdog_thread;
  static bool evdi_available = false;
  static bool exclusive_virtual_display_active = false;
  static std::atomic<bool> virtual_display_capture_fallback_active {false};
  static std::string evdi_library_version;


  enum class VirtualDisplayBackend {
    NONE,
    EVDI,
    HERMES_KMS,
    HYPRLAND_HEADLESS,
  };

  /**
   * @brief Which mechanism creates the virtual display for this session.
   *
   * The configured backend names a *DRM device* - EVDI or Hermes-KMS - and both
   * rest on the same assumption: the compositor renders on the real GPU and
   * imports the buffer into a display-only device it does not render on. That
   * is how KWin and Mutter work and it is not how aquamarine works, so on
   * Hyprland neither device backend produces a picture, whichever one is
   * configured. Hyprland renders its own headless output on the primary GPU
   * instead, which sidesteps the question entirely, so the compositor - not the
   * config option - decides here. The option still selects between EVDI and
   * Hermes-KMS everywhere else.
   */
  static VirtualDisplayBackend selected_backend() {
    // Asking for no backend is a decision, not an absence of one, and it comes
    // before the compositor is consulted: a host that streams an output it
    // already has - the container image's own headless sway session is the
    // standing example - wants none of this machinery, and Hyprland is no
    // exception to that.
    if (config::video.virtual_display_backend == "none") {
      return VirtualDisplayBackend::NONE;
    }
    if (sessionCompositor() == compositor_e::hyprland) {
      return VirtualDisplayBackend::HYPRLAND_HEADLESS;
    }
    return config::video.virtual_display_backend == "hermes_kms" ? VirtualDisplayBackend::HERMES_KMS : VirtualDisplayBackend::EVDI;
  }

  static const char *backend_name(VirtualDisplayBackend backend) {
    switch (backend) {
      case VirtualDisplayBackend::HERMES_KMS:
        return "Hermes-KMS";
      case VirtualDisplayBackend::HYPRLAND_HEADLESS:
        return "Hyprland headless output";
      case VirtualDisplayBackend::NONE:
        return "no";
      case VirtualDisplayBackend::EVDI:
        break;
    }
    return "EVDI";
  }

  // Defined with the KScreen backend below; the Hyprland one interpolates
  // output names into its socket commands too.
  static bool safe_output_name(const std::string &name);

  /**
   * @brief Hyprland's control socket.
   *
   * Hyprland creates and destroys outputs over its own IPC, not over any
   * Wayland protocol, so this is the one part of the headless path that cannot
   * be done with the wlr clients Hermes already has. The socket is used rather
   * than the `hyprctl` binary: hyprctl ships in a separate package a user can
   * be without, and shelling out would run against whatever PATH the service
   * inherited.
   */
  namespace hyprland {
    static std::string socket_path() {
      const char *signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
      if (!signature || !signature[0]) {
        return {};
      }
      const char *runtime_dir = std::getenv("XDG_RUNTIME_DIR");
      std::string base = runtime_dir && runtime_dir[0] ? runtime_dir : "/run/user/" + std::to_string(getuid());
      return base + "/hypr/" + signature + "/.socket.sock";
    }

    static bool available() {
      const auto path = socket_path();
      return !path.empty() && std::filesystem::exists(path);
    }

    /** Send one command and return the whole reply, or nullopt if the socket refused. */
    static std::optional<std::string> request(const std::string &command) {
      const auto path = socket_path();
      if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) {
        return std::nullopt;
      }

      const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        return std::nullopt;
      }
      auto close_fd = util::fail_guard([fd]() {
        close(fd);
      });

      sockaddr_un addr {};
      addr.sun_family = AF_UNIX;
      std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
      if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        return std::nullopt;
      }

      size_t written = 0;
      while (written < command.size()) {
        const ssize_t n = write(fd, command.data() + written, command.size() - written);
        if (n <= 0) {
          if (n < 0 && errno == EINTR) {
            continue;
          }
          return std::nullopt;
        }
        written += static_cast<size_t>(n);
      }

      std::string reply;
      char buffer[4096];
      while (true) {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0) {
          if (errno == EINTR) {
            continue;
          }
          return std::nullopt;
        }
        if (n == 0) {
          break;
        }
        reply.append(buffer, static_cast<size_t>(n));
      }
      return reply;
    }

    /** Names of every output Hyprland currently drives. */
    static std::vector<std::string> monitor_names() {
      std::vector<std::string> names;
      const auto reply = request("j/monitors");
      if (!reply) {
        return names;
      }
      try {
        const auto monitors = nlohmann::json::parse(*reply);
        for (const auto &monitor : monitors) {
          if (const auto name = monitor.find("name"); name != monitor.end() && name->is_string()) {
            names.emplace_back(name->get<std::string>());
          }
        }
      } catch (const nlohmann::json::exception &e) {
        BOOST_LOG(warning) << "[VDISPLAY] Could not read Hyprland's monitor list: " << e.what();
      }
      return names;
    }

    /**
     * @brief Create a headless output and return the name Hyprland gave it.
     *
     * The reply to `output create` is just "ok" - it does not name what was
     * created - and the HEADLESS counter does not reset when an output is
     * removed, so the second output of a session is HEADLESS-2 even when it is
     * the only one alive. Diffing the monitor list is therefore the only way to
     * learn the name; guessing it produces a name that belongs to nothing.
     */
    static std::optional<std::string> create_headless() {
      std::set<std::string> before;
      for (auto &name : monitor_names()) {
        before.insert(std::move(name));
      }

      const auto reply = request("/output create headless");
      if (!reply || reply->rfind("ok", 0) != 0) {
        BOOST_LOG(error) << "[VDISPLAY] Hyprland refused to create a headless output"
                         << (reply ? ": " + *reply : " (no reply from its socket).");
        return std::nullopt;
      }

      // The output appears on Hyprland's own event loop, not on the reply.
      for (int attempt = 0; attempt < 40; ++attempt) {
        for (const auto &name : monitor_names()) {
          if (!before.contains(name)) {
            return name;
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }

      BOOST_LOG(error) << "[VDISPLAY] Hyprland accepted the headless output but never published it.";
      return std::nullopt;
    }

    /**
     * @brief Pin the mode and scale of a headless output with a monitor rule,
     * so they survive Hyprland re-applying its rules.
     *
     * Hyprland applies wlr-output-management state on top of the monitor rule
     * that matches the output, but that state lives in the requesting
     * client's manager object and is erased when the client disconnects.
     * configure_virtual_output() destroys its connection on return, so the
     * next rule reload - a physical display powering off is enough, and so is
     * a config reload - finds nothing of what Hermes applied and puts the
     * headless output back on the catch-all rule: on a scaled desktop that
     * means a different mode, a different scale and an auto position, while
     * the stream keeps encoding the size it negotiated. A rule keyed by the
     * output's name is what `hyprctl keyword monitor` adds by hand, and it is
     * what the reload finds. Rules replace by name and HEADLESS-N names are
     * not reused, so one stale rule per session name remains until the next
     * config reload; it matches nothing once its output is gone.
     *
     * The rule goes through `eval hl.monitor({...})` first, the only form
     * Hyprland main still registers, and through `keyword monitor` when eval
     * is refused, for a release that still parses the legacy config.
     *
     * The position stays `auto`. Adding the rule makes Hyprland lay out every
     * monitor again, and a physical monitor on an `auto` rule is then placed
     * after any explicitly positioned one - pinning the position Hermes just
     * applied would shove the physical monitor to the right of the virtual
     * one. With `auto` on both, the virtual output lands after the physical
     * ones, which is where the wlr-output-management step put it anyway. A
     * future `mirror` layout on Hyprland belongs in this rule too, through
     * Hyprland's `mirror` option, not in overlapping positions.
     */
    static void pin_output(const std::string &name, int width, int height, int refresh_mhz) {
      if (width <= 0 || height <= 0) {
        return;
      }
      // The name is interpolated into a Lua expression and a config line.
      if (!safe_output_name(name)) {
        BOOST_LOG(warning) << "[VDISPLAY] Refusing to pin a Hyprland monitor rule for an unsafe output name.";
        return;
      }
      std::string mode = std::to_string(width) + "x" + std::to_string(height);
      if (refresh_mhz > 0) {
        char refresh[32];
        std::snprintf(refresh, sizeof(refresh), "@%.3f", refresh_mhz / 1000.0);
        mode += refresh;
      }

      std::string rule = "hl.monitor({ output = \"" + name + "\", mode = \"" + mode + "\", position = \"auto\", scale = 1 })";
      auto reply = request("/eval " + rule);
      if (!reply || reply->rfind("ok", 0) != 0) {
        BOOST_LOG(info) << "[VDISPLAY] Hyprland did not take the monitor rule through eval"
                        << (reply ? " (" + *reply + ")" : " (no reply from its socket)") << "; trying keyword.";
        rule = name + "," + mode + ",auto,1";
        reply = request("/keyword monitor " + rule);
      }
      if (!reply || reply->rfind("ok", 0) != 0) {
        BOOST_LOG(warning) << "[VDISPLAY] Hyprland refused the monitor rule '" << rule << "'"
                           << (reply ? ": " + *reply : " (no reply from its socket).");
        return;
      }
      BOOST_LOG(info) << "[VDISPLAY] Pinned Hyprland monitor rule '" << rule << "'";
    }

    static bool remove_output(const std::string &name) {
      if (name.empty()) {
        return false;
      }
      const auto reply = request("/output remove " + name);
      if (!reply || reply->rfind("ok", 0) != 0) {
        BOOST_LOG(warning) << "[VDISPLAY] Hyprland refused to remove output " << name
                           << (reply ? ": " + *reply : " (no reply from its socket).");
        return false;
      }
      return true;
    }
    /**
     * @brief Say once, and in one place, that Hyprland's control socket is out of reach.
     *
     * Two callers check availability - the readiness path and the create path -
     * so a session whose socket cannot be reached reported the same three
     * sentences twice. The wording is the part worth keeping identical: it is
     * the one message that names the actual cause, which is almost always a
     * service started outside the session rather than a broken compositor.
     */
    static void log_unreachable(bool fatal) {
      BOOST_LOG(fatal ? error : warning)
        << "[VDISPLAY] The session is Hyprland but its control socket is not reachable. "
           "Hermes reads HYPRLAND_INSTANCE_SIGNATURE from its own environment, so a "
           "service started outside the session will not find it.";
    }

  }  // namespace hyprland

  namespace hermes_kms {
    constexpr uint32_t multi_output_uapi_version = 8;
    constexpr uint32_t multi_device_uapi_version = 9;
    constexpr uint32_t session_device_uapi_version = 10;
    constexpr uint32_t session_access_uapi_version = 11;
    constexpr uint32_t dynamic_devices_uapi_version = 12;
    constexpr uint32_t session_lifecycle_uapi_version = 13;
    constexpr size_t name_len = 32;

    constexpr uint64_t cap_virtual_output = 1ULL << 0;
    constexpr uint64_t cap_output_control = 1ULL << 1;
    constexpr uint64_t cap_frame_acquire = 1ULL << 5;
    constexpr uint64_t cap_dmabuf_export = 1ULL << 6;
    constexpr uint64_t cap_output_identity = 1ULL << 7;
    constexpr uint64_t cap_session_owner = 1ULL << 8;
    constexpr uint64_t cap_frame_wait = 1ULL << 9;
    constexpr uint64_t cap_metrics = 1ULL << 10;
    constexpr uint64_t cap_multi_output = 1ULL << 11;
    constexpr uint64_t cap_multi_device = 1ULL << 12;
    constexpr uint64_t cap_session_token = 1ULL << 14;
    constexpr uint64_t cap_cursor_capture = 1ULL << 15;
    // Cards come and go at runtime through the driver's configfs group, so
    // device_index is neither dense nor stable; a card is identified by its
    // role and session index instead.
    constexpr uint64_t cap_dynamic_devices = 1ULL << 16;
    // SESSION_ACCESS takes ROTATE_TOKEN and REVOKE_BINDINGS, GET_STATUS reports
    // bound_fd_count, and GET_METRICS reports the binding counters.
    constexpr uint64_t cap_session_lifecycle = 1ULL << 17;
    constexpr uint64_t cap_zero_copy_target = 1ULL << 33;
    constexpr uint64_t cap_sync_file = 1ULL << 35;

    constexpr uint64_t status_output_enabled = 1ULL << 0;
    constexpr uint64_t status_connected = 1ULL << 1;

    constexpr uint32_t set_output_connected = 1U << 0;
    constexpr uint32_t set_output_owner_assigned = 1U << 1;
    constexpr uint32_t device_role_session = 2U;

    struct version_t {
      uint32_t uapi_version;
      uint32_t driver_major;
      uint32_t driver_minor;
      uint32_t driver_patch;
      char driver_name[name_len];
    };

    struct caps_t {
      uint64_t flags;
      uint32_t min_width;
      uint32_t min_height;
      uint32_t max_width;
      uint32_t max_height;
      uint32_t preferred_width;
      uint32_t preferred_height;
      uint32_t max_refresh_hz;
      uint32_t output_count;
    };

    struct status_t {
      uint64_t flags;
      uint64_t frame_sequence;
      uint64_t last_update_ns;
      uint64_t last_enable_ns;
      uint64_t last_disable_ns;
      uint32_t connector_id;
      uint32_t crtc_id;
      uint32_t plane_id;
      uint32_t encoder_id;
      uint32_t requested_width;
      uint32_t requested_height;
      uint32_t requested_refresh_hz;
      uint32_t active_width;
      uint32_t active_height;
      uint32_t active_refresh_hz;
      uint32_t framebuffer_id;
      uint32_t framebuffer_width;
      uint32_t framebuffer_height;
      uint32_t framebuffer_format;
      uint32_t framebuffer_plane_count;
      uint32_t framebuffer_pitch[4];
      uint32_t framebuffer_offset[4];
      uint32_t reserved_alignment;
      uint64_t framebuffer_modifier;
      uint64_t session_id;
      int32_t owner_pid;
      uint32_t reserved0;
      /// Descriptors bound to this output's live session besides the owner's
      /// own (uapi >= 13); reset to zero by a revocation or a new session.
      uint64_t bound_fd_count;
      uint64_t reserved[5];
    };

    struct identity_t {
      char driver_name[name_len];
      char output_name[name_len];
      char connector_name[name_len];
      uint32_t connector_id;
      uint32_t crtc_id;
      uint32_t plane_id;
      uint32_t encoder_id;
      uint32_t output_index;
      uint32_t output_count;
      uint32_t device_index;
      uint32_t device_count;
      uint32_t device_role;
      uint32_t session_index;
      uint32_t session_device_count;
      uint32_t cursor_plane_id;
    };

    struct select_output_t {
      uint32_t output_index;
      uint32_t flags;
      uint32_t selected_output_index;
      uint32_t output_count;
      char output_name[name_len];
      uint32_t reserved[8];
    };

    struct set_output_t {
      uint32_t enabled;
      uint32_t width;
      uint32_t height;
      uint32_t refresh_hz;
      uint32_t flags;
      uint32_t result_flags;
      uint64_t session_id;
    };

    struct acquire_frame_t {
      uint64_t flags;
      uint64_t sequence;
      uint64_t timestamp_ns;
      uint64_t modifier;
      uint32_t framebuffer_id;
      uint32_t width;
      uint32_t height;
      uint32_t format;
      uint32_t plane_count;
      uint32_t pitch[4];
      uint32_t offset[4];
      int32_t dma_buf_fd[4];
      int32_t sync_file_fd;
      uint32_t reserved0;
      uint32_t damage_x1;
      uint32_t damage_y1;
      uint32_t damage_x2;
      uint32_t damage_y2;
      uint64_t reserved[6];
    };

    struct wait_frame_t {
      uint64_t flags;
      uint64_t after_sequence;
      uint64_t sequence;
      uint64_t timestamp_ns;
      uint64_t status_flags;
      uint32_t timeout_ms;
      uint32_t reserved0;
      uint64_t reserved[6];
    };

    struct wait_update_t {
      uint64_t flags;
      uint64_t after_frame_sequence;
      uint64_t after_cursor_sequence;
      uint64_t frame_sequence;
      uint64_t cursor_sequence;
      uint64_t frame_timestamp_ns;
      uint64_t cursor_timestamp_ns;
      uint64_t status_flags;
      uint32_t timeout_ms;
      uint32_t reserved0;
      uint64_t reserved[5];
    };

    struct acquire_cursor_t {
      uint64_t flags;
      uint64_t sequence;
      uint64_t image_sequence;
      uint64_t timestamp_ns;
      uint64_t modifier;
      uint64_t session_id;
      int32_t position_x;
      int32_t position_y;
      int32_t crtc_x;
      int32_t crtc_y;
      uint32_t crtc_w;
      uint32_t crtc_h;
      uint32_t src_x;
      uint32_t src_y;
      uint32_t src_w;
      uint32_t src_h;
      int32_t hotspot_x;
      int32_t hotspot_y;
      uint32_t framebuffer_id;
      uint32_t width;
      uint32_t height;
      uint32_t format;
      uint32_t plane_count;
      uint32_t pitch[4];
      uint32_t offset[4];
      int32_t dma_buf_fd[4];
      int32_t sync_file_fd;
      uint32_t reserved0;
      uint32_t reserved_alignment;
      uint64_t reserved[6];
    };

    static_assert(sizeof(caps_t) == 40);
    static_assert(sizeof(identity_t) == 144);
    static_assert(sizeof(select_output_t) == 80);
    static_assert(sizeof(acquire_frame_t) == 176);
    static_assert(sizeof(wait_frame_t) == 96);
    static_assert(sizeof(wait_update_t) == 112);
    static_assert(sizeof(acquire_cursor_t) == 224);
    static_assert(offsetof(identity_t, cursor_plane_id) == 140);
    static_assert(offsetof(wait_update_t, timeout_ms) == 64);
    static_assert(offsetof(acquire_cursor_t, dma_buf_fd) == 148);
    static_assert(offsetof(acquire_cursor_t, reserved) == 176);

    // Mirrors `struct drm_hermes_kms_metrics` in the driver UAPI header
    // (include/uapi/drm/hermes_kms_drm.h). Field order and size must match
    // exactly. Read-only counters maintained by the driver for the lifetime of
    // the device; they are not per-session.
    struct metrics_t {
      uint64_t frame_sequence;
      uint64_t frame_update_count;
      uint64_t acquire_count;
      uint64_t acquire_no_frame_count;
      uint64_t dmabuf_export_count;
      uint64_t dmabuf_export_fail_count;
      uint64_t sync_file_export_count;
      uint64_t sync_file_export_fail_count;
      uint64_t wait_count;
      uint64_t wait_ready_count;
      uint64_t wait_timeout_count;
      uint64_t wait_interrupted_count;
      uint64_t output_enable_count;
      uint64_t output_disable_count;
      uint64_t hotplug_event_count;
      uint64_t owner_close_disconnect_count;
      uint64_t last_update_ns;
      uint64_t last_acquire_ns;
      uint64_t last_wait_start_ns;
      uint64_t last_wait_end_ns;
      uint64_t last_wait_duration_ns;
      uint64_t last_dmabuf_export_ns;
      uint64_t last_sync_file_export_ns;
      uint64_t vblank_count;
      uint64_t vblank_overrun_count;
      // Session-capability lifecycle (uapi >= 13), taken from reserved slots,
      // so the structure keeps its size.
      uint64_t bind_count;
      uint64_t bind_reject_count;
      uint64_t unbind_count;
      uint64_t binding_revoke_count;
      uint64_t cross_session_buffer_export_count;
      uint64_t reserved[9];
    };

    struct session_access_t {
      uint64_t token[2];
      uint64_t session_id;
      uint32_t operation;
      uint32_t output_index;
      uint32_t flags;
      uint32_t result_flags;
      uint64_t reserved[4];
    };

    // Frame request/result flags (must match the UAPI header).
    constexpr uint64_t frame_request_dmabuf = 1ULL << 0;
    constexpr uint64_t frame_dmabuf_valid = 1ULL << 2;
    constexpr uint64_t frame_sync_file_valid = 1ULL << 3;
    constexpr uint64_t frame_request_sync_file = 1ULL << 5;
    constexpr uint64_t wait_frame_ready = 1ULL << 0;
    constexpr uint64_t wait_update_frame_ready = 1ULL << 0;
    constexpr uint64_t wait_update_cursor_ready = 1ULL << 1;
    constexpr uint64_t cursor_request_dmabuf = 1ULL << 0;
    constexpr uint64_t cursor_request_sync_file = 1ULL << 1;
    constexpr uint64_t cursor_metadata_valid = 1ULL << 2;
    constexpr uint64_t cursor_visible = 1ULL << 3;
    constexpr uint64_t cursor_position_valid = 1ULL << 4;
    constexpr uint64_t cursor_buffer_valid = 1ULL << 6;
    constexpr uint64_t cursor_dmabuf_valid = 1ULL << 7;
    constexpr uint64_t cursor_sync_file_valid = 1ULL << 8;
    constexpr uint64_t cursor_geometry_valid = 1ULL << 9;

    constexpr uint32_t session_access_get_token = 1U;
    constexpr uint32_t session_access_bind = 2U;
    // ROTATE_TOKEN is part of the uapi but unused here: the capture worker
    // re-binds from the stored token whenever it reinitialises, so rotating
    // behind its back would fail a bind that is already under way.
    constexpr uint32_t session_access_rotate_token [[maybe_unused]] = 4U;
    constexpr uint32_t session_access_revoke_bindings = 5U;
    constexpr uint32_t session_access_result_bound = 1U << 0;
    constexpr uint32_t session_access_result_token_valid = 1U << 1;
    constexpr uint32_t session_access_result_revoked = 1U << 2;

    constexpr unsigned long ioctl_get_version = DRM_IOR(DRM_COMMAND_BASE + 0x00, version_t);
    constexpr unsigned long ioctl_get_caps = DRM_IOR(DRM_COMMAND_BASE + 0x01, caps_t);
    constexpr unsigned long ioctl_get_status = DRM_IOR(DRM_COMMAND_BASE + 0x02, status_t);
    constexpr unsigned long ioctl_set_output = DRM_IOWR(DRM_COMMAND_BASE + 0x03, set_output_t);
    constexpr unsigned long ioctl_acquire_frame = DRM_IOWR(DRM_COMMAND_BASE + 0x04, acquire_frame_t);
    constexpr unsigned long ioctl_get_identity = DRM_IOR(DRM_COMMAND_BASE + 0x05, identity_t);
    constexpr unsigned long ioctl_wait_frame = DRM_IOWR(DRM_COMMAND_BASE + 0x06, wait_frame_t);
    constexpr unsigned long ioctl_get_metrics = DRM_IOR(DRM_COMMAND_BASE + 0x07, metrics_t);
    constexpr unsigned long ioctl_select_output = DRM_IOWR(DRM_COMMAND_BASE + 0x08, select_output_t);
    constexpr unsigned long ioctl_session_access = DRM_IOWR(DRM_COMMAND_BASE + 0x09, session_access_t);
    constexpr unsigned long ioctl_acquire_cursor = DRM_IOWR(DRM_COMMAND_BASE + 0x0a, acquire_cursor_t);
    constexpr unsigned long ioctl_wait_update = DRM_IOWR(DRM_COMMAND_BASE + 0x0b, wait_update_t);

    static_assert(sizeof(status_t) == 208);
    static_assert(offsetof(status_t, framebuffer_modifier) == 136);
    static_assert(offsetof(status_t, bound_fd_count) == 160);
    static_assert(sizeof(metrics_t) == 312);
    static_assert(offsetof(metrics_t, bind_count) == 200);
    static_assert(offsetof(metrics_t, cross_session_buffer_export_count) == 232);
    static_assert(sizeof(session_access_t) == 72);

    struct device_t {
      int fd {-1};
      int card_index {-1};
      uint32_t selected_output_index {0};
      version_t version {};
      caps_t caps {};
      identity_t identity {};
    };

    static std::string cstr(const char *value) {
      return value && value[0] ? std::string {value, strnlen(value, name_len)} : std::string {};
    }

    static void forget_secret(void *data, size_t size) {
      auto *byte = static_cast<volatile unsigned char *>(data);
      while (size--) {
        *byte++ = 0;
      }
    }

    static int numbered_node_index(const fs::path &path, std::string_view prefix) {
      const auto name = path.filename().string();
      if (!name.starts_with(prefix) || name.size() == prefix.size()) {
        return -1;
      }

      int index = 0;
      for (const unsigned char ch : std::string_view {name}.substr(prefix.size())) {
        const int digit = ch - static_cast<unsigned char>('0');
        if (!std::isdigit(ch) || index > (std::numeric_limits<int>::max() - digit) / 10) {
          return -1;
        }
        index = index * 10 + digit;
      }
      return index;
    }

    static int card_index_from_path(const fs::path &path) {
      return numbered_node_index(path, "card");
    }

    static bool is_render_node(const fs::path &path) {
      return numbered_node_index(path, "renderD") >= 0;
    }

    static bool is_hermes_device(int fd) {
      if (fd < 0) {
        return false;
      }

      drmVersionPtr version = drmGetVersion(fd);
      if (!version) {
        return false;
      }
      const bool matches = version->name && version->name_len > 0 &&
                           std::string_view {
                             version->name,
                             static_cast<size_t>(version->name_len),
                           } == "hermes-kms";
      drmFreeVersion(version);
      return matches;
    }

    /**
     * Resolve the primary card index behind an open DRM node.
     *
     * This reads sysfs rather than asking libdrm on purpose. drmGetDevice2()
     * tells two platform devices apart by their MODALIAS, and every card this
     * driver registers reports the same "platform:hermes-kms"; libdrm folds the
     * whole pool into a single device and then answers -ENODEV for every node
     * but the first one it happened to enumerate. That makes the session cards
     * invisible, which in turn makes isolated sessions and broker-created cards
     * unreachable. The character device's own sysfs directory names exactly one
     * card and has no such ambiguity.
     */
    static int primary_card_index(int fd) {
      if (fd < 0) {
        return -1;
      }

      struct stat node {};
      if (::fstat(fd, &node) != 0 || !S_ISCHR(node.st_mode)) {
        return -1;
      }

      char drm_dir[64];
      std::snprintf(
        drm_dir,
        sizeof(drm_dir),
        "/sys/dev/char/%u:%u/device/drm",
        static_cast<unsigned>(major(node.st_rdev)),
        static_cast<unsigned>(minor(node.st_rdev))
      );

      // The directory also holds the render node and the connector entries;
      // card_index_from_path() accepts only a bare "card<N>".
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator {drm_dir, ec}) {
        const int index = card_index_from_path(entry.path());
        if (index >= 0) {
          return index;
        }
      }
      return -1;
    }

    static void close_device(device_t &device) {
      if (device.fd >= 0) {
        ::close(device.fd);
        device.fd = -1;
      }
    }

    static int open_render_node_for_card_index(int card_index) {
      if (card_index < 0) {
        return -1;
      }

      std::error_code ec;
      fs::directory_iterator dir("/dev/dri", ec);
      if (ec) {
        return -1;
      }
      for (const auto &entry : dir) {
        if (!is_render_node(entry.path())) {
          continue;
        }
        const int fd = ::open(entry.path().c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
          continue;
        }
        if (is_hermes_device(fd) && primary_card_index(fd) == card_index) {
          return fd;
        }
        ::close(fd);
      }
      return -1;
    }

    static bool multi_output_requested() {
      return config::video.hermes_kms_multi_output &&
             !config::video.hermes_kms_isolated_sessions;
    }

    static bool isolated_sessions_requested() {
      return config::video.hermes_kms_isolated_sessions;
    }

    static uint32_t required_uapi_version() {
      // Secure capture always uses the generic session-capability handoff.
      return session_access_uapi_version;
    }

    static uint32_t output_count(const caps_t &caps) {
      return caps.output_count ? caps.output_count : 1U;
    }

    static bool has_required_caps(
      uint64_t flags,
      bool require_multi_output = multi_output_requested(),
      bool require_multi_device = isolated_sessions_requested()
    ) {
      constexpr uint64_t required = cap_virtual_output | cap_output_control | cap_frame_acquire |
                                    cap_dmabuf_export | cap_output_identity | cap_session_owner |
                                    cap_frame_wait | cap_session_token | cap_cursor_capture |
                                    cap_zero_copy_target | cap_sync_file;
      return (flags & required) == required &&
             (!require_multi_output || (flags & cap_multi_output)) &&
             (!require_multi_device || (flags & cap_multi_device));
    }

    static std::vector<device_t> open_devices(bool log_failures) {
      std::vector<device_t> devices;
      try {
        for (const auto &entry : fs::directory_iterator("/dev/dri")) {
          // All Hermes control/capture ioctls are DRM_RENDER_ALLOW. The broker
          // deliberately does not need access to private-seat primary cards;
          // those are opened later by the compositor through seatd.
          if (!is_render_node(entry.path())) {
            continue;
          }

          device_t candidate {};
          candidate.fd = ::open(entry.path().c_str(), O_RDWR | O_CLOEXEC);
          if (candidate.fd < 0) {
            continue;
          }

          // Identify the driver through the DRM core before issuing any
          // driver-private ioctl. Private command numbers are not globally
          // unique across unrelated DRM drivers.
          if (!is_hermes_device(candidate.fd)) {
            close_device(candidate);
            continue;
          }

          if (::ioctl(candidate.fd, ioctl_get_version, &candidate.version) != 0) {
            close_device(candidate);
            continue;
          }

          if (cstr(candidate.version.driver_name) != "hermes-kms") {
            close_device(candidate);
            continue;
          }

          candidate.card_index = primary_card_index(candidate.fd);
          if (candidate.card_index < 0) {
            if (log_failures) {
              BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Could not resolve the primary card for "
                               << entry.path();
            }
            close_device(candidate);
            continue;
          }
          const auto required_version = required_uapi_version();
          if (candidate.version.uapi_version < required_version) {
            if (log_failures) {
              BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] UAPI " << candidate.version.uapi_version
                               << " is too old; need " << required_version
                               << (multi_output_requested() ? " for experimental multi-output." : ".");
            }
            close_device(candidate);
            continue;
          }

          if (::ioctl(candidate.fd, ioctl_get_caps, &candidate.caps) != 0 || !has_required_caps(candidate.caps.flags)) {
            if (log_failures) {
              BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Driver is present but does not expose the required streaming capabilities.";
            }
            close_device(candidate);
            continue;
          }

          if (::ioctl(candidate.fd, ioctl_get_identity, &candidate.identity) != 0) {
            if (log_failures) {
              BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] GET_IDENTITY failed for "
                               << entry.path() << ": " << std::strerror(errno);
            }
            close_device(candidate);
            continue;
          }

          devices.emplace_back(candidate);
        }
      } catch (const std::exception &exception) {
        if (log_failures) {
          BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Could not enumerate DRM devices: " << exception.what();
        }
      }

      // A driver that creates and removes cards at runtime hands out
      // device_index values that are neither dense nor stable across a card
      // being recreated, so ordering by it would reshuffle the pool between two
      // enumerations. Session cards are then ordered by the session index their
      // seat and private broker are named after, which is stable by
      // construction, and the host card (session_index 0) still comes first.
      const bool dynamic_devices = std::any_of(devices.begin(), devices.end(), [](const auto &device) {
        return (device.caps.flags & cap_dynamic_devices) != 0;
      });
      std::sort(devices.begin(), devices.end(), [dynamic_devices](const auto &left, const auto &right) {
        if (dynamic_devices) {
          if (left.identity.session_index != right.identity.session_index) {
            return left.identity.session_index < right.identity.session_index;
          }
        } else if (left.version.uapi_version >= multi_device_uapi_version &&
                   right.version.uapi_version >= multi_device_uapi_version &&
                   left.identity.device_index != right.identity.device_index) {
          return left.identity.device_index < right.identity.device_index;
        }
        return left.card_index < right.card_index;
      });

      if (devices.empty() && log_failures) {
        BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] No accessible Hermes-KMS render node found. "
                            "Load hermes_kms and run hermes-kms-setup for this user.";
      }
      return devices;
    }

    static bool open_device(device_t &out, bool log_failures) {
      auto devices = open_devices(log_failures);
      if (devices.empty()) {
        return false;
      }

      out = devices.front();
      devices.front().fd = -1;
      for (auto &device : devices) {
        close_device(device);
      }
      return true;
    }

    static bool available() {
      device_t device {};
      const bool ok = open_device(device, false);
      close_device(device);
      return ok;
    }

    // Reason a hermes-kms card node could not be used, distinguished so the UI
    // can offer targeted guidance. `open_device` collapses these into a bool;
    // this walks the same probe but reports why it stopped.
    enum class probe_result {
      ok,
      no_device,  ///< No card node identifies itself as the hermes-kms driver.
      uapi_too_old,
      missing_caps,
    };

    struct probe_t {
      probe_result result {probe_result::no_device};
      int card_index {-1};
      uint32_t uapi_version {0};
      uint32_t output_count {0};
      uint32_t device_count {0};
      uint32_t session_device_count {0};
      bool multi_output_capable {false};
      bool multi_device_capable {false};
      /// Cards can be created and removed at runtime, so `device_count` is a
      /// live count rather than the pool size fixed at module load.
      bool dynamic_devices {false};
      /// An owner can revoke a session's bindings without ending the stream.
      bool session_lifecycle {false};
      std::string driver_version;
    };

    static probe_t probe() {
      probe_t out;
      std::error_code ec;
      fs::directory_iterator dir("/dev/dri", ec);
      if (ec) {
        return out;
      }
      for (const auto &entry : dir) {
        if (!is_render_node(entry.path())) {
          continue;
        }

        device_t candidate {};
        candidate.fd = ::open(entry.path().c_str(), O_RDWR | O_CLOEXEC);
        if (candidate.fd < 0) {
          continue;
        }

        if (!is_hermes_device(candidate.fd) ||
            ::ioctl(candidate.fd, ioctl_get_version, &candidate.version) != 0 ||
            cstr(candidate.version.driver_name) != "hermes-kms") {
          close_device(candidate);
          continue;
        }

        // Found the Hermes-KMS device; from here the outcome is definitive.
        out.card_index = primary_card_index(candidate.fd);
        out.uapi_version = candidate.version.uapi_version;
        out.driver_version = std::to_string(candidate.version.driver_major) + "." +
                             std::to_string(candidate.version.driver_minor) + "." +
                             std::to_string(candidate.version.driver_patch);

        if (candidate.version.uapi_version < required_uapi_version()) {
          out.result = probe_result::uapi_too_old;
        } else if (::ioctl(candidate.fd, ioctl_get_caps, &candidate.caps) != 0) {
          out.result = probe_result::missing_caps;
        } else if (::ioctl(candidate.fd, ioctl_get_identity, &candidate.identity) != 0) {
          out.result = probe_result::missing_caps;
        } else {
          out.output_count = output_count(candidate.caps);
          out.multi_output_capable =
            candidate.version.uapi_version >= multi_output_uapi_version &&
            (candidate.caps.flags & cap_multi_output);
          out.multi_device_capable =
            candidate.version.uapi_version >= multi_device_uapi_version &&
            (candidate.caps.flags & cap_multi_device);
          out.dynamic_devices =
            candidate.version.uapi_version >= dynamic_devices_uapi_version &&
            (candidate.caps.flags & cap_dynamic_devices);
          out.session_lifecycle =
            candidate.version.uapi_version >= session_lifecycle_uapi_version &&
            (candidate.caps.flags & cap_session_lifecycle);
          out.device_count =
            candidate.version.uapi_version >= multi_device_uapi_version &&
              candidate.identity.device_count ?
              candidate.identity.device_count :
              1U;
          out.session_device_count =
            candidate.version.uapi_version >= session_device_uapi_version &&
              candidate.identity.session_device_count ?
              candidate.identity.session_device_count :
              (out.multi_device_capable ? out.device_count : 0U);
          out.result = has_required_caps(candidate.caps.flags) ?
                         probe_result::ok :
                         probe_result::missing_caps;
        }
        close_device(candidate);
        return out;
      }
      return out;
    }

    static bool select_output(int fd, uint32_t output_index, select_output_t *result = nullptr, bool log_failure = true) {
      select_output_t request {};
      request.output_index = output_index;
      if (::ioctl(fd, ioctl_select_output, &request) != 0) {
        if (log_failure) {
          BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] SELECT_OUTPUT(" << output_index
                           << ") failed: " << std::strerror(errno);
        }
        return false;
      }
      if (result) {
        *result = request;
      }
      return true;
    }

    static bool refresh_identity(device_t &device) {
      std::memset(&device.identity, 0, sizeof(device.identity));
      return ::ioctl(device.fd, ioctl_get_identity, &device.identity) == 0;
    }

    static bool set_output(int fd, bool enabled, uint32_t width, uint32_t height, uint32_t refresh_hz,
                           uint64_t &session_id, bool log_failure = true) {
      set_output_t request {};
      request.enabled = enabled ? 1U : 0U;
      request.width = width;
      request.height = height;
      request.refresh_hz = refresh_hz;
      request.session_id = session_id;

      if (::ioctl(fd, ioctl_set_output, &request) != 0) {
        if (log_failure) {
          BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] SET_OUTPUT failed: " << std::strerror(errno);
        }
        return false;
      }

      session_id = request.session_id;
      if (enabled && (request.result_flags & (set_output_connected | set_output_owner_assigned)) !=
                       (set_output_connected | set_output_owner_assigned)) {
        BOOST_LOG(warning) << "[VDISPLAY/Hermes-KMS] Output enabled but result flags look incomplete: 0x"
                           << std::hex << request.result_flags << std::dec;
      }
      return true;
    }

    static bool get_status(int fd, status_t &status) {
      std::memset(&status, 0, sizeof(status));
      return ::ioctl(fd, ioctl_get_status, &status) == 0;
    }

    /**
     * GET_TOKEN, ROTATE_TOKEN and REVOKE_BINDINGS take the same shape: they are
     * owner-only, act on the output the fd already selected, and answer with the
     * token that is valid from now on. `required_result` names the extra flag the
     * operation must report beyond a valid token.
     */
    static bool owner_token_operation(int fd, uint32_t operation, uint32_t output_index,
                                      uint64_t session_id, std::array<uint64_t, 2> &token,
                                      uint32_t required_result, const char *description) {
      session_access_t request {};
      request.operation = operation;
      if (::ioctl(fd, ioctl_session_access, &request) != 0) {
        BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] " << description << ": " << std::strerror(errno);
        forget_secret(&request, sizeof(request));
        token.fill(0);
        return false;
      }

      token = {request.token[0], request.token[1]};
      const bool valid = (request.result_flags & session_access_result_token_valid) &&
                         (request.result_flags & required_result) == required_result &&
                         request.session_id == session_id && request.output_index == output_index &&
                         (token[0] || token[1]);
      forget_secret(&request, sizeof(request));
      if (!valid) {
        token.fill(0);
      }
      return valid;
    }

    static bool get_session_token(int fd, uint32_t output_index, uint64_t session_id,
                                  std::array<uint64_t, 2> &token) {
      return owner_token_operation(fd, session_access_get_token, output_index, session_id, token, 0,
                                   "Could not obtain the generic session capability");
    }

    /**
     * Invalidate every binding at once. Bound descriptors fail their next
     * protected ioctl with EACCES and blocked waits are woken to the same error,
     * while ownership, the session id and the scanout survive; the token is
     * rotated as part of the operation. Requires uapi >= 13.
     */
    static bool revoke_session_bindings(int fd, uint32_t output_index, uint64_t session_id,
                                        std::array<uint64_t, 2> &token) {
      return owner_token_operation(fd, session_access_revoke_bindings, output_index, session_id, token,
                                   session_access_result_revoked,
                                   "Could not revoke the session's bindings");
    }

    static bool bind_session(int fd, uint32_t output_index, uint64_t session_id,
                             const std::array<uint64_t, 2> &token) {
      session_access_t request {};
      request.token[0] = token[0];
      request.token[1] = token[1];
      request.session_id = session_id;
      request.operation = session_access_bind;
      request.output_index = output_index;
      if (::ioctl(fd, ioctl_session_access, &request) != 0) {
        BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Could not bind capture to the requested session: "
                         << std::strerror(errno);
        forget_secret(&request, sizeof(request));
        return false;
      }

      const bool bound = (request.result_flags & session_access_result_bound) &&
                         request.session_id == session_id && request.output_index == output_index;
      forget_secret(&request, sizeof(request));
      if (!bound) {
        BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Driver accepted SESSION_ACCESS without confirming the binding.";
      }
      return bound;
    }

    /**
     * Explain a mode the driver's envelope cannot contain, or an empty string
     * when it can. GET_CAPS reports compiled-in limits on older drivers and the
     * configured ones from uapi v13, where an administrator can raise the
     * maximum to 8K or lower it below what a client asks for - so a refused
     * SET_OUTPUT is worth spelling out rather than leaving as EINVAL.
     */
    static std::string mode_outside_envelope(const caps_t &caps, uint32_t width, uint32_t height,
                                             uint32_t refresh_hz) {
      const bool too_small = (caps.min_width && width < caps.min_width) ||
                             (caps.min_height && height < caps.min_height);
      const bool too_large = (caps.max_width && width > caps.max_width) ||
                             (caps.max_height && height > caps.max_height);
      const bool too_fast = caps.max_refresh_hz && refresh_hz > caps.max_refresh_hz;
      if (!too_small && !too_large && !too_fast) {
        return {};
      }

      std::string reason = " The driver accepts " + std::to_string(caps.min_width) + 'x' +
                           std::to_string(caps.min_height) + " to " + std::to_string(caps.max_width) +
                           'x' + std::to_string(caps.max_height) + " up to " +
                           std::to_string(caps.max_refresh_hz) + " Hz";
      reason += too_fast && !too_small && !too_large ?
                  ", so the refresh rate is out of range." :
                  ", so the requested mode is out of range.";
      return reason;
    }

    static bool claim_available_output(device_t &device, uint32_t width, uint32_t height,
                                       uint32_t refresh_hz, uint64_t &session_id,
                                       bool log_failure = true) {
      const bool select_required = multi_output_requested();
      const uint32_t count = select_required ? output_count(device.caps) : 1U;
      for (uint32_t index = 0; index < count; ++index) {
        if (select_required && !select_output(device.fd, index, nullptr, false)) {
          continue;
        }

        status_t status {};
        if (get_status(device.fd, status) &&
            (status.owner_pid > 0 || status.session_id != 0)) {
          continue;
        }

        uint64_t candidate_session = 0;
        bool enabled = false;
        for (unsigned int attempt = 0; attempt < 6; ++attempt) {
          if (set_output(device.fd, true, width, height, refresh_hz, candidate_session, false)) {
            enabled = true;
            break;
          }
          if (errno != EAGAIN || attempt == 5) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds {200});
        }
        if (!enabled) {
          // Another session may have won the ownership race after GET_STATUS.
          if (errno == EBUSY) {
            continue;
          }
          BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Could not enable output " << (index + 1)
                           << " at " << width << 'x' << height << '@' << refresh_hz << ": "
                           << std::strerror(errno)
                           << mode_outside_envelope(device.caps, width, height, refresh_hz);
          continue;
        }

        device.selected_output_index = index;
        refresh_identity(device);
        session_id = candidate_session;
        return true;
      }

      if (log_failure) {
        BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] No free virtual output is available (configured outputs="
                         << count << ").";
      }
      return false;
    }

    /**
     * Ask the broker for a card of its own and claim its output.
     *
     * This is the difference between a pool fixed at module load and a virtual
     * display per session: with no broker the answer to an exhausted pool is
     * "no device is free", and with one it is a new card on a private seat.
     * The wait afterwards is real work, not politeness - the card exists the
     * moment configfs says so, but its render node belongs to root until udev
     * runs 92-hermes-kms-access.rules over the card's access_uid. Re-enumerating
     * is that wait: open_devices() only reports nodes it could open.
     *
     * `card_owner_uid` is who the card must belong to: the session account
     * whose compositor will run on it, when there is one. A card made for the
     * host user instead is one an isolated session could never open - the
     * access_uid is what udev hands the device nodes to.
     */
    static bool claim_broker_card(device_t &out, uint32_t width, uint32_t height, uint32_t refresh_hz,
                                  uint64_t &session_id, std::string &broker_card,
                                  std::optional<uid_t> card_owner_uid) {
      const auto card = card_broker::create(card_owner_uid);
      if (!card) {
        return false;
      }

      const int card_index = card_index_from_path(card->card_path);
      constexpr unsigned int attempts = 30;  // ~3s, generous for a udev rule.
      for (unsigned int attempt = 0; attempt < attempts; ++attempt) {
        auto devices = open_devices(false);
        for (auto &device : devices) {
          if (device.card_index == card_index &&
              claim_available_output(device, width, height, refresh_hz, session_id, false)) {
            out = device;
            device.fd = -1;
            for (auto &remaining : devices) {
              close_device(remaining);
            }
            broker_card = card->name;
            return true;
          }
        }
        for (auto &device : devices) {
          close_device(device);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {100});
      }

      BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] The broker created " << card->name
                       << " but its render node never became usable; check that the driver's "
                          "92-hermes-kms-access.rules is installed.";
      card_broker::remove(card->name);
      return false;
    }

    static bool claim_available_device_output(
      device_t &out,
      uint32_t width,
      uint32_t height,
      uint32_t refresh_hz,
      uint64_t &session_id,
      std::string &broker_card,
      std::optional<uid_t> card_owner_uid
    ) {
      auto devices = open_devices(true);
      for (auto &device : devices) {
        // session_devices=N adds a seat0 HOST card. Per-client compositors may
        // claim only stable SESSION-role cards and use their session_index for
        // the matching private seat/broker.
        if (device.version.uapi_version < session_device_uapi_version ||
            device.identity.device_role != device_role_session ||
            device.identity.session_index == 0) {
          close_device(device);
          continue;
        }

        if (!claim_available_output(device, width, height, refresh_hz, session_id, false)) {
          close_device(device);
          continue;
        }

        out = device;
        device.fd = -1;
        for (auto &remaining : devices) {
          close_device(remaining);
        }
        return true;
      }

      if (card_broker::available()) {
        return claim_broker_card(out, width, height, refresh_hz, session_id, broker_card, card_owner_uid);
      }

      BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] No independent DRM device/output is free for a new "
                          "session, and no card broker is installed to create one.";
      return false;
    }

    /**
     * Older Hermes-KMS packages load the module with initial_enabled=1. That
     * exposes an unowned connector before Hermes starts and lets a compositor
     * replay a saved virtual-only layout at login. Disconnect that legacy
     * connector so virtual outputs remain tied to a live streaming session.
     */
    static bool disconnect_unowned_outputs(std::vector<std::string> &connector_names) {
      auto devices = open_devices(false);
      if (devices.empty()) {
        return false;
      }

      bool disconnected = false;
      for (auto &device : devices) {
        const uint32_t count = output_count(device.caps);
        for (uint32_t index = 0; index < count; ++index) {
          if (device.version.uapi_version >= multi_output_uapi_version &&
              !select_output(device.fd, index, nullptr, false)) {
            continue;
          }
          refresh_identity(device);
          const auto connector_name = cstr(device.identity.connector_name);

          status_t status {};
          if (get_status(device.fd, status) &&
              !(status.flags & status_output_enabled)) {
            continue;
          }

          // Active state is private under UAPI v11. Disabling is nevertheless
          // safe: the driver accepts this only for an unowned legacy output and
          // rejects a live foreign session.
          uint64_t session_id = 0;
          if (set_output(device.fd, false, 0, 0, 0, session_id, false)) {
            BOOST_LOG(warning) << "[VDISPLAY/Hermes-KMS] Disconnected unowned virtual output "
                               << cstr(device.identity.output_name) << " left enabled at module load.";
            if (!connector_name.empty()) {
              connector_names.emplace_back(connector_name);
            }
            disconnected = true;
          }
        }
        close_device(device);
      }
      return disconnected;
    }

    static bool get_metrics(int fd, metrics_t &metrics) {
      std::memset(&metrics, 0, sizeof(metrics));
      return ::ioctl(fd, ioctl_get_metrics, &metrics) == 0;
    }
  }  // namespace hermes_kms

  // Virtual display info structure
  struct VirtualDisplayInfo {
    std::string name;
    std::string guid_str;
    std::string client_name;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    int device_index;      // EVDI index or 0-based Hermes-KMS device index
    int session_index;     // 1-based Hermes-KMS private-seat index, or -1
    int output_index;      // 0-based Hermes-KMS output index
    int drm_card_index;    // DRM card index assigned by the kernel
    evdi_handle handle;    // EVDI handle
    int drm_fd;            // DRM fd for card
    bool active;
    bool using_evdi;       // true while an EVDI display is connected
    bool using_hermes_kms; // true while a Hermes-KMS output owner fd is held
    bool hermes_kms_session_lifecycle; // driver can revoke this session's bindings (uapi >= 13)
    std::string broker_card; // configfs card the broker made for this display, empty when pooled
    bool using_hyprland_headless; // true while Hyprland owns a headless output for this display
    std::string connector_name;
    uint64_t session_id;
    std::array<uint64_t, 2> session_token;
    std::shared_ptr<EvdiBuffer> evdi_buffer;
    int evdi_buffer_id;
    virtual_display_layout_e layout {virtual_display_layout_e::extend};
  };

  static std::map<std::string, VirtualDisplayInfo> virtual_displays;
  static std::atomic<bool> evdi_events_running {false};
  static std::thread evdi_events_thread;
  static std::string evdi_connector_name(int card_index);

  namespace {

    /** Case-insensitive comparison of one XDG_CURRENT_DESKTOP token. */
    bool desktop_token_matches(std::string_view token, std::string_view desktop) {
      if (token.size() < desktop.size()) {
        return false;
      }
      for (size_t i = 0; i < desktop.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(token[i])) != desktop[i]) {
          return false;
        }
      }
      // "GNOME" and "GNOME-Classic" are both Mutter; "GNOMEish" is not GNOME.
      return token.size() == desktop.size() || token[desktop.size()] == '-';
    }

  }  // namespace

  compositor_e compositorFromDesktopNames(const std::string &desktop_names) {
    // "wlroots" is a family name, not a compositor: Hyprland advertises it too,
    // and a desktop list is not ordered by specificity ("wlroots:sway" and
    // "Hyprland:wlroots" are both shipped). Naming a compositor therefore wins
    // over naming its family no matter which token came first, so the generic
    // match is held back and only used once every token has been read.
    bool wlroots_family = false;

    std::string_view remaining {desktop_names};
    while (!remaining.empty()) {
      const auto separator = remaining.find(':');
      const auto token = remaining.substr(0, separator);
      if (desktop_token_matches(token, "kde") || desktop_token_matches(token, "plasma")) {
        return compositor_e::kwin;
      }
      if (desktop_token_matches(token, "gnome")) {
        return compositor_e::mutter;
      }
      if (desktop_token_matches(token, "hyprland")) {
        return compositor_e::hyprland;
      }
      if (desktop_token_matches(token, "cosmic")) {
        return compositor_e::cosmic;
      }
      // These drive one strategy - plain wlr-output-management, no
      // per-compositor workaround - so they are one class, not one enumerator
      // each. A new wlroots compositor needs no code beyond this list.
      for (const auto *wlr : {"sway", "wayfire", "river", "labwc", "niri"}) {
        if (desktop_token_matches(token, wlr)) {
          return compositor_e::wlroots;
        }
      }
      if (desktop_token_matches(token, "wlroots")) {
        wlroots_family = true;
      }
      if (separator == std::string_view::npos) {
        break;
      }
      remaining.remove_prefix(separator + 1);
    }
    return wlroots_family ? compositor_e::wlroots : compositor_e::unknown;
  }

  compositor_e sessionCompositor() {
    for (const char *variable : {"XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP"}) {
      const char *value = std::getenv(variable);
      if (value && value[0]) {
        if (const auto compositor = compositorFromDesktopNames(value); compositor != compositor_e::unknown) {
          return compositor;
        }
      }
    }
    // A session started from a TTY - `exec Hyprland`, `exec sway` - carries no
    // desktop name at all, which is precisely the setup a user reaching for
    // virtual displays is likely to have. Each of these compositors exports its
    // control socket unconditionally, so the socket names the session when the
    // desktop variables do not.
    struct socket_hint {
      const char *variable;
      compositor_e compositor;
    };
    for (const auto &[variable, compositor] : {
           socket_hint {"HYPRLAND_INSTANCE_SIGNATURE", compositor_e::hyprland},
           socket_hint {"SWAYSOCK", compositor_e::wlroots},
           socket_hint {"WAYFIRE_SOCKET", compositor_e::wlroots},
         }) {
      if (const char *value = std::getenv(variable); value && value[0]) {
        return compositor;
      }
    }
    return compositor_e::unknown;
  }

  std::string compositorName(compositor_e compositor) {
    switch (compositor) {
      case compositor_e::kwin:
        return "KWin (KDE Plasma)";
      case compositor_e::mutter:
        return "Mutter (GNOME)";
      case compositor_e::hyprland:
        return "Hyprland";
      case compositor_e::wlroots:
        return "a wlroots compositor";
      case compositor_e::cosmic:
        return "COSMIC";
      case compositor_e::unknown:
        break;
    }
    return "an unrecognised compositor";
  }

  std::string featureName(feature_e feature) {
    switch (feature) {
      case feature_e::virtual_display:
        return "virtual display";
      case feature_e::client_requested_mode:
        return "client-requested mode";
      case feature_e::exclusive_mode:
        return "exclusive mode";
      case feature_e::multiple_displays:
        return "multiple virtual displays";
      case feature_e::isolated_sessions:
        return "isolated sessions";
      case feature_e::zero_copy_capture:
        return "zero-copy capture";
    }
    return "unknown feature";
  }

  std::string readinessName(readiness_e readiness) {
    switch (readiness) {
      case readiness_e::ready:
        return "ready";
      case readiness_e::degraded:
        return "degraded";
      case readiness_e::unavailable:
        return "unavailable";
      case readiness_e::unknown:
        return "unknown";
    }
    return "unknown";
  }

  namespace {

    /**
     * The seat a DRM card was assigned, as udev recorded it.
     *
     * Read from udev's own database rather than through libudev: the answer is
     * one property of one device, and linking a new library into every build to
     * read it would cost more than it explains. An absent file or an absent
     * property both mean the same thing to logind and to every compositor -
     * the device is on seat0 - so both return empty.
     */
    std::string drm_card_seat(const std::filesystem::path &card) {
      std::ifstream dev_file {card / "dev"};
      std::string devnum;
      if (!std::getline(dev_file, devnum) || devnum.empty()) {
        return {};
      }

      std::ifstream udev_db {std::filesystem::path {"/run/udev/data"} / ("c" + devnum)};
      if (!udev_db) {
        return {};
      }

      static constexpr std::string_view seat_prefix {"E:ID_SEAT="};
      for (std::string line; std::getline(udev_db, line);) {
        if (line.starts_with(seat_prefix)) {
          return line.substr(seat_prefix.size());
        }
      }
      return {};
    }

    /** Whether a DRM card node belongs to the hermes-kms driver. */
    bool is_hermes_kms_card(const std::filesystem::path &card) {
      std::ifstream uevent {card / "device" / "uevent"};
      for (std::string line; std::getline(uevent, line);) {
        if (line == "DRIVER=hermes-kms") {
          return true;
        }
      }
      return false;
    }

  }  // namespace

  std::vector<FeatureReport> assessSession(const SessionFacts &facts) {
    std::vector<FeatureReport> reports;
    const auto add = [&reports](feature_e feature, readiness_e readiness, std::string detail, std::string remediation = {}) {
      reports.push_back({feature, readiness, std::move(detail), std::move(remediation)});
    };

    // --- virtual display -----------------------------------------------------
    // Hyprland is judged on the compositor, not the backend: aquamarine's
    // trouble is with driving any display-only DRM device, so EVDI fails the
    // same way Hermes-KMS does and naming the backend would misdirect the user.
    readiness_e display_readiness = readiness_e::unavailable;
    if (!facts.wayland && !facts.x11) {
      // Not a verdict about the session: there isn't one yet. Saying "ready"
      // here - which treating this as X11 would - is worse than saying nothing.
      for (const auto feature : {
             feature_e::virtual_display,
             feature_e::client_requested_mode,
             feature_e::exclusive_mode,
             feature_e::multiple_displays,
             feature_e::isolated_sessions,
             feature_e::zero_copy_capture,
           }) {
        add(
          feature,
          readiness_e::unknown,
          "No window system is attached to this process, so nothing about the session can be observed.",
          "Run Hermes from inside the graphical session you intend to stream."
        );
      }
      return reports;
    }
    if (facts.x11) {
      display_readiness = readiness_e::ready;
      add(feature_e::virtual_display, display_readiness, "X11 session: the xrandr layout path applies.");
    } else if (facts.compositor == compositor_e::kwin) {
      display_readiness = facts.kscreen ? readiness_e::ready : readiness_e::unavailable;
      add(
        feature_e::virtual_display,
        display_readiness,
        facts.kscreen ? "KWin, configured through kscreen-doctor." :
                        "KWin is running but kscreen-doctor did not answer, so no layout can be applied.",
        facts.kscreen ? "" : "Install the kscreen package that provides kscreen-doctor."
      );
    } else if (facts.compositor == compositor_e::mutter) {
      display_readiness = facts.mutter ? readiness_e::ready : readiness_e::unavailable;
      add(
        feature_e::virtual_display,
        display_readiness,
        facts.mutter ? "GNOME, driven through org.gnome.Mutter.DisplayConfig." :
                       "GNOME is running but org.gnome.Mutter.DisplayConfig did not answer.",
        facts.mutter ? "" : "Check that gnome-shell is running and that Hermes may reach the session bus."
      );
    } else if (facts.compositor == compositor_e::hyprland) {
      // Hyprland renders a headless output itself, on the primary GPU. That is
      // a different mechanism from every other entry here - no virtual DRM
      // device, nothing imported - and it is the reason Hyprland is supported
      // at all: aquamarine cannot composite onto a display-only device, so the
      // device backends stay unavailable here no matter which one is
      // configured.
      display_readiness = facts.hyprland_control ? readiness_e::ready : readiness_e::unavailable;
      add(
        feature_e::virtual_display,
        display_readiness,
        facts.hyprland_control ?
          "Hyprland creates a headless output on request and renders it on the primary GPU, "
          "so no virtual DRM device is involved." :
          "The session is Hyprland, but its control socket is not reachable, so Hermes cannot "
          "ask it for a headless output. Hermes-KMS and EVDI are not an alternative here: "
          "aquamarine wants every GPU owning an output to host a GL renderer, which a "
          "display-only device cannot, and the builds that import directly stall on the "
          "page-flip handshake against the driver's software vblank.",
        facts.hyprland_control ?
          "" :
          "Hermes reads HYPRLAND_INSTANCE_SIGNATURE from its own environment; start it inside "
          "the Hyprland session, or import that variable into the service."
      );
    } else if (facts.output_management) {
      const bool verified = facts.compositor == compositor_e::wlroots || facts.compositor == compositor_e::cosmic;
      display_readiness = verified ? readiness_e::ready : readiness_e::degraded;
      add(
        feature_e::virtual_display,
        display_readiness,
        verified ? compositorName(facts.compositor) + ", driven through wlr-output-management." :
                   "This compositor is not one Hermes has verified, but it advertises "
                   "wlr-output-management, so the generic path applies."
      );
    } else {
      add(
        feature_e::virtual_display,
        readiness_e::unavailable,
        "The session advertises none of the protocols Hermes can drive an output with.",
        "A KDE, GNOME or wlroots session is required for automatic virtual-display activation."
      );
    }

    // --- client-requested mode ----------------------------------------------
    if (display_readiness == readiness_e::unavailable) {
      add(
        feature_e::client_requested_mode,
        readiness_e::unavailable,
        "No virtual display can be driven here, so no mode can be applied to one."
      );
    } else if (facts.compositor == compositor_e::unknown && facts.wayland) {
      // wlr-output-management carries set_custom_mode, but whether a compositor
      // honours it for an arbitrary geometry is a per-compositor answer.
      add(
        feature_e::client_requested_mode,
        readiness_e::unknown,
        "The mode is requested through wlr-output-management, which this compositor advertises. "
        "Whether it honours an arbitrary custom mode has not been verified here."
      );
    } else {
      add(feature_e::client_requested_mode, readiness_e::ready, "The mode the client asks for is pushed to the compositor and confirmed.");
    }

    // --- exclusive mode ------------------------------------------------------
    const bool exclusive_mechanism = facts.x11 ||
                                     (facts.compositor == compositor_e::kwin && facts.kscreen) ||
                                     facts.output_management;
    if (facts.compositor == compositor_e::mutter) {
      add(
        feature_e::exclusive_mode,
        facts.mutter ? readiness_e::ready : readiness_e::unavailable,
        facts.mutter ?
          "GNOME, through ApplyMonitorsConfig: a config naming only the virtual output disables the rest." :
          "GNOME is running but org.gnome.Mutter.DisplayConfig did not answer.",
        facts.mutter ? "" : "Check that gnome-shell is running and that Hermes may reach the session bus."
      );
    } else if (!exclusive_mechanism) {
      add(feature_e::exclusive_mode, readiness_e::unavailable, "No protocol in this session can disable a physical output.");
    } else if (display_readiness == readiness_e::unavailable) {
      // Worth saying rather than reporting "ready": the mechanism is present
      // and would work, but there is nothing here for it to blank the physical
      // monitors on behalf of.
      add(
        feature_e::exclusive_mode,
        readiness_e::degraded,
        "The compositor can disable a physical output, but this session has no virtual display "
        "to blank it for."
      );
    } else {
      add(feature_e::exclusive_mode, readiness_e::ready, "Physical outputs are disabled for the session and restored afterwards.");
    }

    // --- multiple displays ---------------------------------------------------
    // Every branch below counts outputs on a Hermes-KMS device, which is the
    // right question only when a DRM device is what backs a virtual display.
    // Hyprland creates headless outputs on demand and there is no fixed pool to
    // exhaust, so asking about hermes-kms here would report "unavailable" for a
    // session that supports this better than the device backends do.
    if (facts.compositor == compositor_e::hyprland) {
      add(
        feature_e::multiple_displays,
        facts.hyprland_control ? readiness_e::ready : readiness_e::unavailable,
        facts.hyprland_control ?
          "Hyprland creates a headless output per request; there is no fixed pool of outputs." :
          "Hyprland's control socket is not reachable, so no headless output can be created at all.",
        facts.hyprland_control ?
          "" :
          "Start Hermes inside the Hyprland session, or import HYPRLAND_INSTANCE_SIGNATURE into the service."
      );
    } else if (!facts.hermes_kms_present) {
      add(
        feature_e::multiple_displays,
        readiness_e::unavailable,
        "More than one virtual display needs the Hermes-KMS backend; no Hermes-KMS device was opened.",
        "Install and load the hermes-kms module."
      );
    } else if (facts.hermes_kms_multi_output || facts.hermes_kms_multi_device) {
      add(feature_e::multiple_displays, readiness_e::ready, "The Hermes-KMS device exposes more than one output.");
    } else {
      add(
        feature_e::multiple_displays,
        readiness_e::unavailable,
        "The Hermes-KMS device exposes a single output.",
        "Load hermes_kms with outputs=N (or session_devices=N) and enable the multi-output option."
      );
    }

    // --- isolated sessions ---------------------------------------------------
    if (!facts.hermes_kms_multi_device) {
      add(
        feature_e::isolated_sessions,
        readiness_e::unavailable,
        "The loaded hermes-kms driver exposes no session-device pool.",
        "Update hermes-kms to a version providing private session devices."
      );
    } else if (!facts.drm_seat_isolation) {
      // The failure this whole probe exists for: every other check passes, the
      // session starts, its input is isolated - and its screen is not, because
      // a card with no ID_SEAT is on seat0 where the host compositor claims it.
      add(
        feature_e::isolated_sessions,
        readiness_e::unavailable,
        "No Hermes-KMS card carries a private DRM seat, so the host compositor claims the session "
        "cards and a session would share the host's screen. Input isolation is installed "
        "separately and may already be working, which is what makes this failure quiet.",
        "Install the driver's session-seat rule with 'make install-udev' - it lands as "
        "72-hermes-kms-session-seats.rules, 70- in driver builds before it moved - then reload "
        "udev and re-plug or reload the hermes-kms module."
      );
    } else {
      add(feature_e::isolated_sessions, readiness_e::ready, "Hermes-KMS session cards carry private DRM seats.");
    }

    // --- zero-copy capture ---------------------------------------------------
    if (facts.hermes_kms_present) {
      add(feature_e::zero_copy_capture, readiness_e::ready, "Captured as DMA-BUF over the Hermes-KMS render node.");
    } else if (facts.x11) {
      add(feature_e::zero_copy_capture, readiness_e::degraded, "X11 capture goes through a CPU copy.");
    } else if (facts.linux_dmabuf && (facts.screencopy || facts.image_copy_capture)) {
      add(feature_e::zero_copy_capture, readiness_e::ready, "The session offers a screen-capture protocol with linux-dmabuf.");
    } else if (!facts.screencopy && !facts.image_copy_capture) {
      add(
        feature_e::zero_copy_capture,
        readiness_e::unavailable,
        "The session advertises no screen-capture protocol Hermes can use."
      );
    } else {
      add(
        feature_e::zero_copy_capture,
        readiness_e::degraded,
        "A capture protocol is available but linux-dmabuf is not, so frames are copied through the CPU."
      );
    }

    return reports;
  }

  bool hermesKmsSeatIsolationActive() {
    // In the session-device pool the host card deliberately stays on seat0, so
    // the question is not whether every card carries a private seat but whether
    // any does. None of them doing so is the signature of the driver's udev
    // rule not being installed, which is the failure this exists to catch.
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator {"/sys/class/drm", ec}) {
      const auto name = entry.path().filename().string();
      if (!name.starts_with("card") || name.find('-') != std::string::npos) {
        continue;  // "card0-Virtual-1" is a connector, not a card.
      }
      if (is_hermes_kms_card(entry.path()) && drm_card_seat(entry.path()).starts_with("hermes-kms-")) {
        return true;
      }
    }
    return false;
  }

  /**
   * Output names reach kscreen-doctor and xrandr as words of a shell command.
   * They come from the compositor or from sysfs, but the guard is what makes
   * that an assumption the code states rather than one it relies on.
   */
  static bool safe_output_name(const std::string &name) {
    return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
      return std::isalnum(c) || c == '-' || c == '_';
    });
  }

  std::string buildKScreenLayoutCommand(
    const std::string &virtual_output,
    const std::map<std::string, int> &enabled_before,
    int target_x,
    int target_y,
    int mode_width,
    int mode_height,
    int mode_refresh_hz
  ) {
    if (!safe_output_name(virtual_output)) {
      // Every caller reads this name back from the compositor, which has no
      // reason to invent one a shell would reinterpret. Refuse rather than
      // quote: a name that fails this is a bug upstream of here, not a layout
      // to apply.
      return {};
    }

    std::string command = "kscreen-doctor";
    int next_priority = 1;
    for (const auto &[output, priority] : enabled_before) {
      if (output == virtual_output || !safe_output_name(output)) {
        continue;
      }
      command += " output." + output + ".enable";
      if (priority > 0) {
        command += " output." + output + ".priority." + std::to_string(priority);
        next_priority = std::max(next_priority, priority + 1);
      }
    }

    command += " output." + virtual_output + ".enable";
    command += " output." + virtual_output + ".priority." + std::to_string(next_priority);
    command += " output." + virtual_output + ".position." +
               std::to_string(target_x) + ',' + std::to_string(target_y);

    // KWin adopts a hotplugged connector at the mode it prefers, not the one
    // the client negotiated. Hermes-KMS marks the client's exact CVT mode
    // DRM_MODE_TYPE_PREFERRED and still exposes the standard ladder beside it,
    // and KWin picked 1920x1080 out of that ladder for an 854x480 session. The
    // capture path reports the real scanout, so the client received a
    // full-resolution stream of a display it had asked to be small. Presence
    // was never enough to check - push the mode, as the Mutter path does
    // through ApplyMonitorsConfig.
    if (mode_width > 0 && mode_height > 0 && mode_refresh_hz > 0) {
      command += " output." + virtual_output + ".mode." +
                 std::to_string(mode_width) + 'x' + std::to_string(mode_height) +
                 '@' + std::to_string(mode_refresh_hz);
    }

    return command;
  }

  kscreen_mode_state_e kscreenModeState(
    const std::string &json_text,
    const std::string &output,
    int width,
    int height,
    int refresh_hz
  ) {
    if (json_text.empty() || output.empty() || width <= 0 || height <= 0 || refresh_hz <= 0) {
      return kscreen_mode_state_e::unknown_output;
    }

    try {
      const auto data = nlohmann::json::parse(json_text);
      for (const auto &entry : data.value("outputs", nlohmann::json::array())) {
        if (entry.value("name", std::string {}) != output) {
          continue;
        }

        const auto current_mode_id = entry.value("currentModeId", std::string {});
        auto state = kscreen_mode_state_e::not_advertised;
        for (const auto &mode : entry.value("modes", nlohmann::json::array())) {
          const auto size = mode.value("size", nlohmann::json::object());
          if (size.value("width", 0) != width || size.value("height", 0) != height) {
            continue;
          }
          if (std::lround(mode.value("refreshRate", 0.0)) != refresh_hz) {
            continue;
          }
          if (!current_mode_id.empty() && mode.value("id", std::string {}) == current_mode_id) {
            return kscreen_mode_state_e::current;
          }
          state = kscreen_mode_state_e::advertised;
        }
        return state;
      }
    } catch (const std::exception &error) {
      BOOST_LOG(warning) << "[VDISPLAY/KScreen] Could not parse the mode list: " << error.what();
    }
    return kscreen_mode_state_e::unknown_output;
  }

  namespace kscreen {
    struct output_t {
      std::string name;
      bool connected {false};
      bool enabled {false};
      int priority {0};
      int x {0};
      int y {0};
      int width {0};
      int height {0};
    };

    struct layout_t {
      // Every physical output that was enabled before the virtual connector
      // appeared, mapped to its KScreen priority. Exclusive mode disables all
      // of them, not just the primary one, and restore() brings them all back
      // with the priorities they had.
      std::map<std::string, int> original_outputs;
      std::string virtual_output;
      bool physical_output_disabled {false};
    };

    static std::mutex layouts_mutex;
    static std::map<std::string, layout_t> layouts;

    static bool available() {
      if (sessionCompositor() != compositor_e::kwin) {
        return false;
      }
      return ::access("/usr/bin/kscreen-doctor", X_OK) == 0 || ::access("/bin/kscreen-doctor", X_OK) == 0;
    }

    static std::string command_output(const char *command) {
      std::array<char, 4096> buffer {};
      std::string output;
      FILE *pipe = ::popen(command, "r");
      if (!pipe) {
        BOOST_LOG(warning) << "[VDISPLAY/KScreen] Failed to run " << command;
        return {};
      }
      while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
      }
      if (::pclose(pipe) != 0) {
        return {};
      }
      return output;
    }

    static std::vector<output_t> outputs() {
      std::vector<output_t> result;
      if (!available()) {
        return result;
      }

      const auto json_text = command_output("kscreen-doctor -j");
      if (json_text.empty()) {
        return result;
      }

      try {
        const auto data = nlohmann::json::parse(json_text);
        for (const auto &entry : data.value("outputs", nlohmann::json::array())) {
          output_t output {
            .name = entry.value("name", std::string {}),
            .connected = entry.value("connected", false),
            .enabled = entry.value("enabled", false),
            .priority = entry.value("priority", 0),
            .x = entry.value("pos", nlohmann::json::object()).value("x", 0),
            .y = entry.value("pos", nlohmann::json::object()).value("y", 0),
            .width = entry.value("size", nlohmann::json::object()).value("width", 0),
            .height = entry.value("size", nlohmann::json::object()).value("height", 0),
          };
          if (safe_output_name(output.name)) {
            result.emplace_back(std::move(output));
          }
        }
      } catch (const std::exception &error) {
        BOOST_LOG(warning) << "[VDISPLAY/KScreen] Could not parse output layout: " << error.what();
      }
      return result;
    }

    static std::set<std::string> connected_output_names(const std::vector<output_t> &current) {
      std::set<std::string> result;
      for (const auto &output : current) {
        if (output.connected) {
          result.insert(output.name);
        }
      }
      return result;
    }

    static std::map<std::string, int> enabled_output_priorities(const std::vector<output_t> &current) {
      std::map<std::string, int> result;
      for (const auto &output : current) {
        if (output.connected && output.enabled) {
          result.emplace(output.name, output.priority);
        }
      }
      return result;
    }

    static bool run_layout_command(const std::string &command) {
      if (::system((command + " >/dev/null 2>&1").c_str()) != 0) {
        BOOST_LOG(warning) << "[VDISPLAY/KScreen] Layout command failed: " << command;
        return false;
      }
      return true;
    }

    static std::string recovery_state_file() {
      const char *xdg_state = std::getenv("XDG_STATE_HOME");
      const char *home = std::getenv("HOME");
      std::string base;
      if (xdg_state && xdg_state[0]) {
        base = std::string {xdg_state} + "/hermes";
      } else if (home && home[0]) {
        base = std::string {home} + "/.local/state/hermes";
      } else {
        return {};
      }

      std::error_code ec;
      fs::create_directories(base, ec);
      return base + "/saved-primary";
    }

    /**
     * Build a single kscreen-doctor invocation that re-enables every given
     * output with the priority it had before Hermes touched the layout.
     * Outputs whose priority was not recorded are appended after the known
     * ones so the session never ends up without a primary.
     */
    static std::string enable_outputs_command(const std::map<std::string, int> &outputs) {
      std::string command = "kscreen-doctor";
      int next_priority = 1;
      for (const auto &[output, priority] : outputs) {
        const int effective = priority > 0 ? priority : next_priority;
        command += " output." + output + ".enable";
        command += " output." + output + ".priority." + std::to_string(effective);
        next_priority = std::max(next_priority, effective + 1);
      }
      return command;
    }

    /**
     * Persist every physical output exclusive mode is about to disable, one
     * `name priority` pair per line, so a crash mid-session cannot leave a
     * multi-monitor desktop with all of its screens dark.
     */
    static bool write_recovery_state(const std::map<std::string, int> &outputs) {
      std::map<std::string, int> valid;
      for (const auto &[output, priority] : outputs) {
        if (safe_output_name(output)) {
          valid.emplace(output, priority);
        }
      }
      if (valid.empty()) {
        return false;
      }

      const auto path = recovery_state_file();
      if (path.empty()) {
        return false;
      }

      const auto temporary_path = path + ".tmp";
      {
        std::ofstream file {temporary_path, std::ios::binary | std::ios::trunc};
        if (!file) {
          return false;
        }
        for (const auto &[output, priority] : valid) {
          file << output << ' ' << priority << '\n';
        }
      }

      std::error_code ec;
      fs::rename(temporary_path, path, ec);
      if (ec) {
        fs::remove(temporary_path, ec);
        return false;
      }
      return true;
    }

    static std::map<std::string, int> read_recovery_state() {
      std::map<std::string, int> outputs;
      const auto path = recovery_state_file();
      if (path.empty()) {
        return outputs;
      }

      std::ifstream file {path};
      std::string line;
      while (std::getline(file, line)) {
        std::istringstream stream {line};
        std::string output;
        int priority = 0;
        // A file written by an older Hermes holds a bare output name with no
        // priority; leave it at 0 so it is assigned the first free priority.
        if (!(stream >> output) || !safe_output_name(output)) {
          continue;
        }
        if (!(stream >> priority) || priority < 0) {
          priority = 0;
        }
        outputs.emplace(output, priority);
      }
      return outputs;
    }

    static void clear_recovery_state() {
      const auto path = recovery_state_file();
      if (path.empty()) {
        return;
      }
      std::error_code ec;
      fs::remove(path, ec);
    }

    static void recover_on_startup() {
      const auto saved_outputs = read_recovery_state();
      if (saved_outputs.empty()) {
        return;
      }

      std::string names;
      for (const auto &[output, priority] : saved_outputs) {
        names += names.empty() ? output : ", " + output;
      }
      BOOST_LOG(info) << "[VDISPLAY/KScreen] Recovering physical outputs left disabled by a previous session: " << names;
      if (run_layout_command(enable_outputs_command(saved_outputs))) {
        clear_recovery_state();
      } else {
        BOOST_LOG(warning) << "[VDISPLAY/KScreen] Startup recovery failed; retaining recovery state for the next attempt.";
      }
    }

    /**
     * Reapply the outputs that were enabled immediately before the virtual
     * connector appeared. KWin stores a setup for each exact output
     * combination; an exclusive streaming setup can therefore be replayed on
     * the next hotplug even after the setting has been turned off.
     *
     * Apply the complete baseline in one KScreen transaction. This both keeps
     * the local displays lit and replaces the stale saved setup. Exclusive
     * mode, when requested for this session, is applied afterwards.
     */
    static bool apply_pre_session_layout(
      const std::string &virtual_output,
      const std::map<std::string, int> &enabled_before,
      int mode_width,
      int mode_height,
      int mode_refresh_hz,
      virtual_display_layout_e layout
    ) {
      const auto current = outputs();
      int target_x = 0;
      int target_y = 0;
      bool have_existing_output = false;

      // Mirror overlaps the primary output so KWin clones the desktop onto
      // the virtual connector: windows the compositor places on the physical
      // monitor (a nested Gamescope, the panel, anything) then appear in the
      // captured stream. This restores the pre-0.5.0 behaviour, per app.
      if (layout == virtual_display_layout_e::mirror) {
        for (const auto &output : current) {
          if (output.name == virtual_output ||
              !output.connected ||
              !output.enabled ||
              !enabled_before.contains(output.name)) {
            continue;
          }
          // KScreen priority 1 marks the primary output. Before one is
          // found, keep the first enabled output as the fallback anchor.
          if (output.priority == 1 || !have_existing_output) {
            target_x = output.x;
            target_y = output.y;
            have_existing_output = true;
            if (output.priority == 1) {
              break;
            }
          }
        }
      } else {
        // Keep the virtual desktop regions disjoint. KWin may replay a saved
        // hotplug layout with the new connector at 0,0, directly overlapping the
        // physical monitor. Besides mirroring physical content into the stream,
        // overlap makes absolute input ambiguous. Append each new virtual output
        // to the right of every output that was active before this hotplug.
        for (const auto &output : current) {
          if (output.name == virtual_output ||
              !output.connected ||
              !output.enabled ||
              !enabled_before.contains(output.name)) {
            continue;
          }
          if (!have_existing_output) {
            target_y = output.y;
            have_existing_output = true;
          } else {
            target_y = std::min(target_y, output.y);
          }
          target_x = std::max(target_x, output.x + output.width);
        }
      }

      const bool has_mode = mode_width > 0 && mode_height > 0 && mode_refresh_hz > 0;
      const auto command = buildKScreenLayoutCommand(
        virtual_output, enabled_before, target_x, target_y, mode_width, mode_height, mode_refresh_hz);
      if (command.empty()) {
        BOOST_LOG(warning) << "[VDISPLAY/KScreen] Refusing to build a layout around output name "
                           << virtual_output;
        return false;
      }

      if (!run_layout_command(command)) {
        // A mode KWin refuses must not cost us the rest of the layout: the
        // output still has to be enabled and placed or there is nothing to
        // capture at all. Retry without it, and say what the session lost
        // rather than leaving a silent resolution mismatch.
        if (!has_mode || !run_layout_command(buildKScreenLayoutCommand(
                           virtual_output, enabled_before, target_x, target_y, 0, 0, 0))) {
          return false;
        }
        BOOST_LOG(warning) << "[VDISPLAY/KScreen] KWin rejected mode "
                           << mode_width << 'x' << mode_height << '@' << mode_refresh_hz
                           << " for " << virtual_output
                           << "; the output is enabled at the mode KWin chose, so the stream "
                              "will not match the resolution the client asked for.";
      }
      BOOST_LOG(info) << "[VDISPLAY/KScreen] Positioned " << virtual_output
                      << " at " << target_x << ',' << target_y
                      << (layout == virtual_display_layout_e::mirror ?
                            " overlapping the primary output (mirror)" :
                            " outside the pre-session output regions");
      return true;
    }

    /**
     * Loading old Hermes-KMS packages with initial_enabled=1 can make KWin
     * disable every physical output before Hermes is running. Once the
     * unowned connector is disconnected, restore a usable local layout if
     * KWin has not already done so itself.
     */
    static void recover_after_unowned_virtual_disconnect(const std::string &virtual_connector) {
      if (!available()) {
        return;
      }

      for (int attempt = 0; attempt < 20; ++attempt) {
        const auto current = outputs();
        std::vector<std::string> physical_outputs;
        bool physical_output_enabled = false;
        for (const auto &output : current) {
          if (!output.connected || output.name == virtual_connector) {
            continue;
          }
          physical_outputs.emplace_back(output.name);
          physical_output_enabled = physical_output_enabled || output.enabled;
        }

        if (physical_output_enabled) {
          return;
        }
        if (!physical_outputs.empty()) {
          std::string command = "kscreen-doctor";
          int priority = 1;
          for (const auto &output : physical_outputs) {
            command += " output." + output + ".enable";
            command += " output." + output + ".priority." + std::to_string(priority++);
          }
          BOOST_LOG(warning) << "[VDISPLAY/KScreen] Restoring physical outputs after disconnecting a boot-enabled virtual output.";
          run_layout_command(command);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {50});
      }
    }

    static bool activate_evdi_output(
      const std::string &display_name,
      const std::set<std::string> &outputs_before,
      const std::map<std::string, int> &enabled_before,
      const std::string &connector_name,
      const char *backend_label,
      int mode_width,
      int mode_height,
      int mode_refresh_hz,
      virtual_display_layout_e layout
    ) {
      if (!available()) {
        return false;
      }

      std::string virtual_output;
      for (int attempt = 0; attempt < 30 && virtual_output.empty(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds {100});
        const auto current_outputs = outputs();
        const auto expected = std::find_if(current_outputs.begin(), current_outputs.end(), [&](const auto &output) {
          return output.connected && output.name == connector_name;
        });
        if (expected != current_outputs.end()) {
          virtual_output = expected->name;
          break;
        }
        for (const auto &output : current_outputs) {
          if (output.connected && outputs_before.find(output.name) == outputs_before.end()) {
            virtual_output = output.name;
            break;
          }
        }
      }

      if (!safe_output_name(virtual_output)) {
        BOOST_LOG(warning) << "[VDISPLAY/KScreen] " << backend_label << " output " << connector_name
                           << " was not enumerated by KWin.";
        return false;
      }

      // At display-CREATION time we only *enable* the virtual output so the
      // compositor starts composing on it and capture can read its framebuffer.
      // We deliberately do NOT touch the physical outputs' priorities or
      // disable them here: that is a SESSION-level action and must only happen
      // once the stream actually starts (see make_exclusive), and only in
      // isolated mode. Otherwise creating the display would steal the user's
      // monitors before any session is live, blanking the local screens
      // prematurely.
      std::map<std::string, int> original_outputs;
      for (const auto &[output, priority] : enabled_before) {
        if (output != virtual_output && safe_output_name(output)) {
          original_outputs.emplace(output, priority);
        }
      }
      if (!apply_pre_session_layout(virtual_output, enabled_before, mode_width, mode_height, mode_refresh_hz, layout)) {
        return false;
      }

      std::lock_guard<std::mutex> lock(layouts_mutex);
      layouts[display_name] = {
        .original_outputs = std::move(original_outputs),
        .virtual_output = virtual_output,
        .physical_output_disabled = false,
      };
      BOOST_LOG(info) << "[VDISPLAY/KScreen] Enabled " << backend_label << " output " << virtual_output
                      << " and restored the pre-session physical layout";
      return true;
    }

    static bool is_active(const std::string &display_name) {
      std::lock_guard<std::mutex> lock(layouts_mutex);
      return layouts.find(display_name) != layouts.end();
    }

    /**
     * Drive this session's virtual output at @p width x @p height @ @p refresh_hz.
     *
     * Telling the kernel is only half of a mode change: KWin keeps driving the
     * connector at whatever mode it adopted until something asks it to move,
     * and the capture path reads the real scanout, so a change that stops at
     * the driver leaves the stream on the old geometry.
     *
     * Read KWin's mode list first and confirm afterwards, rather than firing a
     * command and trusting its exit code. That separates the three ways this
     * fails - the output is not enumerated, it does not advertise the geometry,
     * or KWin took the request and did not keep it - which otherwise arrive as
     * one indistinguishable "Layout command failed".
     */
    static bool apply_mode(const std::string &display_name, int width, int height, int refresh_hz) {
      if (!available() || width <= 0 || height <= 0 || refresh_hz <= 0) {
        return false;
      }

      std::string output_name;
      {
        std::lock_guard<std::mutex> lock(layouts_mutex);
        const auto it = layouts.find(display_name);
        if (it == layouts.end()) {
          return false;
        }
        output_name = it->second.virtual_output;
      }
      if (!safe_output_name(output_name)) {
        return false;
      }

      const std::string mode = std::to_string(width) + 'x' + std::to_string(height) + '@' +
                               std::to_string(refresh_hz);

      switch (kscreenModeState(command_output("kscreen-doctor -j"), output_name, width, height, refresh_hz)) {
        case kscreen_mode_state_e::current:
          BOOST_LOG(debug) << "[VDISPLAY/KScreen] " << output_name << " is already driven at " << mode << '.';
          return true;
        case kscreen_mode_state_e::not_advertised:
          BOOST_LOG(warning) << "[VDISPLAY/KScreen] " << output_name << " does not advertise " << mode
                             << "; leaving KWin's own choice in place.";
          return false;
        case kscreen_mode_state_e::unknown_output:
          // KWin may not have enumerated the hotplug yet, and a reply we could
          // not read is not evidence of anything. Ask for the mode anyway.
          break;
        case kscreen_mode_state_e::advertised:
          break;
      }

      if (!run_layout_command("kscreen-doctor output." + output_name + ".mode." + mode)) {
        return false;
      }

      switch (kscreenModeState(command_output("kscreen-doctor -j"), output_name, width, height, refresh_hz)) {
        case kscreen_mode_state_e::current:
          BOOST_LOG(info) << "[VDISPLAY/KScreen] Driving " << output_name << " at " << mode
                          << " for this session.";
          return true;
        case kscreen_mode_state_e::unknown_output:
          BOOST_LOG(debug) << "[VDISPLAY/KScreen] Applied " << mode << " to " << output_name
                           << "; KWin's reply was unreadable, so it could not be confirmed.";
          return true;
        default:
          BOOST_LOG(warning) << "[VDISPLAY/KScreen] KWin accepted " << mode << " for " << output_name
                             << " but is not driving it; the stream will not match the resolution the "
                                "client asked for.";
          return false;
      }
    }

    static bool geometry(const std::string &display_name, int &x, int &y, int &env_width, int &env_height) {
      std::string output_name;
      {
        std::lock_guard<std::mutex> lock(layouts_mutex);
        const auto it = layouts.find(display_name);
        if (it == layouts.end()) {
          return false;
        }
        output_name = it->second.virtual_output;
      }

      const auto current = outputs();
      const auto selected = std::find_if(current.begin(), current.end(), [&](const auto &output) {
        return output.connected && output.enabled && output.name == output_name;
      });
      if (selected == current.end()) {
        return false;
      }

      int min_x = 0;
      int min_y = 0;
      int max_x = 0;
      int max_y = 0;
      for (const auto &output : current) {
        if (!output.connected || !output.enabled) {
          continue;
        }
        min_x = std::min(min_x, output.x);
        min_y = std::min(min_y, output.y);
        max_x = std::max(max_x, output.x + output.width);
        max_y = std::max(max_y, output.y + output.height);
      }
      x = selected->x - min_x;
      y = selected->y - min_y;
      env_width = max_x - min_x;
      env_height = max_y - min_y;
      return env_width > 0 && env_height > 0;
    }

    // Called at SESSION START (only when isolated mode is on) to hand the
    // desktop over to the virtual output: make it primary and disable the
    // physical monitors. This is the moment the local screens are allowed to go
    // dark — never before. restore() reverses it when the session ends.
    static bool make_exclusive(const std::string &display_name) {
      std::lock_guard<std::mutex> lock(layouts_mutex);
      const auto it = layouts.find(display_name);
      if (it == layouts.end() || it->second.physical_output_disabled) {
        return it != layouts.end();
      }

      // Every lit physical output has to go dark, not just the one that
      // happened to be primary: on a multi-monitor desktop the secondary
      // screens otherwise stay on and keep showing the local session.
      // Read the live layout so monitors that were enabled after the virtual
      // display was created are covered as well, and fall back to the
      // pre-session snapshot when KScreen cannot be queried. Priorities come
      // from the snapshot where known so restore() reinstates the layout the
      // user actually had.
      std::map<std::string, int> targets;
      for (const auto &output : outputs()) {
        if (!output.connected || !output.enabled ||
            output.name == it->second.virtual_output ||
            !safe_output_name(output.name)) {
          continue;
        }
        const auto known = it->second.original_outputs.find(output.name);
        targets.emplace(
          output.name,
          known != it->second.original_outputs.end() ? known->second : output.priority
        );
      }
      if (targets.empty()) {
        targets = it->second.original_outputs;
      }
      if (targets.empty()) {
        return true;
      }

      // Promote the virtual output to primary and disable the physical ones in
      // a single atomic kscreen-doctor call so the session never lands on a
      // transient no-primary layout.
      std::string command = "kscreen-doctor output." + it->second.virtual_output + ".priority.1";
      for (const auto &[output, priority] : targets) {
        command += " output." + output + ".disable";
      }
      if (!write_recovery_state(targets)) {
        BOOST_LOG(error) << "[VDISPLAY/KScreen] Refusing exclusive mode because monitor recovery state could not be written.";
        return false;
      }
      if (!run_layout_command(command)) {
        BOOST_LOG(warning) << "[VDISPLAY/KScreen] Exclusive layout command failed; retaining recovery state in case it was partially applied.";
        return false;
      }
      BOOST_LOG(info) << "[VDISPLAY/KScreen] Exclusive mode disabled " << targets.size()
                      << " physical output(s) in favour of " << it->second.virtual_output;
      it->second.original_outputs = std::move(targets);
      it->second.physical_output_disabled = true;
      return true;
    }

    static void restore(const std::string &display_name) {
      std::lock_guard<std::mutex> lock(layouts_mutex);
      const auto it = layouts.find(display_name);
      if (it == layouts.end()) {
        return;
      }
      // Re-enable every physical output with the priority it had before the
      // session, in one command, so the desktop never sits with two primaries
      // or none. The virtual connector itself disappears when the display is
      // torn down, so we don't need to .disable it explicitly, but restoring
      // the physical priorities is what brings the screens back.
      bool restored = true;
      if (!it->second.original_outputs.empty()) {
        restored = run_layout_command(enable_outputs_command(it->second.original_outputs));
      }
      if (restored) {
        clear_recovery_state();
      } else {
        BOOST_LOG(warning) << "[VDISPLAY/KScreen] Physical output restoration failed; retaining recovery state for the next startup.";
      }
      layouts.erase(it);
    }
  }  // namespace kscreen

  // GNOME / Mutter output management. Mutter does not speak kscreen-doctor or
  // the wlroots output-management protocol; it exposes
  // org.gnome.Mutter.DisplayConfig over D-Bus. We talk to it via `gdbus` (always
  // present on GNOME) to avoid a libdbus build dependency.
  //
  // This backend is intentionally conservative: Mutter typically adopts the
  // HERMES-1 connector after Hermes connects it for a stream, so the critical
  // step is to *verify* that the virtual output is present and active in
  // Mutter's current state rather than to push a full ApplyMonitorsConfig
  // (which is all-or-nothing and risky to build blind).
  namespace mutter {
    static bool available() {
      if (sessionCompositor() != compositor_e::mutter) {
        return false;
      }
      return ::access("/usr/bin/gdbus", X_OK) == 0 || ::access("/bin/gdbus", X_OK) == 0;
    }

    static std::string command_output(const char *command) {
      std::array<char, 8192> buffer {};
      std::string output;
      FILE *pipe = ::popen(command, "r");
      if (!pipe) {
        return {};
      }
      while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
      }
      if (::pclose(pipe) != 0) {
        return {};
      }
      return output;
    }

    // ------------------------------------------------------------------
    // ApplyMonitorsConfig
    //
    // A boot-enabled virtual connector leaves Mutter extending the desktop
    // onto an output nobody is streaming to. KWin recovers through
    // kscreen-doctor; Mutter only reconfigures through ApplyMonitorsConfig,
    // whose reply-vs-request shapes differ:
    //
    //   GetCurrentState     -> a(iiduba(ssss)a{sv})   logical monitors
    //   ApplyMonitorsConfig -> a(iiduba(ssa{sv}))     logical monitors
    //
    // so a logical monitor loses its trailing property dict, and each monitor
    // collapses from (connector, vendor, product, serial) to
    // (connector, mode_id, properties). The mode id has to be recovered from
    // the monitor's own mode list - the one carrying 'is-current': <true>.
    //
    // ApplyMonitorsConfig is all-or-nothing, so every config is submitted to
    // Mutter's own VERIFY method first and only applied when that succeeds.
    // ------------------------------------------------------------------

    enum class apply_method : unsigned {
      verify = 0,
      temporary = 1,
      persistent = 2,
    };

    struct logical_monitor_t {
      std::string x;
      std::string y;
      std::string scale;
      std::string transform;
      std::string primary;
      std::vector<std::string> connectors;
    };

    struct mode_t {
      std::string id;
      int width {0};
      int height {0};
      double refresh {0.0};
      bool current {false};
    };

    struct monitor_t {
      std::string connector;
      // The rest of the monitor spec. GNOME identifies a monitor by these three
      // when binding an input device to it, not by the connector.
      std::string vendor;
      std::string product;
      std::string serial;
      std::vector<mode_t> modes;
    };

    /// Values of Mutter's "layout-mode" property.
    enum class layout_mode_e : unsigned {
      logical = 1,   ///< A logical monitor measures mode / scale.
      physical = 2,  ///< A logical monitor measures the mode, whatever the scale.
    };

    struct state_t {
      std::string serial;
      std::map<std::string, std::string> current_mode;  ///< connector -> mode id
      std::vector<monitor_t> monitors;
      std::vector<logical_monitor_t> logical_monitors;
      // Absence of "layout-mode" means it cannot be changed and that logical is
      // in use, so that is the default rather than a guess.
      layout_mode_e layout_mode {layout_mode_e::logical};
      bool global_scale_required {false};

      const monitor_t *find_monitor(const std::string &connector) const {
        for (const auto &monitor : monitors) {
          if (monitor.connector == connector) {
            return &monitor;
          }
        }
        return nullptr;
      }

      const logical_monitor_t *find_logical_monitor(const std::string &connector) const {
        for (const auto &lm : logical_monitors) {
          if (std::find(lm.connectors.begin(), lm.connectors.end(), connector) != lm.connectors.end()) {
            return &lm;
          }
        }
        return nullptr;
      }

      static double scale_of(const logical_monitor_t &lm) {
        const double scale = std::atof(lm.scale.c_str());
        return scale > 0.0 ? scale : 1.0;
      }

      /**
       * Size of a logical monitor in layout coordinates - the space the desktop,
       * and therefore the pointer, is measured in.
       *
       * Mutter derives it from the current mode of the monitors assigned to it:
       * a rotated transform swaps the axes, and in the logical layout mode the
       * result is divided by the logical monitor's scale. Taking the mode size
       * instead is only correct at scale 1 with no rotation.
       */
      bool logical_size(const logical_monitor_t &lm, int &width, int &height) const {
        if (lm.connectors.empty()) {
          return false;
        }
        const auto current = current_mode.find(lm.connectors.front());
        const auto *monitor = find_monitor(lm.connectors.front());
        if (current == current_mode.end() || !monitor) {
          return false;
        }
        const mode_t *mode = nullptr;
        for (const auto &candidate : monitor->modes) {
          if (candidate.id == current->second) {
            mode = &candidate;
            break;
          }
        }
        if (!mode || mode->width <= 0 || mode->height <= 0) {
          return false;
        }

        double w = mode->width;
        double h = mode->height;
        // Transforms 1, 3, 5 and 7 are the 90 and 270 degree rotations.
        const int transform = std::atoi(lm.transform.c_str());
        if (transform == 1 || transform == 3 || transform == 5 || transform == 7) {
          std::swap(w, h);
        }
        if (layout_mode == layout_mode_e::logical) {
          const double scale = scale_of(lm);
          w /= scale;
          h /= scale;
        }

        width = static_cast<int>(std::lround(w));
        height = static_cast<int>(std::lround(h));
        return width > 0 && height > 0;
      }
    };

    static const char *get_current_state_command =
      "gdbus call --session "
      "--dest org.gnome.Mutter.DisplayConfig "
      "--object-path /org/gnome/Mutter/DisplayConfig "
      "--method org.gnome.Mutter.DisplayConfig.GetCurrentState 2>/dev/null";

    // Connector names and mode ids are interpolated into a shell command, so
    // restrict them to what Mutter actually emits ("DP-2", "4096x2160@100.000+vrr").
    static bool safe_variant_token(const std::string &value) {
      return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '@' || c == '+' || c == ':';
      });
    }

    static std::string trim_variant(const std::string &value) {
      const size_t first = value.find_first_not_of(" \t\n\r");
      if (first == std::string::npos) {
        return {};
      }
      const size_t last = value.find_last_not_of(" \t\n\r");
      return value.substr(first, last - first + 1);
    }

    // Split the body of a GVariant container on its top-level commas.
    static std::vector<std::string> split_variant(const std::string &body) {
      std::vector<std::string> parts;
      int depth = 0;
      bool in_string = false;
      size_t start = 0;

      for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (in_string) {
          if (c == '\\') {
            ++i;
          } else if (c == '\'') {
            in_string = false;
          }
          continue;
        }
        switch (c) {
          case '\'':
            in_string = true;
            break;
          case '(':
          case '[':
          case '{':
          case '<':
            ++depth;
            break;
          case ')':
          case ']':
          case '}':
          case '>':
            --depth;
            break;
          case ',':
            if (depth == 0) {
              parts.emplace_back(trim_variant(body.substr(start, i - start)));
              start = i + 1;
            }
            break;
          default:
            break;
        }
      }
      parts.emplace_back(trim_variant(body.substr(start)));
      return parts;
    }

    static bool strip_variant(std::string &value, char open, char close) {
      if (value.size() < 2 || value.front() != open || value.back() != close) {
        return false;
      }
      value = trim_variant(value.substr(1, value.size() - 2));
      return true;
    }

    static std::string unquote_variant(const std::string &value) {
      if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        return value.substr(1, value.size() - 2);
      }
      return value;
    }

    // GVariant prints a type annotation ("uint32 0") only where the type cannot
    // be inferred from context; keep the literal and drop any prefix.
    static std::string numeric_variant(const std::string &value) {
      const size_t pos = value.find_last_of(' ');
      return pos == std::string::npos ? value : value.substr(pos + 1);
    }

    // A scale must stay a double: emitting "1" for 1.0 would type the field as
    // an integer and the whole config would be rejected.
    static std::string double_variant(const std::string &value) {
      std::string literal = numeric_variant(value);
      if (literal.find('.') == std::string::npos && literal.find('e') == std::string::npos &&
          literal.find('E') == std::string::npos) {
        literal += ".0";
      }
      return literal;
    }

    static bool parse_current_state(const std::string &reply, state_t &out) {
      std::string body = trim_variant(reply);
      if (!strip_variant(body, '(', ')')) {
        return false;
      }
      const auto top = split_variant(body);
      if (top.size() != 4) {
        return false;
      }

      out.serial = numeric_variant(top[0]);
      if (out.serial.empty()) {
        return false;
      }

      std::string monitors = top[1];
      if (!strip_variant(monitors, '[', ']')) {
        return false;
      }
      for (const auto &entry : split_variant(monitors)) {
        std::string monitor = entry;
        if (!strip_variant(monitor, '(', ')')) {
          continue;
        }
        const auto fields = split_variant(monitor);
        if (fields.size() < 2) {
          continue;
        }
        std::string spec = fields[0];
        if (!strip_variant(spec, '(', ')')) {
          continue;
        }
        const auto spec_fields = split_variant(spec);
        if (spec_fields.empty()) {
          continue;
        }
        const std::string connector = unquote_variant(spec_fields[0]);
        std::string modes = fields[1];
        if (connector.empty() || !strip_variant(modes, '[', ']')) {
          continue;
        }
        monitor_t parsed_monitor;
        parsed_monitor.connector = connector;
        if (spec_fields.size() >= 4) {
          parsed_monitor.vendor = unquote_variant(spec_fields[1]);
          parsed_monitor.product = unquote_variant(spec_fields[2]);
          parsed_monitor.serial = unquote_variant(spec_fields[3]);
        }
        for (const auto &mode_entry : split_variant(modes)) {
          std::string mode = mode_entry;
          if (!strip_variant(mode, '(', ')')) {
            continue;
          }
          const auto mode_fields = split_variant(mode);
          if (mode_fields.size() < 7) {
            continue;
          }

          mode_t parsed_mode;
          parsed_mode.id = unquote_variant(mode_fields[0]);
          if (parsed_mode.id.empty()) {
            continue;
          }
          parsed_mode.width = std::atoi(numeric_variant(mode_fields[1]).c_str());
          parsed_mode.height = std::atoi(numeric_variant(mode_fields[2]).c_str());
          parsed_mode.refresh = std::atof(numeric_variant(mode_fields[3]).c_str());
          parsed_mode.current = mode_fields.back().find("'is-current': <true>") != std::string::npos;
          if (parsed_mode.current) {
            out.current_mode[connector] = parsed_mode.id;
          }
          parsed_monitor.modes.emplace_back(std::move(parsed_mode));
        }
        out.monitors.emplace_back(std::move(parsed_monitor));
      }

      std::string logical = top[2];
      if (!strip_variant(logical, '[', ']')) {
        return false;
      }
      for (const auto &entry : split_variant(logical)) {
        std::string lm = entry;
        if (!strip_variant(lm, '(', ')')) {
          continue;
        }
        const auto fields = split_variant(lm);
        if (fields.size() < 6) {
          continue;
        }

        logical_monitor_t parsed;
        parsed.x = numeric_variant(fields[0]);
        parsed.y = numeric_variant(fields[1]);
        parsed.scale = double_variant(fields[2]);
        parsed.transform = numeric_variant(fields[3]);
        parsed.primary = numeric_variant(fields[4]);

        std::string specs = fields[5];
        if (!strip_variant(specs, '[', ']')) {
          continue;
        }
        for (const auto &spec_entry : split_variant(specs)) {
          std::string spec = spec_entry;
          if (!strip_variant(spec, '(', ')')) {
            continue;
          }
          const auto spec_fields = split_variant(spec);
          if (spec_fields.empty()) {
            continue;
          }
          const std::string connector = unquote_variant(spec_fields[0]);
          if (!connector.empty()) {
            parsed.connectors.emplace_back(connector);
          }
        }

        if (!parsed.connectors.empty()) {
          out.logical_monitors.emplace_back(std::move(parsed));
        }
      }

      // The properties dict carries the two flags that decide how a layout is
      // measured and what scale a new logical monitor may take. Both are
      // optional; the defaults in state_t are what their absence means.
      std::string properties = top[3];
      if (strip_variant(properties, '{', '}')) {
        for (const auto &entry : split_variant(properties)) {
          const size_t colon = entry.find(':');
          if (colon == std::string::npos) {
            continue;
          }
          const std::string key = unquote_variant(trim_variant(entry.substr(0, colon)));
          std::string value = trim_variant(entry.substr(colon + 1));
          strip_variant(value, '<', '>');
          if (key == "layout-mode") {
            if (std::atoi(numeric_variant(value).c_str()) == static_cast<int>(layout_mode_e::physical)) {
              out.layout_mode = layout_mode_e::physical;
            }
          } else if (key == "global-scale-required") {
            out.global_scale_required = numeric_variant(value) == "true";
          }
        }
      }

      return !out.logical_monitors.empty();
    }

    // Render the a(iiduba(ssa{sv})) argument ApplyMonitorsConfig expects.
    static bool build_apply_argument(const state_t &state,
                                     const std::vector<logical_monitor_t> &layout,
                                     std::string &argument,
                                     const std::map<std::string, std::string> &mode_override = {}) {
      argument = "[";
      for (size_t i = 0; i < layout.size(); ++i) {
        const auto &lm = layout[i];
        if (lm.primary != "true" && lm.primary != "false") {
          return false;
        }
        argument += (i ? ", (" : "(");
        argument += lm.x + ", " + lm.y + ", " + lm.scale + ", uint32 " + lm.transform + ", " + lm.primary + ", [";
        for (size_t j = 0; j < lm.connectors.size(); ++j) {
          const auto &connector = lm.connectors[j];
          std::string mode_id;
          if (const auto override_it = mode_override.find(connector); override_it != mode_override.end()) {
            mode_id = override_it->second;
          } else if (const auto current = state.current_mode.find(connector); current != state.current_mode.end()) {
            mode_id = current->second;
          } else {
            BOOST_LOG(warning) << "[VDISPLAY/Mutter] No mode known for " << connector << "; leaving the layout alone.";
            return false;
          }
          if (!safe_variant_token(connector) || !safe_variant_token(mode_id)) {
            BOOST_LOG(warning) << "[VDISPLAY/Mutter] Refusing to build a config from an unexpected connector or mode id.";
            return false;
          }
          argument += (j ? ", ('" : "('");
          argument += connector + "', '" + mode_id + "', @a{sv} {})";
        }
        argument += "])";
      }
      argument += "]";
      return true;
    }

    static bool run_apply(const state_t &state, const std::string &argument, apply_method method) {
      const std::string command =
        "gdbus call --session "
        "--dest org.gnome.Mutter.DisplayConfig "
        "--object-path /org/gnome/Mutter/DisplayConfig "
        "--method org.gnome.Mutter.DisplayConfig.ApplyMonitorsConfig " +
        state.serial + " " + std::to_string(static_cast<unsigned>(method)) + " \"" + argument + "\" \"@a{sv} {}\" 2>&1";

      std::array<char, 4096> buffer {};
      std::string output;
      FILE *pipe = ::popen(command.c_str(), "r");
      if (!pipe) {
        return false;
      }
      while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
      }
      if (::pclose(pipe) != 0) {
        BOOST_LOG(warning) << "[VDISPLAY/Mutter] ApplyMonitorsConfig ("
                           << (method == apply_method::verify ? "verify" : "apply")
                           << ") failed: " << trim_variant(output);
        return false;
      }
      return true;
    }

    enum class layout_repair {
      unavailable,  ///< The reply did not parse; worth retrying.
      not_needed,   ///< The connector is not in Mutter's layout.
      unsafe,       ///< Dropping it would leave Mutter with nothing to drive.
      ready,        ///< `argument` holds the config to submit.
    };

    /**
     * Build the ApplyMonitorsConfig argument that keeps every logical monitor in
     * @p reply except @p virtual_connector. A mirrored logical monitor keeps its
     * physical members; a virtual-only one disappears with the connector, and
     * the primary flag moves to the first survivor when the dropped one held it.
     */
    static layout_repair build_layout_without(const std::string &reply,
                                              const std::string &virtual_connector,
                                              state_t &state,
                                              std::string &argument) {
      state = {};
      argument.clear();
      if (reply.empty() || virtual_connector.empty() || !parse_current_state(reply, state)) {
        return layout_repair::unavailable;
      }

      std::vector<logical_monitor_t> kept;
      bool dropped_primary = false;
      bool dropped = false;
      for (auto lm : state.logical_monitors) {
        const auto before = lm.connectors.size();
        lm.connectors.erase(
          std::remove(lm.connectors.begin(), lm.connectors.end(), virtual_connector),
          lm.connectors.end()
        );
        if (lm.connectors.size() == before) {
          kept.emplace_back(std::move(lm));
          continue;
        }
        dropped = true;
        if (lm.connectors.empty()) {
          dropped_primary = dropped_primary || lm.primary == "true";
        } else {
          kept.emplace_back(std::move(lm));
        }
      }

      if (!dropped) {
        return layout_repair::not_needed;
      }
      if (kept.empty()) {
        return layout_repair::unsafe;
      }
      if (dropped_primary) {
        kept.front().primary = "true";
      }
      return build_apply_argument(state, kept, argument) ? layout_repair::ready : layout_repair::unsafe;
    }

    /**
     * Loading Hermes-KMS with initial_enabled=1 - as older packages wrote into
     * /etc/modprobe.d/hermes-kms.conf - exposes a connected virtual output
     * before Hermes runs, and Mutter extends the desktop onto it. Once that
     * unowned connector is disconnected, drop it from Mutter's layout if the
     * compositor has not done so itself.
     */
    static void recover_after_unowned_virtual_disconnect(const std::string &virtual_connector) {
      if (!available() || virtual_connector.empty()) {
        return;
      }

      for (int attempt = 0; attempt < 20; ++attempt) {
        state_t state;
        std::string argument;
        switch (build_layout_without(command_output(get_current_state_command), virtual_connector, state, argument)) {
          case layout_repair::unavailable:
            std::this_thread::sleep_for(std::chrono::milliseconds {50});
            continue;
          case layout_repair::not_needed:
            return;  // Mutter already dropped the disconnected output.
          case layout_repair::unsafe:
            BOOST_LOG(warning) << "[VDISPLAY/Mutter] Cannot safely drop virtual output " << virtual_connector
                               << " from Mutter's layout; leaving it to the compositor.";
            return;
          case layout_repair::ready:
            break;
        }

        // ApplyMonitorsConfig is all-or-nothing, so let Mutter validate the
        // config before it can black out the physical outputs.
        if (!run_apply(state, argument, apply_method::verify)) {
          return;
        }

        BOOST_LOG(warning) << "[VDISPLAY/Mutter] Dropping boot-enabled virtual output " << virtual_connector
                           << " from Mutter's layout.";
        // Temporary, like every other layout change Hermes makes: the user's
        // saved layout in ~/.config/monitors.xml stays theirs.
        run_apply(state, argument, apply_method::temporary);
        return;
      }
    }

    enum class mode_push {
      unavailable,  ///< Reply unusable, or Mutter has not probed the connector yet.
      unsupported,  ///< The connector does not advertise the requested geometry.
      already_set,  ///< Mutter already drives the connector at that mode.
      ready,        ///< `argument` holds the config to submit.
    };

    /**
     * Build the ApplyMonitorsConfig argument that drives @p connector at the
     * requested geometry, keeping every other logical monitor as Mutter has it.
     *
     * When Mutter has adopted the connector without placing it in the layout,
     * a logical monitor is appended immediately right of the existing ones.
     * Mutter rejects a layout with gaps ("Logical monitors not adjacent"), so
     * the new monitor starts exactly at the current right edge.
     */
    static mode_push build_layout_with_mode(const std::string &reply,
                                            const std::string &connector,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t refresh_mhz,
                                            state_t &state,
                                            std::string &argument) {
      state = {};
      argument.clear();
      if (reply.empty() || connector.empty() || !width || !height || !parse_current_state(reply, state)) {
        return mode_push::unavailable;
      }

      const monitor_t *monitor = state.find_monitor(connector);
      if (!monitor) {
        return mode_push::unavailable;
      }

      // Hermes carries refresh rates in mHz, matching wlr-output-management;
      // Mutter reports them in Hz. Compare in Hz.
      const auto refresh_delta = [refresh_mhz](double rate) {
        const double delta = rate - static_cast<double>(refresh_mhz) / 1000.0;
        return delta < 0 ? -delta : delta;
      };

      const mode_t *chosen = nullptr;
      for (const auto &mode : monitor->modes) {
        if (mode.width != static_cast<int>(width) || mode.height != static_cast<int>(height)) {
          continue;
        }
        if (!chosen || refresh_delta(mode.refresh) < refresh_delta(chosen->refresh)) {
          chosen = &mode;
        }
      }
      if (!chosen) {
        return mode_push::unsupported;
      }
      if (chosen->current) {
        return mode_push::already_set;
      }

      std::vector<logical_monitor_t> layout = state.logical_monitors;
      const auto owner = std::find_if(layout.begin(), layout.end(), [&connector](const logical_monitor_t &lm) {
        return std::find(lm.connectors.begin(), lm.connectors.end(), connector) != lm.connectors.end();
      });
      if (owner == layout.end()) {
        int right_edge = 0;
        // A layout is measured in logical pixels, so the right edge has to come
        // from mode / scale. Using the mode size on a scaled desktop places the
        // new monitor past the edge, which leaves a gap, and Mutter rejects a
        // layout whose monitors are not all adjacent - taking the whole config
        // down with it, not just our output.
        for (const auto &lm : layout) {
          int lm_width = 0;
          int lm_height = 0;
          if (!state.logical_size(lm, lm_width, lm_height)) {
            // Without a size the placement cannot be made adjacent, and Mutter
            // would reject the whole config.
            return mode_push::unavailable;
          }
          right_edge = std::max(right_edge, std::atoi(lm.x.c_str()) + lm_width);
        }
        logical_monitor_t added;
        added.x = std::to_string(right_edge);
        added.y = "0";
        // Where the backend demands one scale for the whole desktop, a scale of
        // our own is refused ("Logical monitor scales must be identical"), so
        // adopt whatever the existing monitors carry.
        added.scale = "1.0";
        if (state.global_scale_required && !layout.empty() && !layout.front().scale.empty()) {
          added.scale = layout.front().scale;
        }
        added.transform = "0";
        added.primary = layout.empty() ? "true" : "false";
        added.connectors.emplace_back(connector);
        layout.emplace_back(std::move(added));
      }

      return build_apply_argument(state, layout, argument, {{connector, chosen->id}}) ? mode_push::ready :
                                                                                        mode_push::unavailable;
    }

    /**
     * Re-read Mutter's state until it reports @p connector at the requested
     * mode. Mutter applies asynchronously, so a single immediate read races
     * with the config it just accepted.
     */
    static bool mode_confirmed(const std::string &connector,
                               uint32_t width,
                               uint32_t height,
                               uint32_t refresh_mhz,
                               std::chrono::steady_clock::time_point deadline) {
      while (std::chrono::steady_clock::now() < deadline) {
        state_t state;
        std::string argument;
        switch (build_layout_with_mode(command_output(get_current_state_command),
                                       connector, width, height, refresh_mhz, state, argument)) {
          case mode_push::already_set:
            return true;
          case mode_push::unavailable:
            // An unreadable reply is not evidence that the mode was refused;
            // keep the session going rather than tearing it down over it.
            BOOST_LOG(debug) << "[VDISPLAY/Mutter] Could not read back the state of " << connector
                             << "; assuming the applied mode holds.";
            return true;
          default:
            std::this_thread::sleep_for(std::chrono::milliseconds {100});
            break;
        }
      }

      BOOST_LOG(warning) << "[VDISPLAY/Mutter] " << connector << " is not being driven at " << width << 'x'
                         << height << " after the config was applied; something else is rewriting the "
                            "display layout, and the stream will not match what the client asked for.";
      return false;
    }

    /**
     * Make Mutter drive @p connector at the geometry the streaming client asked
     * for.
     *
     * Mutter adopts a hotplugged connector on its own, but asynchronously and at
     * whichever mode it prefers — not necessarily the one Hermes requested. Two
     * things go wrong if we only look once and only look for presence:
     *
     *   - checked immediately after SET_OUTPUT, the connector is not in Mutter's
     *     state yet, and Hermes concludes the compositor refused it;
     *   - once it does appear, Mutter may be scanning out its own preferred mode
     *     while Hermes captures at the requested size, so the encoder publishes
     *     frames the compositor never rendered and the client sees black.
     *
     * So wait for the connector, then push the exact mode instead of hoping.
     *
     * @return true when Mutter drives the connector at the requested geometry.
     */
    static bool apply_output_mode(const std::string &connector,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t refresh_mhz,
                                  int deadline_ms) {
      if (!available() || connector.empty() || !width || !height) {
        return false;
      }

      // Mutter adopts a hotplugged connector asynchronously, so this waits.
      // A caller on a hot path - /resume answers a client that is counting the
      // seconds - passes a shorter budget than a launch does. Two thirds go to
      // waiting for the connector to appear and the rest to confirming the mode
      // stuck, so a short budget shortens both rather than spending everything
      // on the first half.
      const auto start = std::chrono::steady_clock::now();
      const auto appear_deadline = start + std::chrono::milliseconds {deadline_ms * 2 / 3};
      const auto confirm_budget = std::chrono::milliseconds {deadline_ms / 3};

      while (std::chrono::steady_clock::now() < appear_deadline) {
        state_t state;
        std::string argument;
        switch (build_layout_with_mode(command_output(get_current_state_command),
                                       connector, width, height, refresh_mhz, state, argument)) {
          case mode_push::unavailable:
            // Mutter has not processed the hotplug yet.
            std::this_thread::sleep_for(std::chrono::milliseconds {100});
            continue;
          case mode_push::unsupported:
            BOOST_LOG(warning) << "[VDISPLAY/Mutter] " << connector << " does not advertise " << width << "x" << height
                               << "; leaving Mutter's own choice in place.";
            return false;
          case mode_push::already_set:
            BOOST_LOG(info) << "[VDISPLAY/Mutter] " << connector << " is already driven at the requested mode.";
            return true;
          case mode_push::ready:
            break;
        }

        if (!run_apply(state, argument, apply_method::verify)) {
          return false;
        }

        BOOST_LOG(info) << "[VDISPLAY/Mutter] Driving " << connector << " at " << width << "x" << height << "@"
                        << (refresh_mhz / 1000) << "Hz for this session.";
        // Temporary rather than persistent: a streaming session must not
        // rewrite the user's saved layout in ~/.config/monitors.xml. Mutter
        // keeps a temporary config until something else replaces it; it does
        // not revert on its own.
        if (!run_apply(state, argument, apply_method::temporary)) {
          return false;
        }

        // ApplyMonitorsConfig returning cleanly is not the same as Mutter
        // scanning out the mode: it can be superseded by a hotplug or by
        // anything else that rewrites the layout while we are still setting up.
        // The capture path reports the real scanout, so an unconfirmed mode is
        // exactly the resolution mismatch that reaches the client as a broken
        // image. Confirm it, and say so when it did not stick.
        return mode_confirmed(connector, width, height, refresh_mhz,
                              std::chrono::steady_clock::now() + confirm_budget);
      }

      BOOST_LOG(warning) << "[VDISPLAY/Mutter] " << connector << " never appeared in Mutter's monitor state.";
      return false;
    }

    /**
     * Where @p connector sits on the desktop, for absolute pointer input.
     *
     * Mutter feeds an absolute pointing device the extents of the whole stage,
     * so a client's coordinates only land on the streamed output once they
     * carry that output's offset within the desktop envelope. Both are read
     * from the logical monitor layout, which is what the stage is measured in;
     * Mutter anchors that layout at the origin, so the envelope is simply the
     * far edge of the furthest monitor.
     *
     * The numbers are returned in the virtual output's own pixel space rather
     * than in logical pixels. The client's coordinates arrive in the mode's
     * pixels, and the consumer only ever forms the ratio
     * (offset + x) / envelope, so scaling offset and envelope by this monitor's
     * scale keeps that ratio exact while letting x stay in the units the
     * capture path already uses. At scale 1 - every desktop that does not use
     * fractional or integer scaling - this is the identity.
     *
     * @return true when the connector is in Mutter's layout and the envelope
     *         could be measured.
     */
    static bool geometry_from_state(const state_t &state,
                                    const std::string &connector,
                                    int &x, int &y,
                                    int &env_width, int &env_height) {
      if (connector.empty()) {
        return false;
      }

      const logical_monitor_t *owner = state.find_logical_monitor(connector);
      if (!owner) {
        BOOST_LOG(debug) << "[VDISPLAY/Mutter] " << connector
                         << " is not in the monitor layout; absolute input has no offset to apply.";
        return false;
      }

      double envelope_width = 0.0;
      double envelope_height = 0.0;
      for (const auto &lm : state.logical_monitors) {
        int lm_width = 0;
        int lm_height = 0;
        if (!state.logical_size(lm, lm_width, lm_height)) {
          return false;
        }
        envelope_width = std::max(envelope_width, static_cast<double>(std::atoi(lm.x.c_str()) + lm_width));
        envelope_height = std::max(envelope_height, static_cast<double>(std::atoi(lm.y.c_str()) + lm_height));
      }
      if (envelope_width <= 0.0 || envelope_height <= 0.0) {
        return false;
      }

      // Only the logical layout mode divides the mode by the scale; in the
      // physical one the layout already counts mode pixels and there is nothing
      // to convert.
      const double scale =
        state.layout_mode == layout_mode_e::logical ? state_t::scale_of(*owner) : 1.0;
      x = static_cast<int>(std::lround(std::atoi(owner->x.c_str()) * scale));
      y = static_cast<int>(std::lround(std::atoi(owner->y.c_str()) * scale));
      env_width = static_cast<int>(std::lround(envelope_width * scale));
      env_height = static_cast<int>(std::lround(envelope_height * scale));
      return env_width > 0 && env_height > 0;
    }

    static bool geometry(const std::string &connector, int &x, int &y, int &env_width, int &env_height) {
      if (!available() || connector.empty()) {
        return false;
      }

      state_t state;
      if (!parse_current_state(command_output(get_current_state_command), state)) {
        return false;
      }
      return geometry_from_state(state, connector, x, y, env_width, env_height);
    }

    // ------------------------------------------------------------------
    // Session layout: exclusive and mirror.
    //
    // ApplyMonitorsConfig takes the complete list of logical monitors, and
    // Mutter disables every connected monitor absent from that list
    // (create_disabled_monitor_specs_for_config). So exclusive mode is a config
    // carrying nothing but the virtual output, and there is no separate
    // "disable this output" call to look for.
    //
    // Mirror is not a position. Mutter refuses overlapping logical monitors, so
    // cloning means one logical monitor holding both connectors - and it
    // refuses that too unless their modes have identical dimensions, which is
    // why this can only mirror at a resolution the virtual output advertises.
    // ------------------------------------------------------------------

    struct layout_state_t {
      /// The config that puts the layout back as it was found, virtual output included.
      std::string restore_argument;
      /// Set once a session layout was applied and a restore is owed.
      bool active {false};
    };

    static std::mutex layouts_mutex;
    static std::map<std::string, layout_state_t> layouts;

    /**
     * Every character a config built by build_apply_argument() can contain.
     * The argument is interpolated into a gdbus command line, and one read back
     * from disk has not been through that builder, so it is checked before use.
     */
    static bool safe_apply_argument(const std::string &argument) {
      if (argument.empty() || argument.size() > 8192) {
        return false;
      }
      return std::all_of(argument.begin(), argument.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
               c == '\'' || c == ',' || c == ' ' || c == '.' || c == '-' || c == '_' || c == '@' ||
               c == '+' || c == ':';
      });
    }

    static std::string recovery_state_file() {
      const char *xdg_state = std::getenv("XDG_STATE_HOME");
      const char *home = std::getenv("HOME");
      std::string base;
      if (xdg_state && xdg_state[0]) {
        base = std::string {xdg_state} + "/hermes";
      } else if (home && home[0]) {
        base = std::string {home} + "/.local/state/hermes";
      } else {
        return {};
      }

      std::error_code ec;
      fs::create_directories(base, ec);
      return base + "/saved-mutter-layout";
    }

    /**
     * Persist the layout to fall back to if Hermes dies while the physical
     * monitors are off. It deliberately excludes the virtual connector: after a
     * crash the connector is usually gone, and a config naming a monitor Mutter
     * cannot find is rejected outright - which would leave the screens dark for
     * exactly the reason the file exists.
     */
    static void write_recovery_state(const std::string &reply, const std::string &virtual_connector) {
      state_t ignored;
      std::string argument;
      if (build_layout_without(reply, virtual_connector, ignored, argument) != layout_repair::ready ||
          !safe_apply_argument(argument)) {
        BOOST_LOG(warning) << "[VDISPLAY/Mutter] No physical-only layout to fall back on; a crash while the "
                              "monitors are off would leave them off.";
        return;
      }

      const auto path = recovery_state_file();
      if (path.empty()) {
        return;
      }
      const auto temporary_path = path + ".tmp";
      {
        std::ofstream file {temporary_path, std::ios::binary | std::ios::trunc};
        if (!file) {
          return;
        }
        file << argument << '\n';
      }
      std::error_code ec;
      fs::rename(temporary_path, path, ec);
      if (ec) {
        fs::remove(temporary_path, ec);
      }
    }

    static void clear_recovery_state() {
      const auto path = recovery_state_file();
      if (path.empty()) {
        return;
      }
      std::error_code ec;
      fs::remove(path, ec);
    }

    /** Submit @p argument against a freshly read serial. */
    static bool apply_argument(const std::string &argument) {
      if (!safe_apply_argument(argument)) {
        return false;
      }
      state_t state;
      if (!parse_current_state(command_output(get_current_state_command), state)) {
        BOOST_LOG(warning) << "[VDISPLAY/Mutter] Could not read the monitor state to apply a layout.";
        return false;
      }
      if (!run_apply(state, argument, apply_method::verify)) {
        return false;
      }
      return run_apply(state, argument, apply_method::temporary);
    }

    static void recover_on_startup() {
      if (!available()) {
        return;
      }
      const auto path = recovery_state_file();
      if (path.empty()) {
        return;
      }
      std::string argument;
      {
        std::ifstream file {path};
        if (!file) {
          return;
        }
        std::getline(file, argument);
      }
      if (argument.empty()) {
        clear_recovery_state();
        return;
      }
      if (!safe_apply_argument(argument)) {
        BOOST_LOG(warning) << "[VDISPLAY/Mutter] Discarding an unreadable saved layout.";
        clear_recovery_state();
        return;
      }

      state_t state;
      if (!parse_current_state(command_output(get_current_state_command), state)) {
        // Mutter did not answer; this is not evidence the layout is stale.
        BOOST_LOG(warning) << "[VDISPLAY/Mutter] Startup recovery deferred: Mutter did not answer.";
        return;
      }

      BOOST_LOG(info) << "[VDISPLAY/Mutter] Recovering the display layout a previous session left behind.";
      if (run_apply(state, argument, apply_method::verify) &&
          run_apply(state, argument, apply_method::temporary)) {
        clear_recovery_state();
        return;
      }
      // Mutter answered and refused: the layout no longer describes this
      // desktop, and Mutter has already configured something for it. Keeping
      // the file would retry the same rejection every start.
      BOOST_LOG(warning) << "[VDISPLAY/Mutter] The saved layout no longer applies to this desktop; discarding it.";
      clear_recovery_state();
    }

    enum class layout_change {
      unavailable,  ///< The reply did not parse, or the connector is not driven.
      unsupported,  ///< The change cannot be expressed on this desktop.
      already_set,  ///< The layout already is what was asked for.
      ready,        ///< `argument` holds the config to submit.
    };

    /** The config that reproduces @p state exactly, for undoing a session layout. */
    static bool build_restore_layout(const state_t &state, std::string &argument) {
      return build_apply_argument(state, state.logical_monitors, argument);
    }

    /**
     * The config that leaves @p connector alone on the desktop. Every other
     * connected monitor is disabled by being left out of the list, which is how
     * Mutter reads a config it is handed.
     */
    static layout_change build_exclusive_layout(const std::string &reply,
                                                const std::string &connector,
                                                state_t &state,
                                                std::string &argument) {
      state = {};
      argument.clear();
      if (reply.empty() || connector.empty() || !parse_current_state(reply, state)) {
        return layout_change::unavailable;
      }
      if (state.current_mode.find(connector) == state.current_mode.end()) {
        // Nothing is being scanned out on it, so there is no mode to name and
        // handing it the desktop would black the session out.
        return layout_change::unsupported;
      }

      const logical_monitor_t *owner = state.find_logical_monitor(connector);
      if (owner && state.logical_monitors.size() == 1 && owner->connectors.size() == 1) {
        return layout_change::already_set;
      }

      logical_monitor_t only;
      only.x = "0";
      only.y = "0";
      // Keep the scale Mutter already drives it at: one of our own can fail the
      // "mode divided by scale must be a whole number" check and take the
      // whole config with it.
      only.scale = owner ? owner->scale : std::string {"1.0"};
      only.transform = owner ? owner->transform : std::string {"0"};
      only.primary = "true";
      only.connectors.emplace_back(connector);

      return build_apply_argument(state, {only}, argument) ? layout_change::ready :
                                                             layout_change::unavailable;
    }

    /**
     * The config that clones the primary output onto @p connector by putting
     * both in one logical monitor - the only form of mirroring Mutter accepts,
     * and only at a mode size both advertise.
     */
    static layout_change build_mirror_layout(const std::string &reply,
                                             const std::string &connector,
                                             state_t &state,
                                             std::string &argument,
                                             int *mirrored_width = nullptr,
                                             int *mirrored_height = nullptr) {
      state = {};
      argument.clear();
      if (reply.empty() || connector.empty() || !parse_current_state(reply, state)) {
        return layout_change::unavailable;
      }

      // The monitor to clone: the primary one that is not ours, else the first.
      const logical_monitor_t *target = nullptr;
      for (const auto &lm : state.logical_monitors) {
        if (std::find(lm.connectors.begin(), lm.connectors.end(), connector) != lm.connectors.end()) {
          if (lm.connectors.size() > 1) {
            return layout_change::already_set;  // already sharing a logical monitor
          }
          continue;
        }
        if (!target || lm.primary == "true") {
          target = &lm;
        }
      }
      if (!target || target->connectors.empty()) {
        return layout_change::unsupported;
      }

      const auto target_mode_id = state.current_mode.find(target->connectors.front());
      const auto *target_monitor = state.find_monitor(target->connectors.front());
      if (target_mode_id == state.current_mode.end() || !target_monitor) {
        return layout_change::unavailable;
      }
      const mode_t *target_mode = nullptr;
      for (const auto &mode : target_monitor->modes) {
        if (mode.id == target_mode_id->second) {
          target_mode = &mode;
          break;
        }
      }
      if (!target_mode) {
        return layout_change::unavailable;
      }

      // Mutter refuses a logical monitor whose monitors do not share the mode
      // dimensions, so the virtual output has to advertise this exact size.
      const monitor_t *virtual_monitor = state.find_monitor(connector);
      if (!virtual_monitor) {
        return layout_change::unavailable;
      }
      const mode_t *chosen = nullptr;
      for (const auto &mode : virtual_monitor->modes) {
        if (mode.width != target_mode->width || mode.height != target_mode->height) {
          continue;
        }
        if (!chosen ||
            std::abs(mode.refresh - target_mode->refresh) < std::abs(chosen->refresh - target_mode->refresh)) {
          chosen = &mode;
        }
      }
      if (!chosen) {
        return layout_change::unsupported;
      }

      if (mirrored_width) {
        *mirrored_width = target_mode->width;
      }
      if (mirrored_height) {
        *mirrored_height = target_mode->height;
      }

      // Take the connector out of wherever it sits and add it to the target,
      // dropping a logical monitor that held nothing else. The target is
      // identified by its leading connector, which the virtual one is not.
      const std::string target_connector = target->connectors.front();
      std::vector<logical_monitor_t> layout;
      bool mirrored = false;
      for (auto lm : state.logical_monitors) {
        lm.connectors.erase(
          std::remove(lm.connectors.begin(), lm.connectors.end(), connector),
          lm.connectors.end()
        );
        if (lm.connectors.empty()) {
          continue;
        }
        if (lm.connectors.front() == target_connector) {
          lm.connectors.emplace_back(connector);
          mirrored = true;
        }
        layout.emplace_back(std::move(lm));
      }
      if (layout.empty() || !mirrored) {
        return layout_change::unavailable;
      }

      return build_apply_argument(state, layout, argument, {{connector, chosen->id}}) ?
               layout_change::ready :
               layout_change::unavailable;
    }

    /**
     * Hand the desktop to @p connector for the session: a config carrying only
     * its logical monitor, which disables every physical output by omission.
     *
     * Called at SESSION START, never at display creation - the local screens
     * must not go dark before a stream is live.
     */
    static bool make_exclusive(const std::string &display_name, const std::string &connector) {
      if (!available() || connector.empty()) {
        return false;
      }

      const auto reply = command_output(get_current_state_command);
      state_t state;
      std::string argument;
      switch (build_exclusive_layout(reply, connector, state, argument)) {
        case layout_change::already_set:
          BOOST_LOG(info) << "[VDISPLAY/Mutter] " << connector << " already has the desktop to itself.";
          return true;
        case layout_change::unsupported:
          BOOST_LOG(warning) << "[VDISPLAY/Mutter] " << connector
                             << " is not being driven; refusing to hand it the desktop.";
          return false;
        case layout_change::unavailable:
          BOOST_LOG(warning) << "[VDISPLAY/Mutter] Could not read the monitor state; leaving the layout alone.";
          return false;
        case layout_change::ready:
          break;
      }

      std::string restore_argument;
      if (!build_restore_layout(state, restore_argument)) {
        BOOST_LOG(warning) << "[VDISPLAY/Mutter] Refusing exclusive mode: the current layout could not be "
                              "captured, so it could not be put back.";
        return false;
      }

      // Written before the monitors go dark, so a crash between the two leaves
      // something for the next start to work from.
      write_recovery_state(reply, connector);

      if (!run_apply(state, argument, apply_method::verify) ||
          !run_apply(state, argument, apply_method::temporary)) {
        BOOST_LOG(warning) << "[VDISPLAY/Mutter] GNOME refused the exclusive layout; the physical outputs stay on.";
        clear_recovery_state();
        return false;
      }

      {
        std::lock_guard<std::mutex> lock(layouts_mutex);
        layouts[display_name] = {.restore_argument = std::move(restore_argument), .active = true};
      }
      BOOST_LOG(info) << "[VDISPLAY/Mutter] " << connector << " has the desktop for this session.";
      return true;
    }

    /**
     * Clone the primary output onto @p connector by putting both in one logical
     * monitor, which is the only thing Mutter accepts as mirroring.
     *
     * The shared logical monitor takes the primary's mode size, so this only
     * works at a resolution @p connector advertises - and the stream then runs
     * at that resolution rather than the one the client negotiated.
     */
    static bool apply_mirror(const std::string &display_name, const std::string &connector) {
      if (!available() || connector.empty()) {
        return false;
      }

      state_t state;
      std::string argument;
      int mirrored_width = 0;
      int mirrored_height = 0;
      switch (build_mirror_layout(
        command_output(get_current_state_command), connector, state, argument, &mirrored_width, &mirrored_height
      )) {
        case layout_change::already_set:
          return true;
        case layout_change::unsupported:
          BOOST_LOG(warning) << "[VDISPLAY/Mutter] Cannot mirror onto " << connector
                             << ": GNOME clones by putting both outputs in one logical monitor, which requires "
                                "them to advertise the same mode size. The session keeps the extended layout.";
          return false;
        case layout_change::unavailable:
          BOOST_LOG(warning) << "[VDISPLAY/Mutter] Could not read the monitor state; not mirroring.";
          return false;
        case layout_change::ready:
          break;
      }

      std::string restore_argument;
      if (!build_restore_layout(state, restore_argument)) {
        return false;
      }

      if (!run_apply(state, argument, apply_method::verify) ||
          !run_apply(state, argument, apply_method::temporary)) {
        BOOST_LOG(warning) << "[VDISPLAY/Mutter] GNOME refused the mirrored layout; the outputs stay as they were.";
        return false;
      }

      {
        std::lock_guard<std::mutex> lock(layouts_mutex);
        layouts[display_name] = {.restore_argument = std::move(restore_argument), .active = true};
      }
      BOOST_LOG(info) << "[VDISPLAY/Mutter] Mirroring onto " << connector << " at " << mirrored_width << 'x'
                      << mirrored_height << "; the stream runs at that resolution, not the one the client asked for.";
      return true;
    }

    // ------------------------------------------------------------------
    // Input device mapping.
    //
    // Mutter measures an absolute *pointer* against the whole desktop, but a
    // touchscreen or a tablet is bound to one monitor by MetaInputMapper. With
    // nothing configured it guesses - by EDID, by the device's physical size,
    // and failing both a touchscreen falls back to the built-in panel. On a
    // laptop that pins the client's touches to the laptop screen; on a desktop
    // with no built-in panel nothing matches and the device stretches over the
    // whole desktop. Neither is the streamed output.
    //
    // The deterministic control is the per-device "output" key: a list of at
    // least three strings compared against the monitor's EDID vendor, product
    // and serial (meta-input-mapper.c, match_config). Hermes owns the vendor
    // and product ids of its virtual devices, so it can write the key for
    // exactly its own and leave every real device alone.
    //
    // A mapping left behind by a crash is harmless: it names a monitor that no
    // longer exists, no monitor matches it, and the device falls back to what
    // it would have done anyway. That is why there is no recovery file here,
    // unlike the layout.
    //
    // The binding is per vendor:product, and every Hermes device carries the
    // same pair, so it is one binding for the host rather than one per session.
    // With concurrent sessions it therefore points at whichever output was
    // activated last. That is a limit of how GNOME addresses devices, not a
    // choice made here - and it is still an improvement on the guess, which is
    // wrong for every session.
    // ------------------------------------------------------------------

    struct device_settings_t {
      const char *schema;
      const char *group;
    };

    /// Touch and pen land in different schemas: Mutter groups touchscreens
    /// separately from the tablet family, which the pen belongs to.
    static constexpr device_settings_t touch_settings {
      "org.gnome.desktop.peripherals.touchscreen", "touchscreens"
    };
    static constexpr device_settings_t pen_settings {
      "org.gnome.desktop.peripherals.tablet", "tablets"
    };

    /// `<schema>:<path>`, the form gsettings takes for a relocatable schema.
    static std::string device_settings_target(const device_settings_t &device) {
      char ids[16] = {};
      // Mutter formats the path with %.4x, so lowercase hex, at least 4 digits.
      std::snprintf(ids, sizeof(ids), "%.4x:%.4x", VIRTUAL_INPUT_SETTINGS_VENDOR_ID, VIRTUAL_INPUT_SETTINGS_PRODUCT_ID);
      return std::string {device.schema} + ":/org/gnome/desktop/peripherals/" + device.group + "/" + ids + "/";
    }

    /**
     * EDID strings come from the monitor, so they reach a command line only
     * after being checked. A quote or a backslash is refused rather than
     * escaped: a monitor whose name needs escaping is not worth the risk of
     * getting the escaping wrong, and the cost of refusing is the fallback
     * behaviour we already had.
     */
    static bool safe_edid_field(const std::string &value) {
      return value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == ':' ||
               c == '+' || c == '/' || c == '(' || c == ')' || c == ',';
      });
    }

    /**
     * The `output` value that binds a device to @p connector, or empty when the
     * monitor's identity cannot be spelled safely.
     *
     * The connector is appended as a fourth element, which Mutter uses to
     * disambiguate two monitors sharing an EDID.
     */
    static std::string device_output_value(const state_t &state, const std::string &connector) {
      const monitor_t *monitor = state.find_monitor(connector);
      if (!monitor) {
        return {};
      }
      if (!safe_edid_field(monitor->vendor) || !safe_edid_field(monitor->product) ||
          !safe_edid_field(monitor->serial) || !safe_variant_token(monitor->connector)) {
        return {};
      }
      // A binding whose three EDID fields are all empty is read as "not
      // configured", so it would bind nothing while still counting as a user
      // value - the worst of both.
      if (monitor->vendor.empty() && monitor->product.empty() && monitor->serial.empty()) {
        return {};
      }
      return "['" + monitor->vendor + "', '" + monitor->product + "', '" + monitor->serial + "', '" +
             monitor->connector + "']";
    }

    struct input_mapping_t {
      bool active {false};
      /// What the key held before, so it can be put back rather than reset when
      /// the user had configured one.
      std::string previous_touch;
      std::string previous_pen;
    };

    static std::mutex input_mapping_mutex;
    static input_mapping_t input_mapping;

    static std::string read_device_setting(const device_settings_t &device) {
      const std::string command = "gsettings get " + device_settings_target(device) + " output 2>/dev/null";
      return trim_variant(command_output(command.c_str()));
    }

    static bool write_device_setting(const device_settings_t &device, const std::string &value) {
      const std::string command = "gsettings set " + device_settings_target(device) + " output \"" + value +
                                  "\" 2>/dev/null";
      return std::system(command.c_str()) == 0;
    }

    static void reset_device_setting(const device_settings_t &device) {
      const std::string command = "gsettings reset " + device_settings_target(device) + " output 2>/dev/null";
      (void) std::system(command.c_str());
    }

    /** The unset value of the key, which restore turns back into a reset. */
    static bool is_unset_output_value(const std::string &value) {
      return value.empty() || value == "['', '', '']" || value == "@as []" || value == "[]";
    }

    /**
     * Bind the touch and pen devices to @p connector for the session.
     *
     * Best effort by design: a failure costs the session its touch placement,
     * not its stream, so it is logged and the stream goes on.
     */
    static void map_input_devices(const std::string &connector) {
      if (!available() || connector.empty()) {
        return;
      }

      state_t state;
      if (!parse_current_state(command_output(get_current_state_command), state)) {
        return;
      }
      const auto value = device_output_value(state, connector);
      if (value.empty()) {
        BOOST_LOG(warning) << "[VDISPLAY/Mutter] Cannot bind touch input to " << connector
                           << ": its monitor identity is missing or cannot be spelled safely. Touches will "
                              "land wherever GNOME guesses.";
        return;
      }

      std::lock_guard<std::mutex> lock(input_mapping_mutex);
      if (!input_mapping.active) {
        input_mapping.previous_touch = read_device_setting(touch_settings);
        input_mapping.previous_pen = read_device_setting(pen_settings);
      }
      const bool touch_ok = write_device_setting(touch_settings, value);
      const bool pen_ok = write_device_setting(pen_settings, value);
      if (!touch_ok && !pen_ok) {
        BOOST_LOG(warning) << "[VDISPLAY/Mutter] Could not bind the virtual touch devices to " << connector
                           << "; is gsettings available in this session?";
        return;
      }
      input_mapping.active = true;
      BOOST_LOG(info) << "[VDISPLAY/Mutter] Touch and pen input bound to " << connector << '.';
    }

    /** Undo map_input_devices(), putting a user's own binding back if there was one. */
    static void unmap_input_devices() {
      std::lock_guard<std::mutex> lock(input_mapping_mutex);
      if (!input_mapping.active) {
        return;
      }
      const auto put_back = [](const device_settings_t &device, const std::string &previous) {
        // A value read back from the store has not been through our builder, so
        // anything that could not have come out of it is reset rather than
        // written to a command line.
        if (is_unset_output_value(previous) || !safe_apply_argument(previous)) {
          reset_device_setting(device);
        } else {
          write_device_setting(device, previous);
        }
      };
      put_back(touch_settings, input_mapping.previous_touch);
      put_back(pen_settings, input_mapping.previous_pen);
      input_mapping = {};
      BOOST_LOG(info) << "[VDISPLAY/Mutter] Released the touch and pen input binding.";
    }

    /** Put back the layout that was captured before the session changed it. */
    static void restore(const std::string &display_name) {
      std::string argument;
      {
        std::lock_guard<std::mutex> lock(layouts_mutex);
        const auto it = layouts.find(display_name);
        if (it == layouts.end() || !it->second.active) {
          return;
        }
        argument = it->second.restore_argument;
        layouts.erase(it);
      }

      if (apply_argument(argument)) {
        clear_recovery_state();
        BOOST_LOG(info) << "[VDISPLAY/Mutter] Restored the display layout the session found.";
        return;
      }
      BOOST_LOG(warning) << "[VDISPLAY/Mutter] Could not restore the display layout; keeping the recovery state "
                            "so the next start can.";
    }

    // ------------------------------------------------------------------
    // Watching the layout.
    //
    // Everything above reads GetCurrentState once and acts on the answer. That
    // answer has a shelf life: GNOME rewrites the layout for reasons that have
    // nothing to do with streaming - another monitor is plugged in, the
    // Displays panel is opened, a stored configuration is reapplied after a
    // resume - and any of those can move our output, change its mode, or
    // replace the temporary configuration a session applied. The offsets the
    // capture path computed at session start are then quietly wrong, and the
    // pointer goes back to landing in the wrong place with nothing to show for
    // it.
    //
    // MonitorsChanged is the compositor telling us exactly that. It costs
    // nothing until it fires, unlike re-reading the state on a timer, which is
    // a subprocess per tick for a whole session.
    //
    // What this deliberately does NOT do is put the layout back. If the mode or
    // the layout changed under us, something - very likely the user - wanted
    // it that way, and a session that keeps overwriting the display settings
    // while someone is trying to change them is worse than one that follows.
    // So it records the change and lets the capture path pick the new geometry
    // up; the resolution the client sees follows the compositor.
    // ------------------------------------------------------------------

    /// Bumped whenever the desktop layout changes in a way that moves outputs
    /// or changes their modes. A capture that started at an older value has
    /// stale geometry and reinitialises.
    static std::atomic<uint64_t> layout_generation {1};

#ifdef SUNSHINE_BUILD_SDBUS
    struct watcher_t {
      sd_bus *bus {nullptr};
      sd_bus_slot *slot {nullptr};
      std::thread thread;
      std::atomic<bool> stop {false};
      /// The layout as last seen, in the form a restore would submit: it spells
      /// out every monitor's position, scale, transform and current mode, so
      /// comparing it catches every change that matters here and ignores the
      /// ones that do not (a monitor's backlight, a privacy screen).
      std::string signature;
    };

    static std::mutex watcher_mutex;
    static std::unique_ptr<watcher_t> watcher;

    /// Read the current layout signature; empty when it cannot be read.
    static std::string layout_signature() {
      state_t state;
      std::string signature;
      if (!parse_current_state(command_output(get_current_state_command), state) ||
          !build_restore_layout(state, signature)) {
        return {};
      }
      return signature;
    }

    static int on_monitors_changed(sd_bus_message * /* message */, void *userdata, sd_bus_error * /* error */) {
      auto *state = static_cast<watcher_t *>(userdata);
      const auto signature = layout_signature();
      if (signature.empty() || signature == state->signature) {
        // Unreadable, or a change that does not touch any geometry we use.
        return 0;
      }
      state->signature = signature;
      layout_generation.fetch_add(1, std::memory_order_relaxed);
      BOOST_LOG(info) << "[VDISPLAY/Mutter] The desktop layout changed; capture will pick up the new geometry.";
      return 0;
    }

    /** Start watching, replacing any previous watch. Best effort. */
    static void watch_layout() {
      if (!available()) {
        return;
      }

      std::lock_guard<std::mutex> lock(watcher_mutex);
      if (watcher) {
        return;  // already watching; one subscription serves every display
      }

      auto started = std::make_unique<watcher_t>();
      if (sd_bus_open_user(&started->bus) < 0 || !started->bus) {
        BOOST_LOG(debug) << "[VDISPLAY/Mutter] No session bus to watch for layout changes.";
        return;
      }
      started->signature = layout_signature();

      const int matched = sd_bus_match_signal(
        started->bus,
        &started->slot,
        "org.gnome.Mutter.DisplayConfig",
        "/org/gnome/Mutter/DisplayConfig",
        "org.gnome.Mutter.DisplayConfig",
        "MonitorsChanged",
        on_monitors_changed,
        started.get()
      );
      if (matched < 0) {
        BOOST_LOG(warning) << "[VDISPLAY/Mutter] Could not subscribe to MonitorsChanged: "
                           << std::strerror(-matched)
                           << ". A layout change mid-session will go unnoticed.";
        sd_bus_unref(started->bus);
        return;
      }

      watcher_t *raw = started.get();
      started->thread = std::thread([raw]() {
        while (!raw->stop.load(std::memory_order_relaxed)) {
          const int processed = sd_bus_process(raw->bus, nullptr);
          if (processed < 0) {
            break;
          }
          if (processed > 0) {
            continue;  // more may be queued
          }
          // A bounded wait rather than an indefinite one, so stopping does not
          // need a second wakeup channel.
          if (sd_bus_wait(raw->bus, 250000 /* us */) < 0) {
            break;
          }
        }
      });
      watcher = std::move(started);
      BOOST_LOG(info) << "[VDISPLAY/Mutter] Watching the desktop layout for changes.";
    }

    static void unwatch_layout() {
      std::unique_ptr<watcher_t> stopping;
      {
        std::lock_guard<std::mutex> lock(watcher_mutex);
        stopping = std::move(watcher);
      }
      if (!stopping) {
        return;
      }
      stopping->stop.store(true, std::memory_order_relaxed);
      if (stopping->thread.joinable()) {
        stopping->thread.join();
      }
      if (stopping->slot) {
        sd_bus_slot_unref(stopping->slot);
      }
      if (stopping->bus) {
        sd_bus_unref(stopping->bus);
      }
    }
#else
    // Without sd-bus there is no subscription to hold, so the generation never
    // moves and every capture keeps the geometry it started with.
    static void watch_layout() {}

    static void unwatch_layout() {}
#endif
  }  // namespace mutter

  EvdiBuffer::EvdiBuffer(uint32_t width, uint32_t height):
      data_(static_cast<size_t>(width) * height * 4, 0), width_(width), height_(height) {
  }

  uint64_t EvdiBuffer::copy_to(uint8_t *dst, uint32_t dst_stride) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto row_bytes = stride();
    if (dst_stride == row_bytes) {
      std::memcpy(dst, data_.data(), static_cast<size_t>(row_bytes) * height_);
    } else {
      const auto copy_bytes = std::min(dst_stride, row_bytes);
      for (uint32_t row = 0; row < height_; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * dst_stride,
                    data_.data() + static_cast<size_t>(row) * row_bytes, copy_bytes);
      }
    }
    return frame_number();
  }

  uint64_t EvdiBuffer::wait_for_update(uint64_t last_frame, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait_for(lock, timeout, [&] { return frame_number() > last_frame; });
    return frame_number();
  }

  void EvdiBuffer::begin_write() {
    mutex_.lock();
  }

  void EvdiBuffer::end_write() {
    mutex_.unlock();
  }

  void EvdiBuffer::mark_updated() {
    frame_number_.fetch_add(1, std::memory_order_release);
    condition_.notify_all();
  }

  // ============================================================================
  // EVDI Library Loading
  // ============================================================================

  static bool load_evdi_library() {
    if (evdi.loaded) {
      return true;
    }

    // Try to load libevdi.so
    const char *lib_names[] = {
      "libevdi.so.1",
      "libevdi.so",
      "/usr/lib/libevdi.so.1",
      "/usr/lib/libevdi.so",
      "/usr/local/lib/libevdi.so.1",
      "/usr/local/lib/libevdi.so"
    };

    for (const auto &lib_name : lib_names) {
      evdi.lib_handle = dlopen(lib_name, RTLD_NOW);
      if (evdi.lib_handle) {
        BOOST_LOG(info) << "[VDISPLAY] Loaded EVDI library: " << lib_name;
        break;
      }
    }

    if (!evdi.lib_handle) {
      BOOST_LOG(warning) << "[VDISPLAY] Could not load libevdi.so: " << dlerror();
      BOOST_LOG(warning) << "[VDISPLAY] Virtual-display sessions will be unavailable.";
      BOOST_LOG(warning) << "[VDISPLAY] Install the EVDI userspace library and kernel module.";
      return false;
    }

    // Load function pointers
    #define LOAD_EVDI_FUNC(name) \
      evdi.name = (fn_evdi_##name)dlsym(evdi.lib_handle, "evdi_" #name); \
      if (!evdi.name) { \
        BOOST_LOG(error) << "[VDISPLAY] Failed to load evdi_" #name; \
        dlclose(evdi.lib_handle); \
        evdi.lib_handle = nullptr; \
        return false; \
      }

    LOAD_EVDI_FUNC(check_device);
    LOAD_EVDI_FUNC(open);
    LOAD_EVDI_FUNC(add_device);
    LOAD_EVDI_FUNC(close);
    LOAD_EVDI_FUNC(connect);
    LOAD_EVDI_FUNC(disconnect);
    LOAD_EVDI_FUNC(grab_pixels);
    LOAD_EVDI_FUNC(register_buffer);
    LOAD_EVDI_FUNC(unregister_buffer);
    LOAD_EVDI_FUNC(request_update);
    LOAD_EVDI_FUNC(handle_events);
    LOAD_EVDI_FUNC(get_event_ready);
    LOAD_EVDI_FUNC(get_lib_version);

    #undef LOAD_EVDI_FUNC

    // Check library version
    evdi_lib_version version;
    evdi.get_lib_version(&version);
    BOOST_LOG(info) << "[VDISPLAY] EVDI library version: "
                    << version.version_major << "."
                    << version.version_minor << "."
                    << version.version_patchlevel;
    evdi_library_version = std::to_string(version.version_major) + "." +
                           std::to_string(version.version_minor) + "." +
                           std::to_string(version.version_patchlevel);

    evdi.loaded = true;
    return true;
  }

  static void unload_evdi_library() {
    if (evdi.lib_handle) {
      dlclose(evdi.lib_handle);
      evdi.lib_handle = nullptr;
    }
    evdi.loaded = false;
    evdi_library_version.clear();
  }

  // ============================================================================
  // EVDI Module Check
  // ============================================================================

  static bool check_evdi_module_loaded(bool log_result = true) {
    // Check if evdi kernel module is loaded
    std::ifstream modules("/proc/modules");
    std::string line;
    while (std::getline(modules, line)) {
      if (line.find("evdi") != std::string::npos) {
        if (log_result) {
          BOOST_LOG(info) << "[VDISPLAY] EVDI kernel module is loaded.";
        }
        return true;
      }
    }

    // Also check sysfs
    if (fs::exists("/sys/module/evdi")) {
      if (log_result) {
        BOOST_LOG(info) << "[VDISPLAY] EVDI kernel module detected via sysfs.";
      }
      return true;
    }

    if (log_result) {
      BOOST_LOG(warning) << "[VDISPLAY] EVDI kernel module is not loaded.";
      BOOST_LOG(warning) << "[VDISPLAY] Try: sudo modprobe evdi";
    }
    return false;
  }

  static bool evdi_library_installed() {
    constexpr std::array<const char *, 6> library_paths {
      "/usr/lib/libevdi.so.1",
      "/usr/lib/libevdi.so",
      "/usr/local/lib/libevdi.so.1",
      "/usr/local/lib/libevdi.so",
      "/lib/libevdi.so.1",
      "/lib/libevdi.so",
    };

    return std::any_of(library_paths.begin(), library_paths.end(), [](const auto *path) {
      return fs::exists(path);
    });
  }

  static bool evdi_module_installed_for_running_kernel() {
    utsname system_info {};
    if (uname(&system_info) != 0) {
      return false;
    }

    const fs::path module_root = fs::path("/lib/modules") / system_info.release;
    constexpr std::array<const char *, 4> module_extensions {"", ".zst", ".xz", ".gz"};
    for (const auto *extension : module_extensions) {
      if (fs::exists(module_root / "updates/dkms" / (std::string("evdi.ko") + extension)) ||
          fs::exists(module_root / "kernel/drivers/gpu/drm/evdi" / (std::string("evdi.ko") + extension))) {
        return true;
      }
    }

    return false;
  }

  static bool evdi_dkms_build_failed() {
    const fs::path dkms_root = "/var/lib/dkms/evdi";
    std::error_code ec;
    if (!fs::exists(dkms_root, ec)) {
      return false;
    }

    utsname system_info {};
    if (uname(&system_info) != 0) {
      return false;
    }

    const std::string kernel_marker = std::string("for kernel ") + system_info.release;
    for (fs::recursive_directory_iterator it(dkms_root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end;
         it.increment(ec)) {
      if (it->path().filename() != "make.log") {
        continue;
      }

      std::ifstream log_file(it->path());
      std::string log_contents((std::istreambuf_iterator<char>(log_file)), std::istreambuf_iterator<char>());
      if (log_contents.find(kernel_marker) != std::string::npos &&
          log_contents.find("# exit code: 0") == std::string::npos &&
          (log_contents.find(" error:") != std::string::npos || log_contents.find("Error ") != std::string::npos)) {
        return true;
      }
    }

    return false;
  }

  static std::string running_kernel_release() {
    utsname system_info {};
    return uname(&system_info) == 0 ? system_info.release : "unknown";
  }

  static std::vector<std::string> dkms_kernels(const std::string &dkms_name) {
    std::vector<std::string> kernels;
    const fs::path dkms_root = fs::path("/var/lib/dkms") / dkms_name;
    std::error_code ec;
    for (const auto &version : fs::directory_iterator(dkms_root, fs::directory_options::skip_permission_denied, ec)) {
      if (ec || !version.is_directory()) {
        continue;
      }
      for (const auto &kernel : fs::directory_iterator(version.path(), fs::directory_options::skip_permission_denied, ec)) {
        if (ec || !kernel.is_directory()) {
          continue;
        }
        bool module_present = false;
        for (const auto &architecture : fs::directory_iterator(kernel.path(), fs::directory_options::skip_permission_denied, ec)) {
          if (architecture.is_directory() && fs::exists(architecture.path() / "module")) {
            module_present = true;
            break;
          }
        }
        if (module_present) {
          kernels.emplace_back(version.path().filename().string() + " / " + kernel.path().filename().string());
        }
      }
    }
    return kernels;
  }

  bool needsInitialDeviceConfiguration() {
    constexpr auto count_path = "/sys/devices/evdi/count";
    constexpr auto add_path = "/sys/devices/evdi/add";

    std::ifstream count_file(count_path);
    int device_count = -1;
    count_file >> device_count;

    return device_count == 0 && access(add_path, W_OK) != 0;
  }

  // Generic kernel-module helpers, parameterized by module name. The EVDI
  // helpers above predate the Hermes-KMS backend and stay specialized; these
  // mirror their logic so the Hermes-KMS diagnostics reach the same fidelity.
  static bool kernel_module_loaded(const std::string &module_name) {
    std::ifstream modules("/proc/modules");
    std::string line;
    while (std::getline(modules, line)) {
      // /proc/modules lines start with the module name followed by a space.
      if (line.rfind(module_name + " ", 0) == 0) {
        return true;
      }
    }
    return fs::exists(fs::path("/sys/module") / module_name);
  }

  static bool dkms_module_installed_for_running_kernel(const std::string &module_file) {
    utsname system_info {};
    if (uname(&system_info) != 0) {
      return false;
    }

    const fs::path module_root = fs::path("/lib/modules") / system_info.release;
    constexpr std::array<const char *, 4> module_extensions {"", ".zst", ".xz", ".gz"};
    for (const auto *extension : module_extensions) {
      if (fs::exists(module_root / "updates/dkms" / (module_file + extension))) {
        return true;
      }
    }
    return false;
  }

  static bool dkms_build_failed(const std::string &dkms_name) {
    const fs::path dkms_root = fs::path("/var/lib/dkms") / dkms_name;
    std::error_code ec;
    if (!fs::exists(dkms_root, ec)) {
      return false;
    }

    utsname system_info {};
    if (uname(&system_info) != 0) {
      return false;
    }

    const std::string kernel_marker = std::string("for kernel ") + system_info.release;
    for (fs::recursive_directory_iterator it(dkms_root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end;
         it.increment(ec)) {
      if (it->path().filename() != "make.log") {
        continue;
      }

      std::ifstream log_file(it->path());
      std::string log_contents((std::istreambuf_iterator<char>(log_file)), std::istreambuf_iterator<char>());
      if (log_contents.find(kernel_marker) != std::string::npos &&
          log_contents.find("# exit code: 0") == std::string::npos &&
          (log_contents.find(" error:") != std::string::npos || log_contents.find("Error ") != std::string::npos)) {
        return true;
      }
    }

    return false;
  }

  EVDI_DIAGNOSTIC getEvdiDiagnostic() {
    if (!evdi_library_installed()) {
      return EVDI_DIAGNOSTIC::LIBRARY_MISSING;
    }

    if (!check_evdi_module_loaded(false)) {
      if (evdi_dkms_build_failed()) {
        return EVDI_DIAGNOSTIC::DKMS_BUILD_FAILED;
      }

      if (!evdi_module_installed_for_running_kernel()) {
        return EVDI_DIAGNOSTIC::MODULE_NOT_INSTALLED;
      }

      return EVDI_DIAGNOSTIC::MODULE_NOT_LOADED;
    }

    if (needsInitialDeviceConfiguration()) {
      return EVDI_DIAGNOSTIC::INITIAL_DEVICE_CONFIGURATION_REQUIRED;
    }

    return EVDI_DIAGNOSTIC::READY;
  }

  SessionFacts probeSessionFacts() {
    SessionFacts facts;
    facts.compositor = sessionCompositor();
    facts.hyprland_control = facts.compositor == compositor_e::hyprland && hyprland::available();
    facts.wayland = window_system == window_system_e::WAYLAND;
    facts.x11 = window_system == window_system_e::X11;

#ifdef SUNSHINE_BUILD_WAYLAND
    if (facts.wayland) {
      const auto protocols = wl::probe_protocols();
      facts.output_management = protocols.output_management;
      facts.screencopy = protocols.screencopy;
      facts.image_copy_capture = protocols.image_copy_capture;
      facts.linux_dmabuf = protocols.linux_dmabuf;
      facts.kscreen = kscreen::available();
      facts.mutter = mutter::available();
    }
#endif

    const auto hermes_kms = getHermesKmsStatus();
    facts.hermes_kms_present = hermes_kms.device_present;
    facts.hermes_kms_multi_output = hermes_kms.multi_output_capable;
    facts.hermes_kms_multi_device = hermes_kms.multi_device_capable;
    facts.drm_seat_isolation = hermesKmsSeatIsolationActive();
    facts.isolated_sessions_requested = config::video.hermes_kms_isolated_sessions;

    return facts;
  }

  void logSessionAssessment() {
    const auto facts = probeSessionFacts();
    BOOST_LOG(info) << "[VDISPLAY] Session: " << compositorName(facts.compositor)
                    << (facts.wayland ? " (Wayland)" : facts.x11 ? " (X11)" : " (no window system)");
    for (const auto &report : assessSession(facts)) {
      // A feature that works is one line; one that does not carries the reason
      // and the fix, because a user who reads only the log is the user this
      // exists for.
      if (report.readiness == readiness_e::ready) {
        BOOST_LOG(info) << "[VDISPLAY]   " << featureName(report.feature) << ": ready - " << report.detail;
        continue;
      }
      std::string message = "[VDISPLAY]   " + featureName(report.feature) + ": " +
                            readinessName(report.readiness) + " - " + report.detail;
      if (!report.remediation.empty()) {
        message += " Fix: " + report.remediation;
      }
      BOOST_LOG(warning) << message;
    }
  }

  EvdiStatus getEvdiStatus() {
    const std::string session_type = window_system == window_system_e::X11 ? "x11" :
                                     window_system == window_system_e::WAYLAND ? "wayland" : "unknown";
    bool exclusive_layout_supported = window_system == window_system_e::X11;
    std::string output_layout_backend = exclusive_layout_supported ? "xrandr" : "unavailable";
#ifdef SUNSHINE_BUILD_WAYLAND
    if (window_system == window_system_e::WAYLAND && kscreen::available()) {
      exclusive_layout_supported = true;
      output_layout_backend = "kscreen-doctor";
    }
#endif
#ifdef SUNSHINE_BUILD_WAYLAND
    if (window_system == window_system_e::WAYLAND) {
      if (!exclusive_layout_supported && wl::output_management_supported()) {
        exclusive_layout_supported = true;
        // Name the compositor: "wlr-output-management" is the protocol, not a
        // strategy, and the compositor behind it is what decides whether a
        // Hermes-KMS output is ever composited onto. A bug report that says
        // Hyprland is a different report from one that says sway.
        output_layout_backend = "wlr-output-management";
        if (const auto compositor = sessionCompositor(); compositor != compositor_e::unknown) {
          output_layout_backend += " (" + compositorName(compositor) + ")";
        }
      }
    }
#endif
    // GNOME/Mutter drives the output through ApplyMonitorsConfig, which takes
    // the whole layout: a config naming only the virtual output turns the
    // physical ones off, so exclusive mode is available here too.
    if (window_system == window_system_e::WAYLAND && output_layout_backend == "unavailable" &&
        mutter::available()) {
      output_layout_backend = "mutter-displayconfig";
      exclusive_layout_supported = true;
    }
    EvdiStatus status {
      .diagnostic = getEvdiDiagnostic(),
      .library_installed = evdi_library_installed(),
      .library_loaded = evdi.loaded,
      .module_loaded = check_evdi_module_loaded(false),
      .module_installed = evdi_module_installed_for_running_kernel(),
      .device_count = -1,
      .session_type = session_type,
      .exclusive_layout_supported = exclusive_layout_supported,
      .output_layout_backend = output_layout_backend,
      .capture_fallback_active = virtual_display_capture_fallback_active.load(),
      .library_version = evdi_library_version,
      .running_kernel = running_kernel_release(),
      .dkms_kernels = dkms_kernels("evdi"),
      .active_displays = {},
    };

    std::ifstream count_file("/sys/devices/evdi/count");
    count_file >> status.device_count;

    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, display] : virtual_displays) {
      if (display.using_evdi && display.active) {
        status.active_displays.push_back({
          .name = display.name,
          .client_name = display.client_name,
          .device_index = display.device_index,
          .output_index = -1,
          .drm_card_index = display.drm_card_index,
          .width = display.width,
          .height = display.height,
          .fps = display.fps / 1000,
          .frame_updates = display.evdi_buffer ? display.evdi_buffer->frame_number() : 0,
        });
      }
    }

    return status;
  }

  HERMES_KMS_DIAGNOSTIC getHermesKmsDiagnostic() {
    const auto probe = hermes_kms::probe();
    switch (probe.result) {
      case hermes_kms::probe_result::ok:
        return HERMES_KMS_DIAGNOSTIC::READY;
      case hermes_kms::probe_result::uapi_too_old:
        return HERMES_KMS_DIAGNOSTIC::UAPI_TOO_OLD;
      case hermes_kms::probe_result::missing_caps:
        return HERMES_KMS_DIAGNOSTIC::MISSING_CAPABILITIES;
      case hermes_kms::probe_result::no_device:
      default:
        break;
    }

    // No usable hermes-kms device node. Distinguish why so the UI can point at
    // the right fix: module not built for this kernel, a DKMS build that
    // failed, the module simply not loaded, or a loaded module that never
    // created a device node.
    if (!kernel_module_loaded("hermes_kms")) {
      if (dkms_build_failed("hermes-kms")) {
        return HERMES_KMS_DIAGNOSTIC::DKMS_BUILD_FAILED;
      }
      if (!dkms_module_installed_for_running_kernel("hermes_kms.ko")) {
        return HERMES_KMS_DIAGNOSTIC::MODULE_NOT_INSTALLED;
      }
      return HERMES_KMS_DIAGNOSTIC::MODULE_NOT_LOADED;
    }

    // Module is loaded but exposed no hermes-kms card node.
    return HERMES_KMS_DIAGNOSTIC::DEVICE_NODE_MISSING;
  }

  HermesKmsStatus getHermesKmsStatus() {
    const auto probe = hermes_kms::probe();
    HermesKmsStatus status {
      .diagnostic = getHermesKmsDiagnostic(),
      .module_loaded = kernel_module_loaded("hermes_kms"),
      .module_installed = dkms_module_installed_for_running_kernel("hermes_kms.ko"),
      .device_present = probe.result == hermes_kms::probe_result::ok,
      .card_index = probe.card_index,
      .uapi_version = probe.uapi_version,
      .required_uapi_version = hermes_kms::required_uapi_version(),
      .experimental_multi_output_enabled = hermes_kms::multi_output_requested(),
      .multi_output_capable = probe.multi_output_capable,
      .experimental_isolated_sessions_enabled = config::video.hermes_kms_isolated_sessions,
      .multi_device_capable = probe.multi_device_capable,
      .device_count = probe.device_count,
      .output_count = probe.output_count,
      .private_seat_broker_count = 0,
      .missing_private_seat_brokers = {},
      .driver_version = probe.driver_version,
      .running_kernel = running_kernel_release(),
      .dkms_kernels = dkms_kernels("hermes-kms"),
      .active_displays = {},
    };

    // device_count includes the seat0 HOST card; only session_device_count has
    // a matching private seatd instance.
    for (uint32_t instance = 1; instance <= probe.session_device_count; ++instance) {
      const auto socket =
        fs::path {"/run/hermes-kms-seatd"} /
        std::to_string(instance) /
        "seatd.sock";
      std::error_code socket_ec;
      if (fs::is_socket(socket, socket_ec) &&
          ::access(socket.c_str(), R_OK | W_OK) == 0) {
        ++status.private_seat_broker_count;
      } else {
        status.missing_private_seat_brokers.push_back(instance);
      }
    }

    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, display] : virtual_displays) {
      if (display.using_hermes_kms && display.active) {
        status.active_displays.push_back({
          .name = display.name,
          .client_name = display.client_name,
          .device_index = display.device_index,
          .output_index = display.output_index,
          .drm_card_index = display.drm_card_index,
          .width = display.width,
          .height = display.height,
          .fps = display.fps / 1000,
          .frame_updates = 0,  // Cumulative KMS frame metrics live in getHermesKmsMetrics().
        });
      }
    }

    return status;
  }

  // ============================================================================
  // Utility Functions
  // ============================================================================

  static std::string generate_display_name(const uuid_util::uuid_t &guid) {
    return "VIRTUAL-" + guid.string().substr(0, 8);
  }

  constexpr int EVDI_DEVICE_SEARCH_LIMIT = 64;

  static int find_available_evdi_device() {
    // First use an already-created EVDI device, if one is available.
    for (int i = 0; i < EVDI_DEVICE_SEARCH_LIMIT; i++) {
      if (evdi.check_device(i) == EVDI_AVAILABLE) {
        return i;
      }
    }

    // evdi_add_device() reports a successful sysfs write, not the EVDI device
    // index. Rescan check_device() to find the index assigned by the kernel.
    //
    // libevdi's write_add_device() returns fwrite()'s result for a 1-byte
    // buffer: exactly 1 on a successful sysfs write, 0 on any failure
    // (fopen("/sys/devices/evdi/add", "w") failing, or the write itself
    // failing). It never returns a negative value, so the previous
    // `result < 0` check could never fire and silently swallowed every
    // real failure, leaving the caller to poll for a device that was
    // never created.
    const int result = evdi.add_device();
    if (result != 1) {
      BOOST_LOG(warning) << "[VDISPLAY] evdi_add_device() failed (returned " << result << ").";
      return -1;
    }

    BOOST_LOG(info) << "[VDISPLAY] Added EVDI device; waiting for its device index.";
    for (int attempt = 0; attempt < 125; ++attempt) {
      for (int i = 0; i < EVDI_DEVICE_SEARCH_LIMIT; ++i) {
        if (evdi.check_device(i) == EVDI_AVAILABLE) {
          BOOST_LOG(info) << "[VDISPLAY] EVDI device available at index " << i;
          return i;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds {20});
    }

    BOOST_LOG(warning) << "[VDISPLAY] No EVDI device became available after creation. "
                       << "Searched indices 0.." << (EVDI_DEVICE_SEARCH_LIMIT - 1) << '.';
    return -1;
  }

  // EVDI device indexes and DRM card indexes are independent.  In particular,
  // card0 is usually the physical GPU, even when the first EVDI device is 0.
  // Discover the card through sysfs instead of assuming they are the same.
  static int find_evdi_drm_card(int device_index) {
    const auto expected_device = "evdi." + std::to_string(device_index);
    std::vector<int> evdi_cards;

    try {
      for (const auto &entry : fs::directory_iterator("/sys/class/drm")) {
        const auto name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) {
          continue;
        }

        std::error_code ec;
        const auto device_path = fs::weakly_canonical(entry.path() / "device", ec);
        if (ec) {
          continue;
        }

        const auto device_path_string = device_path.string();
        const auto module_link = entry.path() / "device" / "driver" / "module";
        const auto module = fs::read_symlink(module_link, ec).filename().string();
        if (ec || module != "evdi") {
          continue;
        }

        const auto card_index = std::stoi(name.substr(4));
        if (device_path_string.find(expected_device) != std::string::npos) {
          return card_index;
        }
        evdi_cards.push_back(card_index);
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "[VDISPLAY] Unable to inspect EVDI DRM devices: " << e.what();
      return -1;
    }

    // Older EVDI kernels do not expose the evdi.N component in the sysfs
    // path.  A single EVDI card is still unambiguous in that case.
    return evdi_cards.size() == 1 ? evdi_cards.front() : -1;
  }

  static uint32_t normalize_refresh_rate(uint32_t refresh_rate) {
    // Apollo's stream path uses mHz, while the early encoder probe passes Hz.
    // Accept both forms so a probe cannot produce a 0 Hz EDID (or divide by 0).
    return refresh_rate > 0 && refresh_rate < 1000 ? refresh_rate * 1000 : refresh_rate;
  }

  static void calculate_edid_checksum(unsigned char *edid, size_t block_size = 128) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < block_size - 1; i++) {
      checksum += edid[i];
    }
    edid[block_size - 1] = 256 - checksum;
  }

  static void create_detailed_timing_descriptor(unsigned char *dtd, uint32_t width, uint32_t height, uint32_t refresh_rate) {
    // Calculate timing parameters based on CVT (Coordinated Video Timings)
    // These are approximate values for common resolutions

    uint32_t h_blank, v_blank, h_front, h_sync, v_front, v_sync;

    // Blanking only. These are the standard industry timings for each
    // resolution and do not change with refresh rate — the refresh rate is
    // carried by the pixel clock, which is derived below.
    if (width == 3840 && height == 2160) {
      h_blank = 560;
      v_blank = 90;
      h_front = 176;
      h_sync = 88;
      v_front = 8;
      v_sync = 10;
    } else if (width == 2560 && height == 1440) {
      h_blank = 160;
      v_blank = 44;
      h_front = 48;
      h_sync = 32;
      v_front = 3;
      v_sync = 5;
    } else if (width == 1920 && height == 1080) {
      h_blank = 280;
      v_blank = 45;
      h_front = 88;
      h_sync = 44;
      v_front = 4;
      v_sync = 5;
    } else if (width == 1280 && height == 720) {
      h_blank = 370;
      v_blank = 30;
      h_front = 110;
      h_sync = 40;
      v_front = 5;
      v_sync = 5;
    } else if (width == 1280 && height == 800) {
      // Steam Deck native mode, CVT-RB blanking.
      h_blank = 160;
      v_blank = 23;
      h_front = 48;
      h_sync = 32;
      v_front = 3;
      v_sync = 6;
    } else {
      // Approximate CVT blanking for anything else.
      h_blank = static_cast<uint32_t>(width * 0.15);
      v_blank = 45;
      h_front = h_blank / 4;
      h_sync = h_blank / 4;
      v_front = 3;
      v_sync = 5;
    }

    const uint32_t h_active = width;
    const uint32_t v_active = height;
    const uint32_t h_total = h_active + h_blank;
    const uint32_t v_total = v_active + v_blank;

    // Derive the pixel clock from the requested refresh rate. Every entry above
    // used to carry its own hardcoded clock, and every one of them worked out to
    // 60 Hz, so the descriptor advertised a 60 Hz display no matter what the
    // client asked for — a request for 1080p120 produced a 1080p60 EDID.
    uint64_t pixel_clock_khz =
      (static_cast<uint64_t>(h_total) * v_total * std::max<uint32_t>(refresh_rate, 1)) / 1000;

    // The descriptor stores the clock in 16 bits of 10 kHz units, so it cannot
    // express more than 655.35 MHz — 4K above about 66 Hz does not fit. Clamp to
    // the highest rate that does, rather than letting the value wrap into a
    // nonsense mode, and say so: silently getting a different refresh rate than
    // asked for is exactly the confusion this function used to cause.
    constexpr uint64_t max_dtd_clock_khz = 655350;
    if (pixel_clock_khz > max_dtd_clock_khz) {
      const uint32_t achievable_hz =
        static_cast<uint32_t>((max_dtd_clock_khz * 1000) / (static_cast<uint64_t>(h_total) * v_total));
      BOOST_LOG(warning) << "[VDISPLAY] "sv << width << 'x' << height << '@' << refresh_rate
                         << "Hz needs a pixel clock a detailed timing descriptor cannot hold; "sv
                         << "advertising "sv << achievable_hz << "Hz instead"sv;
      pixel_clock_khz = max_dtd_clock_khz;
    }

    const uint16_t pixel_clock = static_cast<uint16_t>(pixel_clock_khz / 10);

    // Detailed Timing Descriptor format (18 bytes)
    dtd[0] = pixel_clock & 0xFF;
    dtd[1] = (pixel_clock >> 8) & 0xFF;

    dtd[2] = h_active & 0xFF;
    dtd[3] = h_blank & 0xFF;
    dtd[4] = ((h_active >> 8) & 0x0F) << 4 | ((h_blank >> 8) & 0x0F);

    dtd[5] = v_active & 0xFF;
    dtd[6] = v_blank & 0xFF;
    dtd[7] = ((v_active >> 8) & 0x0F) << 4 | ((v_blank >> 8) & 0x0F);

    dtd[8] = h_front & 0xFF;
    dtd[9] = h_sync & 0xFF;
    dtd[10] = ((v_front & 0x0F) << 4) | (v_sync & 0x0F);
    dtd[11] = (((h_front >> 8) & 0x03) << 6) | (((h_sync >> 8) & 0x03) << 4) |
              (((v_front >> 4) & 0x03) << 2) | ((v_sync >> 4) & 0x03);

    // Physical size (approximate based on 27" diagonal for 4K, scaled for others)
    uint32_t h_size_mm = (width * 600) / 3840;  // 600mm for 4K width
    uint32_t v_size_mm = (height * 340) / 2160; // 340mm for 4K height
    dtd[12] = h_size_mm & 0xFF;
    dtd[13] = v_size_mm & 0xFF;
    dtd[14] = ((h_size_mm >> 8) & 0x0F) << 4 | ((v_size_mm >> 8) & 0x0F);

    dtd[15] = 0; // No border
    dtd[16] = 0; // No border
    dtd[17] = 0x1E; // Digital separate sync, positive H and V
  }

  static unsigned char *generate_edid_for_resolution(uint32_t width, uint32_t height, uint32_t refresh_rate) {
    static unsigned char edid[256]; // Support for 1 extension block
    memset(edid, 0, sizeof(edid));

    // Block 0: Base EDID
    // Header
    edid[0] = 0x00;
    edid[1] = 0xFF; edid[2] = 0xFF; edid[3] = 0xFF;
    edid[4] = 0xFF; edid[5] = 0xFF; edid[6] = 0xFF;
    edid[7] = 0x00;

    // Manufacturer ID: "APL" (Apollo)
    edid[8] = 0x06; edid[9] = 0x4C;

    // Product code
    edid[10] = 0x01; edid[11] = 0x00;

    // Serial number
    edid[12] = 0x01; edid[13] = 0x00; edid[14] = 0x00; edid[15] = 0x00;

    // Week and year of manufacture (week 1, 2024)
    edid[16] = 0x01; edid[17] = 0x22;

    // EDID version 1.4
    edid[18] = 0x01; edid[19] = 0x04;

    // Video input: Digital, 8-bit color, DisplayPort
    edid[20] = 0xB5;

    // Screen size (cm) - approximate for 27"
    edid[21] = 60; // 60 cm wide
    edid[22] = 34; // 34 cm tall

    // Gamma (2.2)
    edid[23] = 0x78;

    // Features: RGB, preferred timing in DTD1
    edid[24] = 0x3A;

    // Chromaticity coordinates (standard sRGB)
    edid[25] = 0xFC; edid[26] = 0x81; edid[27] = 0xA4; edid[28] = 0x55;
    edid[29] = 0x4D; edid[30] = 0x9D; edid[31] = 0x25; edid[32] = 0x12;
    edid[33] = 0x50; edid[34] = 0x54;

    // Advertise only the requested DTD mode. Extra established/standard
    // timings become misleading choices in a game's display settings.
    edid[35] = 0x00; edid[36] = 0x00; edid[37] = 0x00;
    for (int i = 38; i < 54; i += 2) {
      edid[i] = 0x01;
      edid[i + 1] = 0x01;
    }

    // Detailed Timing Descriptor 1 (preferred timing)
    create_detailed_timing_descriptor(&edid[54], width, height, refresh_rate);

    // Descriptor 2: Display name
    edid[72] = 0x00; edid[73] = 0x00; edid[74] = 0x00; edid[75] = 0xFC; edid[76] = 0x00;
    const char *name = "APOLLO VDISP";
    for (int i = 0; i < 13 && name[i]; i++) {
      edid[77 + i] = name[i];
    }
    edid[89] = '\n';

    // Descriptor 3: Display range limits
    edid[90] = 0x00; edid[91] = 0x00; edid[92] = 0x00; edid[93] = 0xFD; edid[94] = 0x00;
    edid[95] = 0x18; // Min V rate: 24 Hz
    edid[96] = 0x78; // Max V rate: 120 Hz
    edid[97] = 0x0F; // Min H rate: 15 kHz
    edid[98] = 0xA0; // Max H rate: 160 kHz
    edid[99] = 0x78; // Max pixel clock: 1200 MHz (for 4K@120Hz support)
    edid[100] = 0x00; // GTF support
    edid[101] = 0x0A; // Newline padding
    for (int i = 102; i < 108; i++) edid[i] = 0x20; // Space padding

    // Descriptor 4: Dummy/unused
    edid[108] = 0x00; edid[109] = 0x00; edid[110] = 0x00; edid[111] = 0x10; edid[112] = 0x00;
    for (int i = 113; i < 126; i++) edid[i] = 0x20;

    // Extension flag: 1 extension block (for resolutions > 1080p)
    bool needs_extension = (width > 1920 || height > 1080);
    edid[126] = needs_extension ? 0x01 : 0x00;

    // Calculate checksum for block 0
    calculate_edid_checksum(edid, 128);

    // Block 1: CEA-861 Extension (for 4K support)
    if (needs_extension) {
      edid[128] = 0x02; // CEA extension tag
      edid[129] = 0x03; // Revision 3
      edid[130] = 0x18; // DTD offset (24 bytes for data blocks)
      edid[131] = 0x72; // Native DTDs, YCbCr support

      // Video Data Block
      edid[132] = 0x47; // Video tag (0x40) + length (7)
      edid[133] = 0x90; // VIC 16: 1080p60 (native)
      edid[134] = 0x04; // VIC 4: 720p60
      edid[135] = 0x03; // VIC 3: 480p60
      edid[136] = 0x5F; // VIC 95: 4K@60Hz (VIC 95)
      edid[137] = 0x60; // VIC 96: 4K@60Hz (VIC 96)
      edid[138] = 0x61; // VIC 97: 4K@60Hz (VIC 97)
      edid[139] = 0x65; // VIC 101: 4K@120Hz

      // HDMI Vendor Specific Data Block
      edid[140] = 0x67; // Vendor tag (0x60) + length (7)
      edid[141] = 0x03; // IEEE OUI for HDMI (0x000C03)
      edid[142] = 0x0C;
      edid[143] = 0x00;
      edid[144] = 0x10; // Source physical address
      edid[145] = 0x00;
      edid[146] = 0x00; // Supports AI, DC 48/36/30 bit
      edid[147] = 0x78; // Max TMDS clock / 5 MHz = 600 MHz

      // Detailed Timing Descriptor for 4K if needed
      if (width >= 3840) {
        create_detailed_timing_descriptor(&edid[152], 3840, 2160, 60);
      }

      // Calculate checksum for block 1
      calculate_edid_checksum(&edid[128], 128);
    }

    return edid;
  }

  // Drain libevdi events promptly. Besides delivering pixels into EvdiBuffer,
  // evdi_grab_pixels acknowledges the pageflip to the EVDI kernel driver; if
  // no consumer does that, compositors can stall on timed-out pageflips.
  static void evdi_events_loop() {
    evdi_event_context event_context {};
    event_context.update_ready_handler = [](int, void *) {};
    constexpr int max_rects = 16;
    constexpr auto min_grab_interval = std::chrono::microseconds {4167};
    constexpr auto max_grab_interval = std::chrono::microseconds {16667};
    evdi_rect rects[max_rects];
    std::map<evdi_handle, std::chrono::steady_clock::time_point> next_grab_by_handle;

    while (evdi_events_running) {
      {
        std::lock_guard<std::mutex> lock(vdisplay_mutex);
        std::set<evdi_handle> active_handles;
        const auto now = std::chrono::steady_clock::now();

        for (auto &[guid, display] : virtual_displays) {
          if (!display.using_evdi || !display.handle || !display.evdi_buffer || display.evdi_buffer_id <= 0) {
            continue;
          }
          active_handles.insert(display.handle);

          const auto next_grab = next_grab_by_handle.find(display.handle);
          if (next_grab != next_grab_by_handle.end() && next_grab->second > now) {
            continue;
          }

          pollfd poll_fd {};
          poll_fd.fd = evdi.get_event_ready(display.handle);
          poll_fd.events = POLLIN;
          if (poll_fd.fd < 0 || ::poll(&poll_fd, 1, 0) <= 0 || !(poll_fd.revents & POLLIN)) {
            evdi.request_update(display.handle, display.evdi_buffer_id);
            continue;
          }

          evdi.handle_events(display.handle, &event_context);
          int rect_count = max_rects;
          display.evdi_buffer->begin_write();
          evdi.grab_pixels(display.handle, rects, &rect_count);
          display.evdi_buffer->end_write();
          display.evdi_buffer->mark_updated();
          evdi.request_update(display.handle, display.evdi_buffer_id);
          const auto refresh_rate = std::max<uint32_t>(display.fps, 1);
          const auto session_interval = std::chrono::microseconds {1000000 / refresh_rate};
          next_grab_by_handle[display.handle] = now + std::clamp(session_interval, min_grab_interval, max_grab_interval);
        }

        for (auto it = next_grab_by_handle.begin(); it != next_grab_by_handle.end();) {
          if (!active_handles.contains(it->first)) {
            it = next_grab_by_handle.erase(it);
          } else {
            ++it;
          }
        }
      }

      // A 1 ms pump keeps pageflip-to-capture wakeup below one millisecond
      // without busy-spinning when no virtual session is active.
      std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
  }

  static void start_evdi_events_thread() {
    if (evdi_events_running.exchange(true)) {
      return;
    }
    evdi_events_thread = std::thread(evdi_events_loop);
  }

  static void stop_evdi_events_thread() {
    evdi_events_running = false;
    if (evdi_events_thread.joinable()) {
      evdi_events_thread.join();
    }
  }

  // ============================================================================
  // Public API Implementation
  // ============================================================================

  DRIVER_STATUS openVDisplayDevice() {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (driver_status == DRIVER_STATUS::OK) {
      return driver_status;
    }

    const auto backend = selected_backend();
    BOOST_LOG(info) << "[VDISPLAY] Initializing Linux virtual display driver with " << backend_name(backend) << " backend...";

    // Nothing to open, nothing to diagnose, and above all nothing to warn
    // about: a host configured for no virtual-display device is not a host with
    // a broken one. Returning before the EVDI machinery runs is what keeps it
    // from reporting a missing library the deployment never wanted.
    if (backend == VirtualDisplayBackend::NONE) {
      evdi_available = false;
      unload_evdi_library();
      BOOST_LOG(info) << "[VDISPLAY] No virtual-display backend is configured; Hermes will only "
                         "stream outputs the session already has.";
      driver_status = DRIVER_STATUS::NOT_SUPPORTED;
      return driver_status;
    }

    kscreen::recover_on_startup();
    mutter::recover_on_startup();

    if (backend == VirtualDisplayBackend::HERMES_KMS) {
      evdi_available = false;
      unload_evdi_library();
      if (!hermes_kms::available()) {
        BOOST_LOG(warning) << "[VDISPLAY/Hermes-KMS] Hermes-KMS is unavailable; virtual-display sessions are disabled.";
        driver_status = DRIVER_STATUS::NOT_SUPPORTED;
        return driver_status;
      }

      std::vector<std::string> boot_connectors;
      if (hermes_kms::disconnect_unowned_outputs(boot_connectors)) {
        for (const auto &connector : boot_connectors) {
          // Each helper no-ops when its compositor is not the running one.
          kscreen::recover_after_unowned_virtual_disconnect(connector);
          mutter::recover_after_unowned_virtual_disconnect(connector);
        }
      }

      // Cards outlive the process that asked for one. This Hermes owns none
      // yet, so anything still standing in its name is from a run that died.
      if (card_broker::available()) {
        card_broker::sweep();
      }

      driver_status = DRIVER_STATUS::OK;
      device_open = true;
      BOOST_LOG(info) << "[VDISPLAY/Hermes-KMS] Hermes-KMS available - experimental zero-copy virtual display supported"
                      << (hermes_kms::multi_output_requested() ? " with session-scoped multi-output enabled." : ".");
      return driver_status;
    }

    if (backend == VirtualDisplayBackend::HYPRLAND_HEADLESS) {
      evdi_available = false;
      unload_evdi_library();
      if (!hyprland::available()) {
        hyprland::log_unreachable(false);
        driver_status = DRIVER_STATUS::NOT_SUPPORTED;
        return driver_status;
      }

      driver_status = DRIVER_STATUS::OK;
      device_open = true;
      BOOST_LOG(info) << "[VDISPLAY] Hyprland headless output backend available - virtual displays supported.";
      return driver_status;
    }

    // Try to load EVDI library
    evdi_available = load_evdi_library();

    if (evdi_available) {
      // Check if kernel module is loaded
      if (!check_evdi_module_loaded()) {
        BOOST_LOG(warning) << "[VDISPLAY] EVDI library loaded but kernel module not available.";
        BOOST_LOG(warning) << "[VDISPLAY] Virtual displays are unavailable until the module is loaded.";
        evdi_available = false;
        unload_evdi_library();
      }
    }

    if (evdi_available) {
      BOOST_LOG(info) << "[VDISPLAY] EVDI available - real virtual displays supported!";
    } else {
      BOOST_LOG(warning) << "[VDISPLAY] EVDI is unavailable; virtual-display sessions are disabled.";
      BOOST_LOG(warning) << "[VDISPLAY] Install libevdi and load the evdi kernel module, then restart Apollo.";
      driver_status = DRIVER_STATUS::NOT_SUPPORTED;
      return driver_status;
    }

    driver_status = DRIVER_STATUS::OK;
    device_open = true;
    BOOST_LOG(info) << "[VDISPLAY] Linux virtual display driver initialized successfully.";

    return driver_status;
  }

  void closeVDisplayDevice() {
    // Idempotent: the watchdog failure path, an explicit shutdown and proc's
    // deinit_t can all reach this. Tearing down twice would re-run set_output()
    // and ::close() on drm_fds that were already released.
    if (!device_open.exchange(false)) {
      return;
    }

    BOOST_LOG(info) << "[VDISPLAY] Closing Linux virtual display driver...";
    // Before the displays go: the watch outlives any single one of them,
    // and its thread holds a bus connection.
    mutter::unwatch_layout();

    // Stop watchdog thread
    watchdog_running = false;
    if (watchdog_thread.joinable()) {
      // The watchdog can call the failure callback itself. Joining the current
      // thread would throw, while joining while holding vdisplay_mutex can
      // deadlock with a watchdog which is just checking a display.
      if (watchdog_thread.get_id() == std::this_thread::get_id()) {
        watchdog_thread.detach();
      } else {
        watchdog_thread.join();
      }
    }
    stop_evdi_events_thread();

    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    // Clean up all virtual displays
    for (auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active) {
        if (vdinfo.using_evdi && vdinfo.handle) {
          if (vdinfo.evdi_buffer_id > 0) {
            evdi.unregister_buffer(vdinfo.handle, vdinfo.evdi_buffer_id);
          }
          evdi.disconnect(vdinfo.handle);
          evdi.close(vdinfo.handle);
        }
        if (vdinfo.using_hermes_kms && vdinfo.drm_fd >= 0) {
          hermes_kms::set_output(vdinfo.drm_fd, false, 0, 0, 0, vdinfo.session_id);
        }
        if (vdinfo.drm_fd >= 0) {
          ::close(vdinfo.drm_fd);
        }
        kscreen::restore(vdinfo.name);
        mutter::restore(vdinfo.name);
        mutter::unmap_input_devices();
      }
      hermes_kms::forget_secret(vdinfo.session_token.data(), sizeof(vdinfo.session_token));
    }
    virtual_displays.clear();

    // Unload EVDI library
    unload_evdi_library();

    driver_status = DRIVER_STATUS::UNKNOWN;
    BOOST_LOG(info) << "[VDISPLAY] Linux virtual display driver closed.";
  }

  bool startPingThread(std::function<void()> failCb) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (watchdog_running) {
      return true;
    }

    watchdog_running = true;

    watchdog_thread = std::thread([failCb = std::move(failCb)]() {
      BOOST_LOG(debug) << "[VDISPLAY] Watchdog thread started.";

      while (watchdog_running) {
        std::this_thread::sleep_for(5s);

        if (!watchdog_running) {
          break;
        }

        bool display_lost = false;
        {
          std::lock_guard<std::mutex> lock(vdisplay_mutex);
          for (const auto &[guid, vdinfo] : virtual_displays) {
            if (vdinfo.active && vdinfo.using_evdi && vdinfo.handle) {
              // Check EVDI device health
              int ready = evdi.get_event_ready(vdinfo.handle);
              if (ready < 0) {
                BOOST_LOG(error) << "[VDISPLAY] Virtual display " << vdinfo.name << " lost!";
                display_lost = true;
                break;
              }
            } else if (vdinfo.active && vdinfo.using_hermes_kms && vdinfo.drm_fd >= 0) {
              hermes_kms::status_t status {};
              if (!hermes_kms::get_status(vdinfo.drm_fd, status)) {
                BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Virtual display " << vdinfo.name << " lost!";
                display_lost = true;
                break;
              }
            }
          }
        }

        if (display_lost) {
          if (failCb) {
            failCb();
          }
          return;
        }
      }

      BOOST_LOG(debug) << "[VDISPLAY] Watchdog thread stopped.";
    });

    return true;
  }

  bool setRenderAdapterByName(const std::string &adapterName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (adapterName.empty()) {
      BOOST_LOG(debug) << "[VDISPLAY] No specific adapter requested.";
      return true;
    }

    BOOST_LOG(info) << "[VDISPLAY] Adapter hint: " << adapterName;
    // On Linux, we don't need to select specific adapters for EVDI
    return true;
  }

  std::string createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const uuid_util::uuid_t &guid,
    std::optional<uid_t> session_owner_uid,
    virtual_display_layout_e layout
  ) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (driver_status != DRIVER_STATUS::OK) {
      BOOST_LOG(error) << "[VDISPLAY] Driver not initialized.";
      return "";
    }

    std::string guid_str = guid.string();
    if (virtual_displays.contains(guid_str)) {
      BOOST_LOG(error) << "[VDISPLAY] Client already owns an active virtual display: " << guid_str;
      return "";
    }
    std::string display_name = generate_display_name(guid);

    fps = normalize_refresh_rate(fps);
    const uint32_t fps_hz = fps / 1000;

    if (width == 0 || height == 0 || fps_hz == 0) {
      BOOST_LOG(error) << "[VDISPLAY] Refusing invalid virtual display mode "
                       << width << 'x' << height << '@' << fps_hz << "Hz";
      return "";
    }

    BOOST_LOG(info) << "[VDISPLAY] Creating virtual display: " << display_name
                    << " (W: " << width << ", H: " << height << ", FPS: " << fps_hz << ")";
    BOOST_LOG(info) << "[VDISPLAY] Client: " << s_client_name << " (" << s_client_uid << ")";

    VirtualDisplayInfo vdinfo;
    vdinfo.name = display_name;
    vdinfo.guid_str = guid_str;
    vdinfo.client_name = s_client_name ? s_client_name : "";
    vdinfo.width = width;
    vdinfo.height = height;
    vdinfo.fps = fps;
    vdinfo.device_index = -1;
    vdinfo.session_index = -1;
    vdinfo.output_index = -1;
    vdinfo.drm_card_index = -1;
    vdinfo.handle = nullptr;
    vdinfo.drm_fd = -1;
    vdinfo.active = true;
    vdinfo.using_evdi = false;
    vdinfo.using_hermes_kms = false;
    vdinfo.hermes_kms_session_lifecycle = false;
    vdinfo.broker_card.clear();
    vdinfo.using_hyprland_headless = false;
    vdinfo.session_id = 0;
    vdinfo.session_token = {};
    vdinfo.evdi_buffer_id = 0;
    vdinfo.layout = layout;

    const auto backend = selected_backend();

    if (backend == VirtualDisplayBackend::NONE) {
      BOOST_LOG(error) << "[VDISPLAY] A virtual display was requested but no virtual-display backend "
                          "is configured. Set virtual_display_backend to evdi or hermes_kms, or stream "
                          "an output the session already has.";
      return "";
    }

    // Hyprland owns its headless outputs; there is no DRM device to claim, no
    // connector to hotplug and no kscreen layout to diff, so this returns
    // before any of that runs. The mode is not set here: Hyprland publishes the
    // output at its own default and takes the client's geometry through
    // wlr-output-management, which activateVirtualDisplayOutput() already
    // drives for every wlr session.
    if (backend == VirtualDisplayBackend::HYPRLAND_HEADLESS) {
      if (!hyprland::available()) {
        hyprland::log_unreachable(true);
        return "";
      }

      const auto output = hyprland::create_headless();
      if (!output) {
        return "";
      }

      vdinfo.connector_name = *output;
      vdinfo.using_hyprland_headless = true;
      virtual_displays[guid_str] = vdinfo;

      BOOST_LOG(info) << "[VDISPLAY] Hyprland created headless output " << *output << " for " << display_name
                      << "; it renders on the primary GPU, so no virtual DRM device is involved.";
      return display_name;
    }

    const auto kscreen_before = kscreen::outputs();
    const auto outputs_before = kscreen::connected_output_names(kscreen_before);
    const auto enabled_before = kscreen::enabled_output_priorities(kscreen_before);

    if (backend == VirtualDisplayBackend::HERMES_KMS) {
      hermes_kms::device_t device {};
      uint64_t session_id = 0;
      bool claimed = false;
      if (config::video.hermes_kms_isolated_sessions) {
        claimed = hermes_kms::claim_available_device_output(
          device,
          width,
          height,
          fps_hz,
          session_id,
          vdinfo.broker_card,
          session_owner_uid
        );
      } else if (hermes_kms::open_device(device, true)) {
        claimed = hermes_kms::claim_available_output(
          device,
          width,
          height,
          fps_hz,
          session_id
        );
      }

      std::array<uint64_t, 2> session_token {};
      if (claimed && !hermes_kms::get_session_token(
                       device.fd,
                       device.selected_output_index,
                       session_id,
                       session_token
                     )) {
        hermes_kms::set_output(device.fd, false, 0, 0, 0, session_id, false);
        claimed = false;
      }
      if (!claimed && !vdinfo.broker_card.empty()) {
        // The card was created for this display and this display is not
        // happening; leaving it would hold a private seat for nothing.
        card_broker::remove(vdinfo.broker_card);
        vdinfo.broker_card.clear();
      }

      if (claimed) {
        hermes_kms::status_t status {};
        hermes_kms::get_status(device.fd, status);
        vdinfo.drm_fd = device.fd;
        device.fd = -1;
        vdinfo.device_index = device.version.uapi_version >= hermes_kms::multi_device_uapi_version ?
                                static_cast<int>(device.identity.device_index) :
                                0;
        vdinfo.session_index =
          device.version.uapi_version >= hermes_kms::session_device_uapi_version &&
              device.identity.device_role == hermes_kms::device_role_session ?
            static_cast<int>(device.identity.session_index) :
            -1;
        vdinfo.output_index = static_cast<int>(device.selected_output_index);
        vdinfo.drm_card_index = device.card_index;
        vdinfo.session_id = session_id;
        vdinfo.session_token = session_token;
        vdinfo.using_hermes_kms = true;
        vdinfo.hermes_kms_session_lifecycle =
          (device.caps.flags & hermes_kms::cap_session_lifecycle) != 0;
        vdinfo.connector_name = hermes_kms::cstr(device.identity.connector_name);
        display_name = hermes_kms::cstr(device.identity.output_name);
        if (display_name.empty()) {
          display_name = "HERMES-1";
        }
        vdinfo.name = display_name;
        BOOST_LOG(info) << "[VDISPLAY/Hermes-KMS] Connected " << display_name
                        << " device=" << (vdinfo.device_index + 1)
                        << " session_device=" << vdinfo.session_index
                        << " output=" << (vdinfo.output_index + 1)
                        << " connector=" << vdinfo.connector_name
                        << " card=" << vdinfo.drm_card_index
                        << " session=" << vdinfo.session_id
                        << " requested=" << status.requested_width << 'x' << status.requested_height
                        << '@' << status.requested_refresh_hz
                        << " flags=0x" << std::hex << status.flags << std::dec;
        if (!config::video.hermes_kms_isolated_sessions &&
            !vdinfo.connector_name.empty()) {
          kscreen::activate_evdi_output(
            vdinfo.name,
            outputs_before,
            enabled_before,
            vdinfo.connector_name,
            "Hermes-KMS",
            static_cast<int>(vdinfo.width),
            static_cast<int>(vdinfo.height),
            static_cast<int>(vdinfo.fps / 1000),
            vdinfo.layout
          );
        }
      }
      hermes_kms::forget_secret(session_token.data(), sizeof(session_token));
      hermes_kms::close_device(device);
    } else if (evdi_available) {
      // Create real virtual display using EVDI
      int device = find_available_evdi_device();
      if (device >= 0) {
        evdi_handle handle = evdi.open(device);
        if (handle) {
          // Generate EDID for requested resolution
          unsigned char *edid = generate_edid_for_resolution(width, height, fps_hz);

          // Determine EDID size (128 for base, 256 with extension for 4K)
          unsigned int edid_size = (width > 1920 || height > 1080) ? 256 : 128;

          // Connect with EDID (no area limit)
          BOOST_LOG(info) << "[VDISPLAY] Connecting with " << edid_size << "-byte EDID for " << width << "x" << height;
          evdi.connect(handle, edid, edid_size, 0);

          // The DRM node is added asynchronously after connect(). Poll at a
          // short interval: this is on the stream-start critical path, so the
          // former 100 ms sleep made a ready display wait unnecessarily.
          for (int attempt = 0; attempt < 25 && vdinfo.drm_card_index < 0; ++attempt) {
            vdinfo.drm_card_index = find_evdi_drm_card(device);
            if (vdinfo.drm_card_index < 0) {
              std::this_thread::sleep_for(std::chrono::milliseconds {20});
            }
          }

          if (vdinfo.drm_card_index >= 0) {
            vdinfo.device_index = device;
            vdinfo.handle = handle;
            vdinfo.using_evdi = true;
            const std::string card_path = "/dev/dri/card" + std::to_string(vdinfo.drm_card_index);
            display_name = "VIRTUAL-card" + std::to_string(vdinfo.drm_card_index);
            vdinfo.name = display_name;

            vdinfo.evdi_buffer = std::make_shared<EvdiBuffer>(width, height);
            vdinfo.evdi_buffer_id = 1;
            evdi_buffer buffer {};
            buffer.id = vdinfo.evdi_buffer_id;
            buffer.buffer = vdinfo.evdi_buffer->raw_buffer();
            buffer.width = static_cast<int>(width);
            buffer.height = static_cast<int>(height);
            buffer.stride = static_cast<int>(vdinfo.evdi_buffer->stride());
            buffer.rects = nullptr;
            buffer.rect_count = 0;
            evdi.register_buffer(handle, buffer);
            evdi.request_update(handle, vdinfo.evdi_buffer_id);
            BOOST_LOG(info) << "[VDISPLAY] Created EVDI virtual display on device " << device
                            << " (" << card_path << ')';

            // KWin must own the DRM card and explicitly enable the hotplugged
            // EVDI connector. Opening card_path here races for DRM master and
            // prevents KWin from ever exposing the output to KScreen.
            kscreen::activate_evdi_output(
              vdinfo.name,
              outputs_before,
              enabled_before,
              evdi_connector_name(vdinfo.drm_card_index),
              "EVDI",
              static_cast<int>(vdinfo.width),
              static_cast<int>(vdinfo.height),
              static_cast<int>(vdinfo.fps / 1000),
              vdinfo.layout
            );
          } else {
            BOOST_LOG(error) << "[VDISPLAY] EVDI connected but no matching DRM card appeared.";
            evdi.disconnect(handle);
            evdi.close(handle);
          }
        } else {
          BOOST_LOG(warning) << "[VDISPLAY] Failed to open EVDI device " << device;
        }
      } else {
        BOOST_LOG(warning) << "[VDISPLAY] No available EVDI device.";
      }
    }

    if (!vdinfo.using_evdi && !vdinfo.using_hermes_kms) {
      BOOST_LOG(error) << "[VDISPLAY] Failed to create a usable " << backend_name(backend) << " virtual display.";
      return "";
    }

    const bool using_evdi = vdinfo.using_evdi;
    virtual_displays.emplace(guid_str, std::move(vdinfo));
    hermes_kms::forget_secret(vdinfo.session_token.data(), sizeof(vdinfo.session_token));
    if (using_evdi) {
      start_evdi_events_thread();
    }

    BOOST_LOG(info) << "[VDISPLAY] Virtual display created successfully: " << display_name;
    BOOST_LOG(info) << "[VDISPLAY] Mode: " << backend_name(backend) << " (real virtual display)";

    return display_name;
  }

  bool removeVirtualDisplay(const uuid_util::uuid_t &guid) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    std::string guid_str = guid.string();

    auto it = virtual_displays.find(guid_str);
    if (it == virtual_displays.end()) {
      BOOST_LOG(warning) << "[VDISPLAY] Virtual display not found: " << guid_str;
      return false;
    }

    auto &vdinfo = it->second;
    BOOST_LOG(info) << "[VDISPLAY] Removing virtual display: " << vdinfo.name;

    if (vdinfo.using_evdi && vdinfo.handle) {
      if (vdinfo.evdi_buffer_id > 0) {
        evdi.unregister_buffer(vdinfo.handle, vdinfo.evdi_buffer_id);
      }
      evdi.disconnect(vdinfo.handle);
      evdi.close(vdinfo.handle);
    }
    if (vdinfo.using_hermes_kms && vdinfo.drm_fd >= 0) {
      // Cut every consumer loose before the output goes down. Disabling ends
      // the session too, but a worker blocked in WAIT_FRAME only learns that
      // when its wait returns, and a bind already in flight with the token we
      // are about to forget can still land in between. REVOKE_BINDINGS fails
      // both immediately with EACCES, and rotates the token as it goes.
      if (vdinfo.hermes_kms_session_lifecycle && vdinfo.output_index >= 0) {
        std::array<uint64_t, 2> rotated {};
        const bool revoked = hermes_kms::revoke_session_bindings(
          vdinfo.drm_fd,
          static_cast<uint32_t>(vdinfo.output_index),
          vdinfo.session_id,
          rotated
        );
        hermes_kms::forget_secret(rotated.data(), sizeof(rotated));
        BOOST_LOG(debug) << "[VDISPLAY/Hermes-KMS] Session " << vdinfo.session_id
                         << (revoked ? " bindings revoked before disable." :
                                       " bindings could not be revoked; the disable ends them.");
      }
      hermes_kms::set_output(vdinfo.drm_fd, false, 0, 0, 0, vdinfo.session_id);
    }
    // A headless output outlives the session that asked for it - Hyprland keeps
    // it, and its workspaces, until something removes it - so leaving it behind
    // strands a monitor on the user's desktop after every stream.
    if (vdinfo.using_hyprland_headless) {
      hyprland::remove_output(vdinfo.connector_name);
    }
    hermes_kms::forget_secret(vdinfo.session_token.data(), sizeof(vdinfo.session_token));
    kscreen::restore(vdinfo.name);
    mutter::restore(vdinfo.name);
    mutter::unmap_input_devices();

    if (vdinfo.drm_fd >= 0) {
      ::close(vdinfo.drm_fd);
    }
    if (!vdinfo.broker_card.empty()) {
      // After the fd is closed, so the card is idle when it is unplugged.
      card_broker::remove(vdinfo.broker_card);
    }

    virtual_displays.erase(it);
    virtual_display_capture_fallback_active = false;

    BOOST_LOG(info) << "[VDISPLAY] Virtual display removed successfully.";
    return true;
  }

  static std::string virtual_display_connector_name(const std::string &display_name);
  static bool pushVirtualOutputMode(
    const std::string &displayName,
    const std::string &connector,
    int width,
    int height,
    int refresh_mhz,
    int deadline_ms = 4000
  );

  int changeDisplaySettings(const char *deviceName, int width, int height, int refresh_rate, int deadline_ms) {
    std::unique_lock<std::mutex> lock(vdisplay_mutex);

    refresh_rate = normalize_refresh_rate(refresh_rate);
    const int refresh_hz = refresh_rate / 1000;

    if (width <= 0 || height <= 0 || refresh_hz <= 0) {
      BOOST_LOG(error) << "[VDISPLAY] Refusing invalid virtual display mode "
                       << width << 'x' << height << '@' << refresh_hz << "Hz";
      return -1;
    }

    BOOST_LOG(info) << "[VDISPLAY] Changing display settings for " << deviceName
                    << " to " << width << "x" << height << "@" << refresh_hz << "Hz";

    // Find the virtual display
    bool found = false;
    std::string push_mode_for;
    for (auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.name == deviceName) {
        // createVirtualDisplay() is normally followed by this call with the
        // same values. Reconnecting EVDI in that case causes an avoidable
        // compositor modeset and a burst of capture reinitializations.
        //
        // Hermes-KMS is exempt. hermes_kms::set_output() is a cheap idempotent
        // ioctl rather than a reconnect, so the cost this guard avoids does not
        // exist there, while the skip silently dropped the only reassertion of
        // the client's mode: because KWin adopts the connector at its own
        // preferred mode, a session whose requested mode already matched what
        // createVirtualDisplay() recorded pushed nothing and streamed the
        // compositor's resolution instead of the client's.
        if (!vdinfo.using_hermes_kms &&
            vdinfo.width == static_cast<uint32_t>(width) &&
            vdinfo.height == static_cast<uint32_t>(height) &&
            vdinfo.fps == static_cast<uint32_t>(refresh_rate)) {
          BOOST_LOG(debug) << "[VDISPLAY] Requested mode is already active; skipping virtual-display reconnect.";
          return 0;
        }

        vdinfo.width = width;
        vdinfo.height = height;
        vdinfo.fps = refresh_rate;

        if (vdinfo.using_evdi && vdinfo.handle) {
          // Reconnect with new EDID for new resolution
          evdi.disconnect(vdinfo.handle);
          unsigned char *edid = generate_edid_for_resolution(width, height, refresh_hz);
          unsigned int edid_size = (width > 1920 || height > 1080) ? 256 : 128;
          BOOST_LOG(info) << "[VDISPLAY] Reconnecting with " << edid_size << "-byte EDID for " << width << "x" << height;
          evdi.connect(vdinfo.handle, edid, edid_size, 0);
        } else if (vdinfo.using_hermes_kms && vdinfo.drm_fd >= 0) {
          if (!hermes_kms::set_output(vdinfo.drm_fd, true, width, height, refresh_hz, vdinfo.session_id)) {
            return -1;
          }
          // The driver now scans out the new geometry, but the compositor still
          // drives the connector at the mode it adopted, and the capture path
          // follows the compositor. Move it too - after the lock is released,
          // because resolving the connector takes vdisplay_mutex again.
          push_mode_for = vdinfo.name;
        }

        BOOST_LOG(info) << "[VDISPLAY] Display settings updated successfully.";
        found = true;
        break;
      }
    }

    lock.unlock();

    if (!found) {
      BOOST_LOG(debug) << "[VDISPLAY] Display not found: " << deviceName;
      return 0;
    }

    if (!push_mode_for.empty()) {
      const auto connector = virtual_display_connector_name(push_mode_for);
      if (connector.empty() ||
          !pushVirtualOutputMode(push_mode_for, connector, width, height, refresh_rate, deadline_ms)) {
        BOOST_LOG(warning) << "[VDISPLAY] The compositor was not moved to " << width << 'x' << height << '@'
                           << refresh_hz << "Hz for " << push_mode_for
                           << "; the previous compositor mode stays active.";
        // The driver took the mode and the compositor did not. Callers that
        // care can say so; the ones that do not are no worse off than before,
        // since the display is still there and still capturable.
        return 1;
      }
    }

    return 0;
  }

  int changeDisplaySettings2(const char *deviceName, int width, int height, int refresh_rate, bool bApplyIsolated) {
    if (bApplyIsolated) {
      BOOST_LOG(debug) << "[VDISPLAY] Isolated mode is handled by the active virtual-display backend.";
    }
    return changeDisplaySettings(deviceName, width, height, refresh_rate);
  }

  static std::string evdi_connector_name(int card_index) {
    const auto prefix = "card" + std::to_string(card_index) + "-";
    try {
      for (const auto &entry : fs::directory_iterator("/sys/class/drm")) {
        const auto name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0) {
          continue;
        }
        std::error_code ec;
        const auto module = fs::canonical(entry.path() / "device/driver/module", ec).filename().string();
        if (!ec && module == "evdi") {
          return name.substr(prefix.size());
        }
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "[VDISPLAY] Unable to find EVDI X11 connector: " << e.what();
    }
    return {};
  }

  std::string getEvdiConnectorName(const std::string &displayName) {
    return evdi_connector_name(getEvdiCardIndex(displayName));
  }

  std::string getHermesKmsConnectorName(const std::string &displayName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, display] : virtual_displays) {
      if (display.name == displayName && display.using_hermes_kms) {
        return display.connector_name;
      }
    }
    return {};
  }

  bool buildMutterLayoutWithoutConnector(
    const std::string &current_state,
    const std::string &virtual_connector,
    std::string &serial,
    std::string &argument
  ) {
    mutter::state_t state;
    const bool ready =
      mutter::build_layout_without(current_state, virtual_connector, state, argument) == mutter::layout_repair::ready;
    serial = ready ? state.serial : std::string {};
    if (!ready) {
      argument.clear();
    }
    return ready;
  }

  bool buildMutterLayoutWithMode(
    const std::string &current_state,
    const std::string &connector,
    uint32_t width,
    uint32_t height,
    uint32_t refresh_mhz,
    std::string &serial,
    std::string &argument
  ) {
    mutter::state_t state;
    const bool ready =
      mutter::build_layout_with_mode(current_state, connector, width, height, refresh_mhz, state, argument) ==
      mutter::mode_push::ready;
    serial = ready ? state.serial : std::string {};
    if (!ready) {
      argument.clear();
    }
    return ready;
  }

  bool buildMutterExclusiveLayout(
    const std::string &current_state,
    const std::string &connector,
    std::string &serial,
    std::string &argument
  ) {
    mutter::state_t state;
    const bool ready =
      mutter::build_exclusive_layout(current_state, connector, state, argument) == mutter::layout_change::ready;
    serial = ready ? state.serial : std::string {};
    if (!ready) {
      argument.clear();
    }
    return ready;
  }

  bool buildMutterMirrorLayout(
    const std::string &current_state,
    const std::string &connector,
    std::string &serial,
    std::string &argument
  ) {
    mutter::state_t state;
    const bool ready =
      mutter::build_mirror_layout(current_state, connector, state, argument) == mutter::layout_change::ready;
    serial = ready ? state.serial : std::string {};
    if (!ready) {
      argument.clear();
    }
    return ready;
  }

  bool buildMutterRestoreLayout(
    const std::string &current_state,
    std::string &serial,
    std::string &argument
  ) {
    mutter::state_t state;
    argument.clear();
    serial.clear();
    if (!mutter::parse_current_state(current_state, state) ||
        !mutter::build_restore_layout(state, argument)) {
      argument.clear();
      return false;
    }
    serial = state.serial;
    return true;
  }

  std::string mutterInputDeviceOutputValue(
    const std::string &current_state,
    const std::string &connector
  ) {
    mutter::state_t state;
    if (!mutter::parse_current_state(current_state, state)) {
      return {};
    }
    return mutter::device_output_value(state, connector);
  }

  void mutterInputDeviceSettingsTargets(std::string &touch, std::string &pen) {
    touch = mutter::device_settings_target(mutter::touch_settings);
    pen = mutter::device_settings_target(mutter::pen_settings);
  }

  uint64_t displayLayoutGeneration() {
    return mutter::layout_generation.load(std::memory_order_relaxed);
  }

  bool mutterDisplayGeometry(
    const std::string &current_state,
    const std::string &connector,
    int &offset_x,
    int &offset_y,
    int &environment_width,
    int &environment_height
  ) {
    mutter::state_t state;
    if (!mutter::parse_current_state(current_state, state)) {
      return false;
    }
    return mutter::geometry_from_state(
      state,
      connector,
      offset_x,
      offset_y,
      environment_width,
      environment_height
    );
  }

  void setVirtualDisplayCaptureFallbackActive(bool active) {
    virtual_display_capture_fallback_active = active;
  }

  static bool virtual_display_mode(const std::string &display_name, int &width, int &height, int &refresh_rate) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, display] : virtual_displays) {
      // The backend flags are the test for "this entry still owns a display",
      // not a list of DRM backends: a Hyprland headless output owns one just as
      // much, and leaving it out made activation return false with no log at
      // all - the mode was never pushed and nothing said why.
      if (display.name == display_name &&
          (display.using_evdi || display.using_hermes_kms || display.using_hyprland_headless)) {
        width = static_cast<int>(display.width);
        height = static_cast<int>(display.height);
        refresh_rate = static_cast<int>(display.fps);
        return true;
      }
    }
    return false;
  }

  /** The layout an app asked for, or extend when the display is not registered. */
  static virtual_display_layout_e virtual_display_layout(const std::string &display_name) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, display] : virtual_displays) {
      if (display.name == display_name) {
        return display.layout;
      }
    }
    return virtual_display_layout_e::extend;
  }

  std::string getHyprlandOutputName(const std::string &displayName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.name == displayName && vdinfo.using_hyprland_headless) {
        return vdinfo.connector_name;
      }
    }
    return {};
  }

  static std::string virtual_display_connector_name(const std::string &display_name) {
    // A Hyprland headless output has no DRM connector at all - the name is what
    // Hyprland called it - but it is the same kind of answer to the same
    // question every caller here asks: what does the compositor call this
    // display.
    if (auto output = getHyprlandOutputName(display_name); !output.empty()) {
      return output;
    }
    if (auto connector = getHermesKmsConnectorName(display_name); !connector.empty()) {
      return connector;
    }
    return getEvdiConnectorName(display_name);
  }

  /**
   * Make the compositor drive @p connector at the mode the client negotiated.
   *
   * Every compositor adopts a hotplugged connector on its own and at whichever
   * mode it prefers, while the capture path reports the real scanout - so
   * without this the client receives the resolution the compositor picked, not
   * the one it asked for. What differs per compositor is only how it is told:
   * KWin through kscreen-doctor, Mutter through ApplyMonitorsConfig, everything
   * else through wlr-output-management.
   *
   * @param refresh_mhz Refresh rate in mHz, as Hermes carries it internally.
   *                    Only the KWin path converts, because only kscreen-doctor
   *                    speaks whole Hz.
   * @return true when the compositor ends up driving the requested mode.
   *
   * The caller must NOT hold vdisplay_mutex: resolving the connector and the
   * KScreen layout both read the display registry back.
   */
  static bool pushVirtualOutputMode(
    const std::string &displayName,
    const std::string &connector,
    int width,
    int height,
    int refresh_mhz,
    int deadline_ms
  ) {
    if (width <= 0 || height <= 0 || refresh_mhz <= 0 || window_system != window_system_e::WAYLAND) {
      return false;
    }

    if (kscreen::is_active(displayName)) {
      return kscreen::apply_mode(displayName, width, height, refresh_mhz / 1000);
    }
    if (mutter::available()) {
      return mutter::apply_output_mode(connector, width, height, refresh_mhz, deadline_ms);
    }
#ifdef SUNSHINE_BUILD_WAYLAND
    const bool applied = wl::configure_virtual_output(connector, width, height, refresh_mhz, false);
    if (applied && !getHyprlandOutputName(displayName).empty()) {
      if (virtual_display_layout(displayName) == virtual_display_layout_e::mirror) {
        BOOST_LOG(warning) << "[VDISPLAY] The mirror layout is not implemented on Hyprland; "
                              "the virtual output extends the desktop instead.";
      }
      // Pinned here rather than at creation so a mid-session mode change
      // re-pins the new mode.
      hyprland::pin_output(connector, width, height, refresh_mhz);
    }
    return applied;
#else
    (void) connector;
    return false;
#endif
  }

  bool activateVirtualDisplayOutput(const std::string &displayName) {
    // KWin is configured when the display is created - enabled, placed and
    // driven at the requested mode in one kscreen-doctor transaction - so
    // activation only has to confirm the mode still holds. A mode KWin declines
    // is a warning there, not a failed activation: the output is composited and
    // capturable either way, and failing here would tear the session down over
    // a resolution mismatch the user can see.
    if (window_system == window_system_e::WAYLAND && kscreen::is_active(displayName)) {
      int width = 0;
      int height = 0;
      int refresh_mhz = 0;
      if (virtual_display_mode(displayName, width, height, refresh_mhz)) {
        kscreen::apply_mode(displayName, width, height, refresh_mhz / 1000);
      }
      return true;
    }

    const auto connector = virtual_display_connector_name(displayName);
    if (connector.empty()) {
      BOOST_LOG(warning) << "[VDISPLAY] Cannot activate virtual output: DRM connector was not found.";
      return false;
    }

#ifdef SUNSHINE_BUILD_WAYLAND
    if (window_system == window_system_e::WAYLAND) {
      int width = 0;
      int height = 0;
      int refresh_mhz = 0;
      if (!virtual_display_mode(displayName, width, height, refresh_mhz)) {
        return false;
      }

      const auto compositor = sessionCompositor();

      // A Hyprland headless output is already a real output rendered on the
      // primary GPU; only its mode is still Hyprland's default. That mode goes
      // through wlr-output-management like any other wlr session, so this needs
      // no Hyprland-specific branch - just the generic path below, with the
      // connector resolved to the HEADLESS name.
      //
      // A Hermes-KMS or EVDI display on Hyprland is the case that does not
      // work. Both assume the model KWin and Mutter use - the compositor
      // renders on the real GPU and imports the buffer into a display-only
      // device - while aquamarine wants every GPU that owns an output to host
      // its own GL renderer, which such a device cannot. Where a build does
      // import directly, the page-flip handshake against the software vblank
      // stalls instead. Both end as a black or frozen stream with input
      // working, so say it before the stream starts. Reaching here means the
      // display outlived the backend switch, since selected_backend() sends new
      // Hyprland displays to the headless path.
      if (compositor == compositor_e::hyprland && getHyprlandOutputName(displayName).empty()) {
        BOOST_LOG(warning) << "[VDISPLAY] Hyprland cannot composite onto a virtual DRM device: "
                              "aquamarine either cannot build a renderer on the display-only device or "
                              "stalls on page-flip against its software vblank. This display was not "
                              "created as a Hyprland headless output, so expect a black or frozen "
                              "stream; recreating it will take the headless path.";
      }

      // GNOME/Mutter exposes neither kscreen-doctor nor wlr-output-management,
      // only org.gnome.Mutter.DisplayConfig. Mutter does adopt the hotplugged
      // connector on its own, but asynchronously and at its own preferred mode,
      // so waiting for it to appear is not enough: capture would run at the
      // requested geometry while the compositor scans out another one, and the
      // client would receive a black image. Push the mode instead.
      if (mutter::available()) {  // implies compositor == compositor_e::mutter
        if (pushVirtualOutputMode(displayName, connector, width, height, refresh_mhz)) {
          BOOST_LOG(info) << "[VDISPLAY] Mutter is driving virtual output " << connector << " at " << width << "x"
                          << height << "@" << refresh_mhz << "; capturing it directly.";
          // Mirror is a display-creation choice, unlike exclusive mode, which
          // waits for a live session before the local screens may go dark.
          // A refusal is logged where it happens and costs the session the
          // cloning, not the display: the output is driven and capturable.
          if (virtual_display_layout(displayName) == virtual_display_layout_e::mirror) {
            mutter::apply_mirror(displayName, connector);
          }
          // Touch and pen are bound to a single monitor by GNOME, and the one
          // it picks by itself is never this one.
          mutter::map_input_devices(connector);
          // Everything decided above is a snapshot of a layout the compositor
          // can rewrite at any moment.
          mutter::watch_layout();
          return true;
        }
        BOOST_LOG(warning) << "[VDISPLAY] GNOME/Mutter did not end up driving " << connector << " at " << width << "x"
                           << height << "@" << refresh_mhz
                           << ". The virtual display may need to be enabled manually in GNOME Settings, "
                           << "or use a KDE/wlroots session for automatic activation.";
        return false;
      }

      const bool activated = pushVirtualOutputMode(displayName, connector, width, height, refresh_mhz);
      if (activated) {
        BOOST_LOG(info) << "[VDISPLAY] Activated Wayland virtual output " << connector;
      }
      return activated;
    }
#endif

    if (window_system != window_system_e::X11) {
      BOOST_LOG(warning) << "[VDISPLAY] No display-layout backend is available for this session.";
      return false;
    }

    if (connector.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) {
      BOOST_LOG(warning) << "[VDISPLAY] Refusing unsafe virtual connector name.";
      return false;
    }

    const std::string command = "xrandr --output '" + connector + "' --auto";
    if (std::system(command.c_str()) != 0) {
      BOOST_LOG(warning) << "[VDISPLAY] Failed to activate X11 virtual output " << connector;
      return false;
    }

    BOOST_LOG(info) << "[VDISPLAY] Activated X11 virtual output " << connector;
    return true;
  }

  bool enableExclusiveVirtualDisplay(const std::string &displayName) {
    if (window_system == window_system_e::WAYLAND && kscreen::is_active(displayName)) {
      const bool enabled = kscreen::make_exclusive(displayName);
      if (enabled) {
        exclusive_virtual_display_active = true;
      }
      return enabled;
    }

    const auto connector = virtual_display_connector_name(displayName);
    if (connector.empty()) {
      BOOST_LOG(warning) << "[VDISPLAY] Cannot enable exclusive mode: virtual connector was not found.";
      return false;
    }

    // GNOME reaches this before the wlr path below, which it does not speak.
    // ApplyMonitorsConfig disables every monitor left out of the config, so the
    // desktop is handed over by submitting one that names only this output.
    if (window_system == window_system_e::WAYLAND && mutter::available()) {
      const bool enabled = mutter::make_exclusive(displayName, connector);
      if (enabled) {
        exclusive_virtual_display_active = true;
      }
      return enabled;
    }

#ifdef SUNSHINE_BUILD_WAYLAND
    if (window_system == window_system_e::WAYLAND) {
      int width = 0;
      int height = 0;
      int refresh_rate = 0;
      if (!virtual_display_mode(displayName, width, height, refresh_rate)) {
        return false;
      }
      const bool enabled = wl::configure_virtual_output(connector, width, height, refresh_rate, true);
      if (enabled) {
        exclusive_virtual_display_active = true;
        BOOST_LOG(info) << "[VDISPLAY] Enabled exclusive Wayland layout for virtual output " << connector;
      } else {
        BOOST_LOG(warning) << "[VDISPLAY] Wayland compositor cannot apply the virtual-display exclusive layout.";
      }
      return enabled;
    }
#endif

    if (window_system != window_system_e::X11) {
      BOOST_LOG(warning) << "[VDISPLAY] Exclusive virtual display layout is unavailable for this session.";
      return false;
    }

    // Connector names come from sysfs. Keep the command defensive nevertheless.
    if (connector.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) {
      BOOST_LOG(warning) << "[VDISPLAY] Refusing unsafe virtual connector name.";
      return false;
    }

    const std::string command = "xrandr --output '" + connector + "' --auto --primary"
      " && xrandr --query | awk '$2 == \"connected\" && $1 != \"" + connector + "\" { print $1 }'"
      " | while IFS= read -r output; do xrandr --output \"$output\" --off; done";
    if (std::system(command.c_str()) != 0) {
      BOOST_LOG(warning) << "[VDISPLAY] Failed to enable exclusive X11 virtual display layout.";
      return false;
    }

    exclusive_virtual_display_active = true;
    BOOST_LOG(info) << "[VDISPLAY] Enabled exclusive X11 layout for virtual output " << connector;
    return true;
  }

  void restoreExclusiveVirtualDisplay() {
    if (!exclusive_virtual_display_active) {
      return;
    }

    // On GNOME the layout is restored per display when it is torn down, where
    // the captured configuration lives. Falling through would reach the wlr
    // path, which GNOME does not implement, and warn about it.
    if (window_system == window_system_e::WAYLAND && mutter::available()) {
      exclusive_virtual_display_active = false;
      return;
    }

#ifdef SUNSHINE_BUILD_WAYLAND
    if (window_system == window_system_e::WAYLAND) {
      if (!wl::restore_virtual_output_layout()) {
        BOOST_LOG(warning) << "[VDISPLAY] Failed to restore the Wayland display layout.";
      }
      exclusive_virtual_display_active = false;
      return;
    }
#endif

    // Restore every connected physical output before EVDI is removed.
    const auto command = "xrandr --query | awk '$2 == \"connected\" { print $1 }'"
      " | while IFS= read -r output; do xrandr --output \"$output\" --auto; done";
    if (std::system(command) != 0) {
      BOOST_LOG(warning) << "[VDISPLAY] Failed to restore the X11 display layout.";
    } else {
      BOOST_LOG(info) << "[VDISPLAY] Restored X11 physical display layout.";
    }
    exclusive_virtual_display_active = false;
  }

  std::string getPrimaryDisplay() {
    // Return first connected physical display
    try {
      for (const auto &entry : fs::directory_iterator("/dev/dri")) {
        const auto &path = entry.path();
        std::string filename = path.filename().string();
        if (filename.find("card") == 0 && filename.find("render") == std::string::npos) {
          int fd = ::open(path.c_str(), O_RDWR);
          if (fd >= 0) {
            drmModeRes *res = drmModeGetResources(fd);
            if (res) {
              for (int i = 0; i < res->count_connectors; i++) {
                drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
                if (conn && conn->connection == DRM_MODE_CONNECTED) {
                  std::string name = "HDMI-A-" + std::to_string(conn->connector_type_id);
                  drmModeFreeConnector(conn);
                  drmModeFreeResources(res);
                  ::close(fd);
                  return name;
                }
                if (conn) drmModeFreeConnector(conn);
              }
              drmModeFreeResources(res);
            }
            ::close(fd);
          }
        }
      }
    } catch (...) {}
    return "";
  }

  bool setPrimaryDisplay(const char *primaryDeviceName) {
    BOOST_LOG(debug) << "[VDISPLAY] setPrimaryDisplay is a no-op on Linux.";
    return true;
  }

  bool getDisplayHDRByName(const char *displayName) {
    BOOST_LOG(debug) << "[VDISPLAY] HDR check for: " << displayName;
    // EVDI doesn't support HDR currently
    return false;
  }

  bool setDisplayHDRByName(const char *displayName, bool enableAdvancedColor) {
    BOOST_LOG(debug) << "[VDISPLAY] HDR setting not supported on Linux/EVDI.";
    return false;
  }

  std::vector<std::string> matchDisplay(const std::string &sMatch) {
    std::vector<std::string> matches;

    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.name.find(sMatch) != std::string::npos) {
        matches.push_back(vdinfo.name);
      }
    }

    return matches;
  }

  // ============================================================================
  // EVDI-specific functions for KMS integration
  // ============================================================================

  /**
   * @brief Check if a display name is an EVDI virtual display.
   */
  bool isEvdiDisplay(const std::string &displayName) {
    if (!evdi_available) {
      return false;
    }

    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.name == displayName && vdinfo.using_evdi) {
        return true;
      }
    }
    return false;
  }

  bool isHermesKmsDisplay(const std::string &displayName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.name == displayName && vdinfo.using_hermes_kms) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Get the DRM card index for an EVDI display.
   * @return Card index, or -1 if not found.
   */
  int getEvdiCardIndex(const std::string &displayName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.name == displayName && vdinfo.using_evdi) {
        return vdinfo.drm_card_index;
      }
    }
    return -1;
  }

  int getHermesKmsCardIndex(const std::string &displayName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.name == displayName && vdinfo.using_hermes_kms) {
        return vdinfo.drm_card_index;
      }
    }
    return -1;
  }

  bool isHermesKmsDriverPresent() {
    auto devices = hermes_kms::open_devices(false);
    const bool present = !devices.empty();
    for (auto &candidate : devices) {
      hermes_kms::close_device(candidate);
    }
    return present;
  }

  std::vector<std::string> listHermesKmsDisplayNames() {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    std::vector<std::string> names;
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.using_hermes_kms) {
        names.emplace_back(vdinfo.name);
      }
    }
    return names;
  }

  std::string getHermesKmsDevicePath(const std::string &displayName) {
    const int card_index = getHermesKmsCardIndex(displayName);
    return card_index >= 0 ?
             "/dev/dri/card" + std::to_string(card_index) :
             std::string {};
  }

  std::string getHermesKmsSeatName(const std::string &displayName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.name == displayName &&
          vdinfo.using_hermes_kms &&
          vdinfo.session_index > 0) {
        // Must match Hermes-KMS' 72-hermes-kms-session-seats.rules (70- in
        // driver builds before the rule moved after systemd's uaccess tagging).
        //
        // The name deliberately does not start with "seat". systemd-logind
        // only registers seats whose names do, and a seat it registers is one
        // a multi-seat display manager offers a login on: naming these
        // seathermes<N> made SDDM start a greeter on each, which tore down the
        // host's own desktop session. These seats are for compositors Hermes
        // starts, not for anyone to log in to.
        return "hermes-kms-" + std::to_string(vdinfo.session_index);
      }
    }
    return {};
  }

  HermesKmsMetrics getHermesKmsMetrics() {
    HermesKmsMetrics out {};
    int target_card_index = -1;
    uint64_t session_id = 0;
    std::array<uint64_t, 2> session_token {};
    auto token_guard = util::fail_guard([&session_token]() {
      hermes_kms::forget_secret(session_token.data(), sizeof(session_token));
    });
    {
      std::lock_guard<std::mutex> lock(vdisplay_mutex);
      for (const auto &[guid, display] : virtual_displays) {
        if (display.using_hermes_kms && display.active) {
          out.output_index = display.output_index;
          out.output_name = display.name;
          target_card_index = display.drm_card_index;
          session_id = display.session_id;
          session_token = display.session_token;
          break;
        }
      }
    }

    hermes_kms::device_t device {};
    auto devices = hermes_kms::open_devices(false);
    if (devices.empty()) {
      return out;
    }
    auto selected = target_card_index >= 0 ?
                      std::find_if(devices.begin(), devices.end(), [target_card_index](const auto &candidate) {
                        return candidate.card_index == target_card_index;
                      }) :
                      devices.begin();
    if (selected == devices.end()) {
      for (auto &candidate : devices) {
        hermes_kms::close_device(candidate);
      }
      return out;
    }
    device = *selected;
    selected->fd = -1;
    for (auto &candidate : devices) {
      hermes_kms::close_device(candidate);
    }
    auto guard = util::fail_guard([&device]() { hermes_kms::close_device(device); });

    // The driver may be present without the metrics capability (older build).
    if (!(device.caps.flags & hermes_kms::cap_metrics)) {
      return out;
    }
    if (out.output_index < 0 ||
        !hermes_kms::bind_session(
          device.fd,
          static_cast<uint32_t>(out.output_index),
          session_id,
          session_token
        )) {
      return out;
    }

    hermes_kms::metrics_t metrics {};
    if (!hermes_kms::get_metrics(device.fd, metrics)) {
      BOOST_LOG(debug) << "[VDISPLAY/Hermes-KMS] GET_METRICS failed: " << std::strerror(errno);
      return out;
    }

    out.available = true;
    out.frame_sequence = metrics.frame_sequence;
    out.frame_update_count = metrics.frame_update_count;
    out.acquire_count = metrics.acquire_count;
    out.acquire_no_frame_count = metrics.acquire_no_frame_count;
    out.dmabuf_export_count = metrics.dmabuf_export_count;
    out.dmabuf_export_fail_count = metrics.dmabuf_export_fail_count;
    out.wait_count = metrics.wait_count;
    out.wait_ready_count = metrics.wait_ready_count;
    out.wait_timeout_count = metrics.wait_timeout_count;
    out.output_enable_count = metrics.output_enable_count;
    out.output_disable_count = metrics.output_disable_count;
    out.hotplug_event_count = metrics.hotplug_event_count;
    out.last_update_ns = metrics.last_update_ns;
    out.last_wait_duration_ns = metrics.last_wait_duration_ns;

    if (device.caps.flags & hermes_kms::cap_session_lifecycle) {
      out.session_lifecycle = true;
      out.bind_count = metrics.bind_count;
      out.bind_reject_count = metrics.bind_reject_count;
      out.unbind_count = metrics.unbind_count;
      out.binding_revoke_count = metrics.binding_revoke_count;
      out.cross_session_buffer_export_count = metrics.cross_session_buffer_export_count;

      // BIND selected this output atomically, so the status describes the
      // session just read rather than whatever the fd looked at before.
      hermes_kms::status_t status {};
      if (hermes_kms::get_status(device.fd, status)) {
        out.bound_fd_count = status.bound_fd_count;
      }
    }
    return out;
  }

  std::shared_ptr<EvdiBuffer> getEvdiBuffer(const std::string &display_name) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, display] : virtual_displays) {
      if (display.name == display_name && display.using_evdi) {
        return display.evdi_buffer;
      }
    }
    return nullptr;
  }

  void HermesKmsFrame::close() {
    for (auto &fd : dma_buf_fd) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
    if (sync_file_fd >= 0) {
      ::close(sync_file_fd);
      sync_file_fd = -1;
    }
  }

  void HermesKmsCursor::close() {
    for (auto &fd : dma_buf_fd) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
    if (sync_file_fd >= 0) {
      ::close(sync_file_fd);
      sync_file_fd = -1;
    }
  }

  int hermesKmsOpenCapture(const std::string &display_name) {
    int card_index = -1;
    int output_index = -1;
    uint64_t session_id = 0;
    std::array<uint64_t, 2> session_token {};
    auto token_guard = util::fail_guard([&session_token]() {
      hermes_kms::forget_secret(session_token.data(), sizeof(session_token));
    });
    {
      std::lock_guard<std::mutex> lock(vdisplay_mutex);
      for (const auto &[guid, display] : virtual_displays) {
        if (display.name == display_name && display.using_hermes_kms) {
          card_index = display.drm_card_index;
          output_index = display.output_index;
          session_id = display.session_id;
          session_token = display.session_token;
          break;
        }
      }
    }
    if (card_index < 0) {
      BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] No card mapping for capture of " << display_name;
      return -1;
    }

    // The broker intentionally has no access to private-seat primary cards.
    // Open the matching render node directly; the compositor opens cardN via
    // its private seatd connection.
    const int render_fd = hermes_kms::open_render_node_for_card_index(card_index);
    if (render_fd < 0) {
      BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] No accessible render node maps to card"
                       << card_index << ".";
      return -1;
    }

    if (output_index < 0 ||
        !hermes_kms::bind_session(
          render_fd,
          static_cast<uint32_t>(output_index),
          session_id,
          session_token
        )) {
      ::close(render_fd);
      return -1;
    }
    return render_fd;
  }

  bool getHermesKmsDisplayGeometry(const std::string &display_name,
                                   int &offset_x, int &offset_y,
                                   int &environment_width, int &environment_height) {
    // Each compositor answers this through its own output management, and there
    // is no generic one: KWin through kscreen-doctor, GNOME through
    // org.gnome.Mutter.DisplayConfig. Both guards already check which session
    // this is, so the order between them does not matter.
    if (kscreen::available()) {
      return kscreen::geometry(
        display_name,
        offset_x,
        offset_y,
        environment_width,
        environment_height
      );
    }

    if (mutter::available()) {
      const auto connector = virtual_display_connector_name(display_name);
      if (connector.empty()) {
        return false;
      }
      return mutter::geometry(
        connector,
        offset_x,
        offset_y,
        environment_width,
        environment_height
      );
    }

    return false;
  }

  bool hermesKmsCaptureSize(int render_fd, int &width, int &height) {
    if (render_fd < 0) {
      return false;
    }
    hermes_kms::status_t status {};
    if (::ioctl(render_fd, hermes_kms::ioctl_get_status, &status) != 0) {
      return false;
    }
    // Prefer the live scanout geometry; fall back to the requested mode.
    width = status.active_width ? static_cast<int>(status.active_width) : static_cast<int>(status.requested_width);
    height = status.active_height ? static_cast<int>(status.active_height) : static_cast<int>(status.requested_height);
    return width > 0 && height > 0;
  }

  bool hermesKmsAcquireFrame(int render_fd, uint64_t after_sequence,
                             uint32_t timeout_ms, HermesKmsFrame &out) {
    if (render_fd < 0) {
      errno = EBADF;
      return false;
    }

    out.close();

    // Block for a frame newer than what the caller last saw. A zero timeout
    // returns immediately; the caller then uses whatever frame is current.
    if (timeout_ms) {
      hermes_kms::wait_frame_t wait {};
      wait.after_sequence = after_sequence;
      wait.timeout_ms = timeout_ms;
      if (::ioctl(render_fd, hermes_kms::ioctl_wait_frame, &wait) != 0) {
        // ETIMEDOUT/EAGAIN: no new frame. Caller may retry or reuse the last one.
        return false;
      }
    }

    // Time only the acquire ioctl (DMA-BUF export), excluding the wait above,
    // so callers can measure the actual zero-copy cost.
    hermes_kms::acquire_frame_t frame {};
    const auto acquire_t0 = std::chrono::steady_clock::now();
    int acquire_ret = -1;
    for (unsigned int attempt = 0; attempt < 4; ++attempt) {
      frame = {};
      frame.flags = hermes_kms::frame_request_dmabuf | hermes_kms::frame_request_sync_file;
      for (auto &fd : frame.dma_buf_fd) {
        fd = -1;
      }
      frame.sync_file_fd = -1;
      acquire_ret = ::ioctl(render_fd, hermes_kms::ioctl_acquire_frame, &frame);
      if (acquire_ret == 0 || errno != ESTALE) {
        break;
      }
    }
    const auto acquire_t1 = std::chrono::steady_clock::now();
    out.acquire_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(acquire_t1 - acquire_t0).count();
    if (acquire_ret != 0) {
      // ENODATA: no scanout framebuffer yet (compositor has not committed).
      return false;
    }

    if (!(frame.flags & hermes_kms::frame_dmabuf_valid) ||
        !(frame.flags & hermes_kms::frame_sync_file_valid) ||
        !frame.plane_count || frame.plane_count > 4 || frame.sync_file_fd < 0) {
      // No usable DMA-BUFs; release anything we got back.
      for (auto fd : frame.dma_buf_fd) {
        if (fd >= 0) {
          ::close(fd);
        }
      }
      if (frame.sync_file_fd >= 0) {
        ::close(frame.sync_file_fd);
      }
      errno = EPROTO;
      return false;
    }

    out.width = static_cast<int>(frame.width);
    out.height = static_cast<int>(frame.height);
    out.fourcc = frame.format;
    out.modifier = frame.modifier;
    out.plane_count = frame.plane_count;
    out.sequence = frame.sequence;
    out.sync_file_fd = frame.sync_file_fd;
    for (uint32_t i = 0; i < 4; ++i) {
      out.dma_buf_fd[i] = frame.dma_buf_fd[i];
      out.pitch[i] = frame.pitch[i];
      out.offset[i] = frame.offset[i];
    }
    return true;
  }

  bool hermesKmsWaitUpdate(int render_fd, uint64_t after_frame_sequence,
                           uint64_t after_cursor_sequence, uint32_t timeout_ms,
                           HermesKmsUpdate &out) {
    out = {};
    if (render_fd < 0) {
      errno = EBADF;
      return false;
    }

    hermes_kms::wait_update_t wait {};
    wait.after_frame_sequence = after_frame_sequence;
    wait.after_cursor_sequence = after_cursor_sequence;
    wait.timeout_ms = timeout_ms;
    if (::ioctl(render_fd, hermes_kms::ioctl_wait_update, &wait) != 0) {
      return false;
    }

    constexpr uint64_t known_flags = hermes_kms::wait_update_frame_ready |
                                     hermes_kms::wait_update_cursor_ready;
    out.frame_ready = (wait.flags & hermes_kms::wait_update_frame_ready) != 0;
    out.cursor_ready = (wait.flags & hermes_kms::wait_update_cursor_ready) != 0;
    out.frame_sequence = wait.frame_sequence;
    out.cursor_sequence = wait.cursor_sequence;
    if ((wait.flags & ~known_flags) ||
        out.frame_ready != (out.frame_sequence > after_frame_sequence) ||
        out.cursor_ready != (out.cursor_sequence > after_cursor_sequence) ||
        (!out.frame_ready && !out.cursor_ready)) {
      errno = EPROTO;
      return false;
    }
    return true;
  }

  bool hermesKmsAcquireCursor(int render_fd, bool request_buffer,
                              HermesKmsCursor &out) {
    if (render_fd < 0) {
      errno = EBADF;
      return false;
    }

    out.close();
    hermes_kms::acquire_cursor_t cursor {};
    int acquire_ret = -1;
    for (unsigned int attempt = 0; attempt < 4; ++attempt) {
      cursor = {};
      if (request_buffer) {
        cursor.flags = hermes_kms::cursor_request_dmabuf |
                       hermes_kms::cursor_request_sync_file;
      }
      for (auto &fd : cursor.dma_buf_fd) {
        fd = -1;
      }
      cursor.sync_file_fd = -1;
      acquire_ret = ::ioctl(render_fd, hermes_kms::ioctl_acquire_cursor, &cursor);
      if (acquire_ret == 0 || errno != ESTALE) {
        break;
      }
    }
    if (acquire_ret != 0) {
      return false;
    }

    const auto close_reply_fds = [&cursor]() {
      for (auto &fd : cursor.dma_buf_fd) {
        if (fd >= 0) {
          ::close(fd);
          fd = -1;
        }
      }
      if (cursor.sync_file_fd >= 0) {
        ::close(cursor.sync_file_fd);
        cursor.sync_file_fd = -1;
      }
    };

    const bool metadata_valid = (cursor.flags & hermes_kms::cursor_metadata_valid) != 0;
    const bool buffer_valid = (cursor.flags & hermes_kms::cursor_buffer_valid) != 0;
    const bool dmabuf_valid = (cursor.flags & hermes_kms::cursor_dmabuf_valid) != 0;
    const bool sync_file_valid = (cursor.flags & hermes_kms::cursor_sync_file_valid) != 0;
    const bool invalid_layout =
      cursor.plane_count > 4 ||
      (buffer_valid && (!cursor.plane_count || !cursor.width || !cursor.height || !cursor.format)) ||
      (request_buffer && buffer_valid &&
       (!dmabuf_valid || !sync_file_valid || cursor.sync_file_fd < 0));
    if (!metadata_valid || invalid_layout) {
      close_reply_fds();
      errno = EPROTO;
      return false;
    }
    if (request_buffer && buffer_valid) {
      for (uint32_t i = 0; i < cursor.plane_count; ++i) {
        if (cursor.dma_buf_fd[i] < 0 || !cursor.pitch[i]) {
          close_reply_fds();
          errno = EPROTO;
          return false;
        }
      }
    }

    out.visible = (cursor.flags & hermes_kms::cursor_visible) != 0;
    out.position_valid = (cursor.flags & hermes_kms::cursor_position_valid) != 0;
    out.geometry_valid = (cursor.flags & hermes_kms::cursor_geometry_valid) != 0;
    out.buffer_valid = buffer_valid;
    out.position_x = cursor.position_x;
    out.position_y = cursor.position_y;
    out.crtc_x = cursor.crtc_x;
    out.crtc_y = cursor.crtc_y;
    out.crtc_w = cursor.crtc_w;
    out.crtc_h = cursor.crtc_h;
    out.src_x = cursor.src_x;
    out.src_y = cursor.src_y;
    out.src_w = cursor.src_w;
    out.src_h = cursor.src_h;
    out.hotspot_x = cursor.hotspot_x;
    out.hotspot_y = cursor.hotspot_y;
    out.width = cursor.width;
    out.height = cursor.height;
    out.fourcc = cursor.format;
    out.modifier = cursor.modifier;
    out.plane_count = cursor.plane_count;
    out.sync_file_fd = cursor.sync_file_fd;
    out.sequence = cursor.sequence;
    out.image_sequence = cursor.image_sequence;
    cursor.sync_file_fd = -1;
    for (uint32_t i = 0; i < 4; ++i) {
      out.dma_buf_fd[i] = cursor.dma_buf_fd[i];
      out.pitch[i] = cursor.pitch[i];
      out.offset[i] = cursor.offset[i];
      cursor.dma_buf_fd[i] = -1;
    }
    return true;
  }

}  // namespace VDISPLAY
