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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

// third-party includes
#include <nlohmann/json.hpp>

// platform includes
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// local includes
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
  static std::thread watchdog_thread;
  static bool evdi_available = false;
  static bool exclusive_virtual_display_active = false;
  static std::atomic<bool> virtual_display_capture_fallback_active {false};
  static std::string evdi_library_version;


  enum class VirtualDisplayBackend {
    EVDI,
    HERMES_KMS,
  };

  static VirtualDisplayBackend selected_backend() {
    return config::video.virtual_display_backend == "hermes_kms" ? VirtualDisplayBackend::HERMES_KMS : VirtualDisplayBackend::EVDI;
  }

  static const char *backend_name(VirtualDisplayBackend backend) {
    return backend == VirtualDisplayBackend::HERMES_KMS ? "Hermes-KMS" : "EVDI";
  }

  namespace hermes_kms {
    constexpr uint32_t uapi_version = 7;
    constexpr size_t name_len = 32;

    constexpr uint64_t cap_virtual_output = 1ULL << 0;
    constexpr uint64_t cap_output_control = 1ULL << 1;
    constexpr uint64_t cap_frame_acquire = 1ULL << 5;
    constexpr uint64_t cap_dmabuf_export = 1ULL << 6;
    constexpr uint64_t cap_output_identity = 1ULL << 7;
    constexpr uint64_t cap_session_owner = 1ULL << 8;
    constexpr uint64_t cap_frame_wait = 1ULL << 9;
    constexpr uint64_t cap_metrics = 1ULL << 10;
    constexpr uint64_t cap_zero_copy_target = 1ULL << 33;
    constexpr uint64_t cap_sync_file = 1ULL << 35;

    constexpr uint64_t status_connected = 1ULL << 1;

    constexpr uint32_t set_output_connected = 1U << 0;
    constexpr uint32_t set_output_owner_assigned = 1U << 1;

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
      uint32_t reserved0;
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
      uint64_t framebuffer_modifier;
      uint64_t session_id;
      int32_t owner_pid;
      uint32_t reserved0;
      uint64_t reserved[6];
    };

    struct identity_t {
      char driver_name[name_len];
      char output_name[name_len];
      char connector_name[name_len];
      uint32_t connector_id;
      uint32_t crtc_id;
      uint32_t plane_id;
      uint32_t encoder_id;
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
      uint64_t reserved[8];
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
      uint64_t reserved[16];
    };

    // Frame request/result flags (must match the UAPI header).
    constexpr uint64_t frame_request_dmabuf = 1ULL << 0;
    constexpr uint64_t frame_dmabuf_valid = 1ULL << 2;
    constexpr uint64_t frame_request_sync_file = 1ULL << 5;
    constexpr uint64_t wait_frame_ready = 1ULL << 0;

    constexpr unsigned long ioctl_get_version = DRM_IOR(DRM_COMMAND_BASE + 0x00, version_t);
    constexpr unsigned long ioctl_get_caps = DRM_IOR(DRM_COMMAND_BASE + 0x01, caps_t);
    constexpr unsigned long ioctl_get_status = DRM_IOR(DRM_COMMAND_BASE + 0x02, status_t);
    constexpr unsigned long ioctl_set_output = DRM_IOWR(DRM_COMMAND_BASE + 0x03, set_output_t);
    constexpr unsigned long ioctl_acquire_frame = DRM_IOWR(DRM_COMMAND_BASE + 0x04, acquire_frame_t);
    constexpr unsigned long ioctl_get_identity = DRM_IOR(DRM_COMMAND_BASE + 0x05, identity_t);
    constexpr unsigned long ioctl_wait_frame = DRM_IOWR(DRM_COMMAND_BASE + 0x06, wait_frame_t);
    constexpr unsigned long ioctl_get_metrics = DRM_IOR(DRM_COMMAND_BASE + 0x07, metrics_t);

    struct device_t {
      int fd {-1};
      int card_index {-1};
      version_t version {};
      caps_t caps {};
      identity_t identity {};
    };

    static std::string cstr(const char *value) {
      return value && value[0] ? std::string {value, strnlen(value, name_len)} : std::string {};
    }

    static bool is_card_node(const fs::path &path) {
      const auto name = path.filename().string();
      return name.rfind("card", 0) == 0 && name.find('-') == std::string::npos;
    }

    static int card_index_from_path(const fs::path &path) {
      const auto name = path.filename().string();
      if (name.size() <= 4 || name.substr(4).find_first_not_of("0123456789") != std::string::npos) {
        return -1;
      }
      return std::stoi(name.substr(4));
    }

    static void close_device(device_t &device) {
      if (device.fd >= 0) {
        ::close(device.fd);
        device.fd = -1;
      }
    }

    // Open the render node that belongs to the same DRM device as @card_fd.
    // The Hermes consumer must use the render node (not the primary card node)
    // so it never becomes DRM master: the compositor (KWin/GNOME) owns the
    // primary node and drives the modeset, while we pull frames through the
    // render node. All Hermes ioctls are DRM_RENDER_ALLOW, so they work here.
    // Returns a fd >= 0 on success, or -1 (caller may fall back to the card fd).
    static int open_render_node(int card_fd) {
      if (card_fd < 0) {
        return -1;
      }

      drmDevicePtr dev = nullptr;
      if (drmGetDevice2(card_fd, 0, &dev) != 0 || !dev) {
        return -1;
      }

      int fd = -1;
      if ((dev->available_nodes & (1 << DRM_NODE_RENDER)) && dev->nodes[DRM_NODE_RENDER]) {
        fd = ::open(dev->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
      }
      drmFreeDevice(&dev);
      return fd;
    }

    static bool has_required_caps(uint64_t flags) {
      constexpr uint64_t required = cap_virtual_output | cap_output_control | cap_frame_acquire |
                                    cap_dmabuf_export | cap_output_identity | cap_session_owner |
                                    cap_frame_wait | cap_zero_copy_target | cap_sync_file;
      return (flags & required) == required;
    }

    static bool open_device(device_t &out, bool log_failures) {
      try {
        for (const auto &entry : fs::directory_iterator("/dev/dri")) {
          if (!is_card_node(entry.path())) {
            continue;
          }

          device_t candidate {};
          candidate.fd = ::open(entry.path().c_str(), O_RDWR | O_CLOEXEC);
          if (candidate.fd < 0) {
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

          candidate.card_index = card_index_from_path(entry.path());
          if (candidate.version.uapi_version < uapi_version) {
            if (log_failures) {
              BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] UAPI " << candidate.version.uapi_version
                               << " is too old; need " << uapi_version << ".";
            }
            close_device(candidate);
            return false;
          }

          if (::ioctl(candidate.fd, ioctl_get_caps, &candidate.caps) != 0 || !has_required_caps(candidate.caps.flags)) {
            if (log_failures) {
              BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Driver is present but does not expose the required streaming capabilities.";
            }
            close_device(candidate);
            return false;
          }

          if (::ioctl(candidate.fd, ioctl_get_identity, &candidate.identity) != 0) {
            std::strncpy(candidate.identity.driver_name, "hermes-kms", name_len - 1);
            std::strncpy(candidate.identity.output_name, "HERMES-1", name_len - 1);
          }

          out = candidate;
          return true;
        }
      } catch (const std::exception &exception) {
        if (log_failures) {
          BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Could not enumerate DRM devices: " << exception.what();
        }
      }

      if (log_failures) {
        BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] No Hermes-KMS DRM card found. Load hermes_kms and install its udev rules.";
      }
      return false;
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
        if (!is_card_node(entry.path())) {
          continue;
        }

        device_t candidate {};
        candidate.fd = ::open(entry.path().c_str(), O_RDWR | O_CLOEXEC);
        if (candidate.fd < 0) {
          continue;
        }

        if (::ioctl(candidate.fd, ioctl_get_version, &candidate.version) != 0 ||
            cstr(candidate.version.driver_name) != "hermes-kms") {
          close_device(candidate);
          continue;
        }

        // Found the Hermes-KMS device; from here the outcome is definitive.
        out.card_index = card_index_from_path(entry.path());
        out.uapi_version = candidate.version.uapi_version;
        out.driver_version = std::to_string(candidate.version.driver_major) + "." +
                             std::to_string(candidate.version.driver_minor) + "." +
                             std::to_string(candidate.version.driver_patch);

        if (candidate.version.uapi_version < uapi_version) {
          out.result = probe_result::uapi_too_old;
        } else if (::ioctl(candidate.fd, ioctl_get_caps, &candidate.caps) != 0 ||
                   !has_required_caps(candidate.caps.flags)) {
          out.result = probe_result::missing_caps;
        } else {
          out.result = probe_result::ok;
        }
        close_device(candidate);
        return out;
      }
      return out;
    }

    static bool set_output(int fd, bool enabled, uint32_t width, uint32_t height, uint32_t refresh_hz, uint64_t &session_id) {
      set_output_t request {};
      request.enabled = enabled ? 1U : 0U;
      request.width = width;
      request.height = height;
      request.refresh_hz = refresh_hz;
      request.session_id = session_id;

      if (::ioctl(fd, ioctl_set_output, &request) != 0) {
        BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] SET_OUTPUT failed: " << std::strerror(errno);
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

    static bool get_metrics(int fd, metrics_t &metrics) {
      std::memset(&metrics, 0, sizeof(metrics));
      return ::ioctl(fd, ioctl_get_metrics, &metrics) == 0;
    }
  }  // namespace hermes_kms

  // Virtual display info structure
  struct VirtualDisplayInfo {
    std::string name;
    std::string guid_str;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    int device_index;      // EVDI device index
    int drm_card_index;    // DRM card index assigned by the kernel
    evdi_handle handle;    // EVDI handle
    int drm_fd;            // DRM fd for card
    bool active;
    bool using_evdi;       // true while an EVDI display is connected
    bool using_hermes_kms; // true while a Hermes-KMS output owner fd is held
    std::string connector_name;
    uint64_t session_id;
    std::shared_ptr<EvdiBuffer> evdi_buffer;
    int evdi_buffer_id;
  };

  static std::map<std::string, VirtualDisplayInfo> virtual_displays;
  static std::atomic<bool> evdi_events_running {false};
  static std::thread evdi_events_thread;
  static std::string evdi_connector_name(int card_index);

  namespace kscreen {
    struct output_t {
      std::string name;
      bool connected {false};
      bool enabled {false};
      int priority {0};
    };

    struct layout_t {
      std::string original_primary;
      std::string virtual_output;
      bool physical_output_disabled {false};
    };

    static std::mutex layouts_mutex;
    static std::map<std::string, layout_t> layouts;

    static bool safe_output_name(const std::string &name) {
      return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
      });
    }

    static bool available() {
      const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
      if (!desktop || std::string_view {desktop}.find("KDE") == std::string_view::npos) {
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

    static std::set<std::string> connected_output_names() {
      std::set<std::string> result;
      for (const auto &output : outputs()) {
        if (output.connected) {
          result.insert(output.name);
        }
      }
      return result;
    }

    static std::string primary_output() {
      for (const auto &output : outputs()) {
        if (output.connected && output.enabled && output.priority == 1) {
          return output.name;
        }
      }
      return {};
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

    static bool write_recovery_state(const std::string &output) {
      if (!safe_output_name(output)) {
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
        file << output << '\n';
      }

      std::error_code ec;
      fs::rename(temporary_path, path, ec);
      if (ec) {
        fs::remove(temporary_path, ec);
        return false;
      }
      return true;
    }

    static std::string read_recovery_state() {
      const auto path = recovery_state_file();
      if (path.empty()) {
        return {};
      }

      std::ifstream file {path};
      std::string output;
      std::getline(file, output);
      while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back()))) {
        output.pop_back();
      }
      return safe_output_name(output) ? output : std::string {};
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
      const auto output = read_recovery_state();
      if (output.empty()) {
        return;
      }

      BOOST_LOG(info) << "[VDISPLAY/KScreen] Recovering physical output left disabled by a previous session: " << output;
      run_layout_command("kscreen-doctor output." + output + ".enable output." + output + ".priority.1");
      clear_recovery_state();
    }

    static bool activate_evdi_output(
      const std::string &display_name,
      const std::set<std::string> &outputs_before,
      const std::string &original_primary,
      const std::string &connector_name,
      const char *backend_label,
      bool disable_physical
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
      // We deliberately do NOT touch the physical output's priority or disable
      // it here: that is a SESSION-level action and must only happen once the
      // stream actually starts (see make_exclusive), and only in isolated mode.
      // Otherwise creating the display would steal the user's primary monitor
      // before any session is live, blanking the physical screen prematurely.
      const bool can_manage_original = safe_output_name(original_primary) && original_primary != virtual_output;
      if (!run_layout_command("kscreen-doctor output." + virtual_output + ".enable")) {
        return false;
      }

      std::lock_guard<std::mutex> lock(layouts_mutex);
      layouts[display_name] = {
        .original_primary = can_manage_original ? original_primary : std::string {},
        .virtual_output = virtual_output,
        .physical_output_disabled = false,
      };
      BOOST_LOG(info) << "[VDISPLAY/KScreen] Enabled " << backend_label << " output " << virtual_output
                      << " (physical output left untouched until the session starts)";
      // disable_physical is intentionally unused here; the physical layout is
      // managed at session start in make_exclusive().
      (void) disable_physical;
      return true;
    }

    static bool is_active(const std::string &display_name) {
      std::lock_guard<std::mutex> lock(layouts_mutex);
      return layouts.find(display_name) != layouts.end();
    }

    // Called at SESSION START (only when isolated mode is on) to hand the
    // desktop over to the virtual output: make it primary and disable the
    // physical monitor. This is the moment the physical screen is allowed to go
    // dark — never before. restore() reverses it when the session ends.
    static bool make_exclusive(const std::string &display_name) {
      std::lock_guard<std::mutex> lock(layouts_mutex);
      const auto it = layouts.find(display_name);
      if (it == layouts.end() || it->second.physical_output_disabled || it->second.original_primary.empty()) {
        return it != layouts.end();
      }
      // Promote the virtual output to primary and disable the physical one in a
      // single atomic kscreen-doctor call so the session never lands on a
      // transient no-primary layout.
      std::string command = "kscreen-doctor output." + it->second.virtual_output + ".priority.1"
        " output." + it->second.original_primary + ".disable";
      if (!run_layout_command(command)) {
        return false;
      }
      if (!write_recovery_state(it->second.original_primary)) {
        BOOST_LOG(warning) << "[VDISPLAY/KScreen] Could not write monitor recovery state; "
                           << "a crash may leave the physical output disabled.";
      }
      it->second.physical_output_disabled = true;
      return true;
    }

    static void restore(const std::string &display_name) {
      std::lock_guard<std::mutex> lock(layouts_mutex);
      const auto it = layouts.find(display_name);
      if (it == layouts.end()) {
        return;
      }
      // Re-enable the physical output as primary. Do this in the same command
      // that drops the virtual output's priority so the desktop never sits with
      // two primaries or none. The virtual connector itself disappears when the
      // display is torn down, so we don't need to .disable it explicitly, but
      // restoring the physical priority is what brings the screen back.
      if (!it->second.original_primary.empty()) {
        std::string command = "kscreen-doctor"
          " output." + it->second.original_primary + ".enable"
          " output." + it->second.original_primary + ".priority.1";
        run_layout_command(command);
      }
      clear_recovery_state();
      layouts.erase(it);
    }
  }  // namespace kscreen

  // GNOME / Mutter output management. Mutter does not speak kscreen-doctor or
  // the wlroots output-management protocol; it exposes
  // org.gnome.Mutter.DisplayConfig over D-Bus. We talk to it via `gdbus` (always
  // present on GNOME) to avoid a libdbus build dependency.
  //
  // This backend is intentionally conservative: with the Hermes-KMS module
  // loaded `initial_enabled=1`, Mutter typically adopts the hotplugged HERMES-1
  // connector on its own, so the critical step is to *verify* that the virtual
  // output is present and active in Mutter's current state rather than to push a
  // full ApplyMonitorsConfig (which is all-or-nothing and risky to build blind).
  namespace mutter {
    static bool available() {
      const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
      if (!desktop) {
        return false;
      }
      const std::string_view d {desktop};
      if (d.find("GNOME") == std::string_view::npos && d.find("gnome") == std::string_view::npos) {
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

    // Returns true if Mutter's current monitor state references the given DRM
    // connector name (e.g. "HERMES-1"), meaning the compositor has adopted the
    // virtual output and is driving it.
    static bool output_present(const std::string &connector_name) {
      if (!available() || connector_name.empty()) {
        return false;
      }
      const std::string command =
        "gdbus call --session "
        "--dest org.gnome.Mutter.DisplayConfig "
        "--object-path /org/gnome/Mutter/DisplayConfig "
        "--method org.gnome.Mutter.DisplayConfig.GetCurrentState 2>/dev/null";
      const auto state = command_output(command.c_str());
      if (state.empty()) {
        return false;
      }
      // GetCurrentState embeds the connector name as a string in its reply; a
      // substring match is sufficient to know Mutter is aware of the output.
      return state.find("'" + connector_name + "'") != std::string::npos ||
             state.find('"' + connector_name + '"') != std::string::npos;
    }
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
        output_layout_backend = "wlr-output-management";
      }
    }
#endif
    // GNOME/Mutter: Hermes cannot push a layout, but it can verify Mutter has
    // adopted the virtual output. Report this so diagnostics make the limited
    // (verify-only, no exclusive layout) support explicit.
    if (window_system == window_system_e::WAYLAND && output_layout_backend == "unavailable" &&
        mutter::available()) {
      output_layout_backend = "mutter-displayconfig (verify-only)";
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
          .device_index = display.device_index,
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
      .required_uapi_version = hermes_kms::uapi_version,
      .driver_version = probe.driver_version,
      .running_kernel = running_kernel_release(),
      .dkms_kernels = dkms_kernels("hermes-kms"),
      .active_displays = {},
    };

    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, display] : virtual_displays) {
      if (display.using_hermes_kms && display.active) {
        status.active_displays.push_back({
          .name = display.name,
          .device_index = display.device_index,
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
    const int result = evdi.add_device();
    // libevdi returns 0 when the sysfs request succeeds. It does not return
    // the device index, so only negative values are failures.
    if (result < 0) {
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
    uint32_t pixel_clock_khz;

    if (width == 3840 && height == 2160) {
      // 4K UHD @ 60Hz
      h_blank = 560;
      v_blank = 90;
      h_front = 176;
      h_sync = 88;
      v_front = 8;
      v_sync = 10;
      // 4400 * 2250 * 60 = 594 MHz. 533.25 MHz produces ~53.86 Hz,
      // causing stutter when the stream and display target 60 Hz.
      pixel_clock_khz = 594000; // 594 MHz
    } else if (width == 2560 && height == 1440) {
      // 1440p @ 60Hz
      h_blank = 160;
      v_blank = 44;
      h_front = 48;
      h_sync = 32;
      v_front = 3;
      v_sync = 5;
      pixel_clock_khz = 241500; // 241.5 MHz
    } else if (width == 1920 && height == 1080) {
      // 1080p @ 60Hz
      h_blank = 280;
      v_blank = 45;
      h_front = 88;
      h_sync = 44;
      v_front = 4;
      v_sync = 5;
      pixel_clock_khz = 148500; // 148.5 MHz
    } else if (width == 1280 && height == 720) {
      // 720p @ 60Hz
      h_blank = 370;
      v_blank = 30;
      h_front = 110;
      h_sync = 40;
      v_front = 5;
      v_sync = 5;
      pixel_clock_khz = 74250; // 74.25 MHz
    } else if (width == 1280 && height == 800) {
      // Steam Deck native mode, CVT-RB at 60.0003 Hz.
      h_blank = 160;
      v_blank = 23;
      h_front = 48;
      h_sync = 32;
      v_front = 3;
      v_sync = 6;
      pixel_clock_khz = 71107;
    } else {
      // Generic calculation for other resolutions
      // Using simplified CVT formula
      double h_period = (1000000.0 / refresh_rate - 550) / (height + 3);
      double h_total = width + (width * 0.15); // ~15% horizontal blanking
      pixel_clock_khz = static_cast<uint32_t>((h_total / h_period) * 1000);
      h_blank = static_cast<uint32_t>(width * 0.15);
      v_blank = 45;
      h_front = h_blank / 4;
      h_sync = h_blank / 4;
      v_front = 3;
      v_sync = 5;
    }

    uint32_t h_active = width;
    uint32_t v_active = height;

    // Pixel clock in 10kHz units
    uint16_t pixel_clock = pixel_clock_khz / 10;

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
    kscreen::recover_on_startup();

    if (backend == VirtualDisplayBackend::HERMES_KMS) {
      evdi_available = false;
      unload_evdi_library();
      if (!hermes_kms::available()) {
        BOOST_LOG(warning) << "[VDISPLAY/Hermes-KMS] Hermes-KMS is unavailable; virtual-display sessions are disabled.";
        driver_status = DRIVER_STATUS::NOT_SUPPORTED;
        return driver_status;
      }

      driver_status = DRIVER_STATUS::OK;
      BOOST_LOG(info) << "[VDISPLAY/Hermes-KMS] Hermes-KMS available - experimental zero-copy virtual display supported.";
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
    BOOST_LOG(info) << "[VDISPLAY] Linux virtual display driver initialized successfully.";

    return driver_status;
  }

  void closeVDisplayDevice() {
    BOOST_LOG(info) << "[VDISPLAY] Closing Linux virtual display driver...";

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
      }
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
    const uuid_util::uuid_t &guid
  ) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (driver_status != DRIVER_STATUS::OK) {
      BOOST_LOG(error) << "[VDISPLAY] Driver not initialized.";
      return "";
    }

    std::string guid_str = guid.string();
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
    vdinfo.width = width;
    vdinfo.height = height;
    vdinfo.fps = fps;
    vdinfo.device_index = -1;
    vdinfo.drm_card_index = -1;
    vdinfo.handle = nullptr;
    vdinfo.drm_fd = -1;
    vdinfo.active = true;
    vdinfo.using_evdi = false;
    vdinfo.using_hermes_kms = false;
    vdinfo.session_id = 0;
    vdinfo.evdi_buffer_id = 0;

    const auto backend = selected_backend();
    const auto outputs_before = kscreen::connected_output_names();
    const auto original_primary = kscreen::primary_output();

    if (backend == VirtualDisplayBackend::HERMES_KMS) {
      hermes_kms::device_t device {};
      if (hermes_kms::open_device(device, true)) {
        // Switch to the render node before owning the output. Holding the
        // primary card node would make this process the DRM master and block
        // the compositor (KWin/GNOME) from driving the modeset (EBUSY), so the
        // virtual output would never be scanned out. The render node carries
        // all Hermes ioctls (DRM_RENDER_ALLOW) without taking master, letting
        // the compositor own the card and us pull frames. Fall back to the card
        // node only if the render node cannot be opened.
        int render_fd = hermes_kms::open_render_node(device.fd);
        if (render_fd >= 0) {
          hermes_kms::close_device(device);
          device.fd = render_fd;
        } else {
          BOOST_LOG(warning) << "[VDISPLAY/Hermes-KMS] No render node available; using the card node "
                                "(the compositor may fail to take DRM master).";
        }

        uint64_t session_id = 0;
        if (hermes_kms::set_output(device.fd, true, width, height, fps_hz, session_id)) {
          hermes_kms::status_t status {};
          hermes_kms::get_status(device.fd, status);
          vdinfo.drm_fd = device.fd;
          device.fd = -1;
          vdinfo.drm_card_index = device.card_index;
          vdinfo.session_id = session_id;
          vdinfo.using_hermes_kms = true;
          vdinfo.connector_name = hermes_kms::cstr(device.identity.connector_name);
          display_name = hermes_kms::cstr(device.identity.output_name);
          if (display_name.empty()) {
            display_name = "HERMES-1";
          }
          vdinfo.name = display_name;
          BOOST_LOG(info) << "[VDISPLAY/Hermes-KMS] Connected " << display_name
                          << " connector=" << vdinfo.connector_name
                          << " card=" << vdinfo.drm_card_index
                          << " session=" << vdinfo.session_id
                          << " requested=" << status.requested_width << 'x' << status.requested_height
                          << '@' << status.requested_refresh_hz
                          << " flags=0x" << std::hex << status.flags << std::dec;
          if (!vdinfo.connector_name.empty()) {
            kscreen::activate_evdi_output(
              vdinfo.name,
              outputs_before,
              original_primary,
              vdinfo.connector_name,
              "Hermes-KMS",
              config::video.isolated_virtual_display_option
            );
          }
        }
        hermes_kms::close_device(device);
      }
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
              original_primary,
              evdi_connector_name(vdinfo.drm_card_index),
              "EVDI",
              config::video.isolated_virtual_display_option
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

    virtual_displays[guid_str] = vdinfo;
    if (vdinfo.using_evdi) {
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
      hermes_kms::set_output(vdinfo.drm_fd, false, 0, 0, 0, vdinfo.session_id);
    }
    kscreen::restore(vdinfo.name);

    if (vdinfo.drm_fd >= 0) {
      ::close(vdinfo.drm_fd);
    }

    virtual_displays.erase(it);
    virtual_display_capture_fallback_active = false;

    BOOST_LOG(info) << "[VDISPLAY] Virtual display removed successfully.";
    return true;
  }

  int changeDisplaySettings(const char *deviceName, int width, int height, int refresh_rate) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

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
    for (auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.name == deviceName) {
        // createVirtualDisplay() is normally followed by this call with the
        // same values. Reconnecting EVDI in that case causes an avoidable
        // compositor modeset and a burst of capture reinitializations.
        if (vdinfo.width == static_cast<uint32_t>(width) &&
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
        }

        BOOST_LOG(info) << "[VDISPLAY] Display settings updated successfully.";
        return 0;
      }
    }

    BOOST_LOG(debug) << "[VDISPLAY] Display not found: " << deviceName;
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

  void setVirtualDisplayCaptureFallbackActive(bool active) {
    virtual_display_capture_fallback_active = active;
  }

  static bool virtual_display_mode(const std::string &display_name, int &width, int &height, int &refresh_rate) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, display] : virtual_displays) {
      if (display.name == display_name && (display.using_evdi || display.using_hermes_kms)) {
        width = static_cast<int>(display.width);
        height = static_cast<int>(display.height);
        refresh_rate = static_cast<int>(display.fps);
        return true;
      }
    }
    return false;
  }

  static std::string virtual_display_connector_name(const std::string &display_name) {
    if (auto connector = getHermesKmsConnectorName(display_name); !connector.empty()) {
      return connector;
    }
    return getEvdiConnectorName(display_name);
  }

  bool activateVirtualDisplayOutput(const std::string &displayName) {
    if (window_system == window_system_e::WAYLAND && kscreen::is_active(displayName)) {
      return true;
    }

    const auto connector = virtual_display_connector_name(displayName);
    if (connector.empty()) {
      BOOST_LOG(warning) << "[VDISPLAY] Cannot activate virtual output: DRM connector was not found.";
      return false;
    }

#ifdef SUNSHINE_BUILD_WAYLAND
    if (window_system == window_system_e::WAYLAND) {
      // GNOME/Mutter exposes neither kscreen-doctor nor wlr-output-management.
      // It typically adopts the hotplugged HERMES-1 connector on its own (the
      // module is loaded initial_enabled=1), so confirm Mutter is driving the
      // virtual output via its D-Bus DisplayConfig before deciding capture is
      // safe. We do not push a layout to Mutter here.
      if (mutter::available()) {
        if (mutter::output_present(connector)) {
          BOOST_LOG(info) << "[VDISPLAY] Mutter has adopted virtual output " << connector
                          << "; capturing it directly.";
          return true;
        }
        BOOST_LOG(warning) << "[VDISPLAY] GNOME/Mutter session detected but it has not adopted "
                           << connector << ". Hermes cannot push a display layout to Mutter; "
                           << "the virtual display may need to be enabled in GNOME Settings, "
                           << "or use a KDE/wlroots session for automatic activation.";
        return false;
      }

      int width = 0;
      int height = 0;
      int refresh_rate = 0;
      if (!virtual_display_mode(displayName, width, height, refresh_rate)) {
        return false;
      }
      const bool activated = wl::configure_virtual_output(connector, width, height, refresh_rate, false);
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

  HermesKmsMetrics getHermesKmsMetrics() {
    HermesKmsMetrics out {};

    hermes_kms::device_t device {};
    if (!hermes_kms::open_device(device, false)) {
      return out;
    }
    auto guard = util::fail_guard([&device]() { hermes_kms::close_device(device); });

    // The driver may be present without the metrics capability (older build).
    if (!(device.caps.flags & hermes_kms::cap_metrics)) {
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

  int hermesKmsOpenCapture(const std::string &display_name) {
    const int card_index = getHermesKmsCardIndex(display_name);
    if (card_index < 0) {
      BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] No card mapping for capture of " << display_name;
      return -1;
    }

    const std::string card_path = "/dev/dri/card" + std::to_string(card_index);
    const int card_fd = ::open(card_path.c_str(), O_RDWR | O_CLOEXEC);
    if (card_fd < 0) {
      BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Could not open " << card_path << " for capture.";
      return -1;
    }

    // Switch to the render node so capture never contends for DRM master with
    // the compositor that owns the card.
    const int render_fd = hermes_kms::open_render_node(card_fd);
    ::close(card_fd);
    if (render_fd < 0) {
      BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Card has no render node; cannot capture without DRM master.";
      return -1;
    }
    return render_fd;
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
      return false;
    }

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

    hermes_kms::acquire_frame_t frame {};
    frame.flags = hermes_kms::frame_request_dmabuf | hermes_kms::frame_request_sync_file;
    for (auto &fd : frame.dma_buf_fd) {
      fd = -1;
    }
    frame.sync_file_fd = -1;

    // Time only the acquire ioctl (DMA-BUF export), excluding the wait above,
    // so callers can measure the actual zero-copy cost.
    const auto acquire_t0 = std::chrono::steady_clock::now();
    const int acquire_ret = ::ioctl(render_fd, hermes_kms::ioctl_acquire_frame, &frame);
    const auto acquire_t1 = std::chrono::steady_clock::now();
    out.acquire_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(acquire_t1 - acquire_t0).count();
    if (acquire_ret != 0) {
      // ENODATA: no scanout framebuffer yet (compositor has not committed).
      return false;
    }

    if (!(frame.flags & hermes_kms::frame_dmabuf_valid) || !frame.plane_count || frame.plane_count > 4) {
      // No usable DMA-BUFs; release anything we got back.
      for (auto fd : frame.dma_buf_fd) {
        if (fd >= 0) {
          ::close(fd);
        }
      }
      if (frame.sync_file_fd >= 0) {
        ::close(frame.sync_file_fd);
      }
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

}  // namespace VDISPLAY
