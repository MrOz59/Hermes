/**
 * @file src/platform/linux/wayland.h
 * @brief Declarations for Wayland capture.
 */
#pragma once

// standard includes
#include <bitset>
#include <cstdint>
#include <map>
#include <vector>

#ifdef SUNSHINE_BUILD_WAYLAND
  #include <ext-image-capture-source-v1.h>
  #include <ext-image-copy-capture-v1.h>
  #include <linux-dmabuf-unstable-v1.h>
  #include <wlr-output-management-unstable-v1.h>
  #include <wlr-screencopy-unstable-v1.h>
  #include <xdg-output-unstable-v1.h>
#endif

// local includes
#include "graphics.h"

// Declared at global scope so a translation unit that never includes <gbm.h>
// agrees with one that does about the type of dmabuf_t's members; otherwise
// `struct gbm_device *` below declares a wl::gbm_device of its own and LTO
// reports the mismatch.
struct gbm_device;
struct gbm_bo;

/**
 * The classes defined in this macro block should only be used by
 * cpp files whose compilation depends on SUNSHINE_BUILD_WAYLAND
 */
#ifdef SUNSHINE_BUILD_WAYLAND

namespace wl {
  using display_internal_t = util::safe_ptr<wl_display, wl_display_disconnect>;

  class frame_t {
  public:
    frame_t();
    void destroy();

    egl::surface_descriptor_t sd;
  };

  class dmabuf_t {
  public:
    enum status_e {
      WAITING,  ///< Waiting for a frame
      READY,  ///< Frame is ready
      REINIT,  ///< Reinitialize the frame
    };

    dmabuf_t();
    ~dmabuf_t();

    dmabuf_t(dmabuf_t &&) = delete;
    dmabuf_t(const dmabuf_t &) = delete;
    dmabuf_t &operator=(const dmabuf_t &) = delete;
    dmabuf_t &operator=(dmabuf_t &&) = delete;

    void listen(zwlr_screencopy_manager_v1 *screencopy_manager, zwp_linux_dmabuf_v1 *dmabuf_interface, wl_output *output, bool blend_cursor = false);
    static void buffer_params_created(void *data, struct zwp_linux_buffer_params_v1 *params, struct wl_buffer *wl_buffer);
    static void buffer_params_failed(void *data, struct zwp_linux_buffer_params_v1 *params);

    /**
     * @brief Open an ext-image-copy-capture session on an output.
     *
     * Unlike screencopy, which is asked for one frame at a time, this protocol
     * negotiates buffer size, format and modifiers once and then hands out
     * frames from that session - so the session is opened here and outlives
     * every frame taken from it. Returns false when the session could not be
     * created at all; a session that fails later reports through status.
     */
    bool icc_listen(ext_image_copy_capture_manager_v1 *manager, ext_output_image_capture_source_manager_v1 *source_manager, zwp_linux_dmabuf_v1 *dmabuf_interface, wl_output *output, bool blend_cursor = false);

    /// Request one frame from an established ICC session.
    void icc_capture(bool blend_cursor = false);

    /// Whether an ICC session is open and has finished negotiating.
    bool icc_ready() const {
      return icc_session.session && icc_session.done;
    }

    /// Whether this object is driving ICC rather than screencopy.
    bool icc_active() const {
      return icc_session.session != nullptr;
    }

    void icc_buffer_params_created(zwp_linux_buffer_params_v1 *params, struct wl_buffer *buffer);
    void icc_buffer_params_failed(zwp_linux_buffer_params_v1 *params);
    void icc_session_buffer_size(ext_image_copy_capture_session_v1 *session, std::uint32_t width, std::uint32_t height);
    void icc_session_shm_format(ext_image_copy_capture_session_v1 *session, std::uint32_t format);
    void icc_session_dmabuf_device(ext_image_copy_capture_session_v1 *session, struct wl_array *device);
    void icc_session_dmabuf_format(ext_image_copy_capture_session_v1 *session, std::uint32_t format, struct wl_array *modifiers);
    void icc_session_done(ext_image_copy_capture_session_v1 *session);
    void icc_session_stopped(ext_image_copy_capture_session_v1 *session);
    void icc_frame_transform(ext_image_copy_capture_frame_v1 *frame, std::uint32_t transform);
    void icc_frame_damage(ext_image_copy_capture_frame_v1 *frame, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
    void icc_frame_presentation_time(ext_image_copy_capture_frame_v1 *frame, std::uint32_t tv_sec_hi, std::uint32_t tv_sec_lo, std::uint32_t tv_nsec);
    void icc_frame_ready(ext_image_copy_capture_frame_v1 *frame);
    void icc_frame_failed(ext_image_copy_capture_frame_v1 *frame, std::uint32_t reason);
    void buffer(zwlr_screencopy_frame_v1 *frame, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t stride);
    void linux_dmabuf(zwlr_screencopy_frame_v1 *frame, std::uint32_t format, std::uint32_t width, std::uint32_t height);
    void buffer_done(zwlr_screencopy_frame_v1 *frame);
    void flags(zwlr_screencopy_frame_v1 *frame, std::uint32_t flags);
    void damage(zwlr_screencopy_frame_v1 *frame, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height);
    void ready(zwlr_screencopy_frame_v1 *frame, std::uint32_t tv_sec_hi, std::uint32_t tv_sec_lo, std::uint32_t tv_nsec);
    void failed(zwlr_screencopy_frame_v1 *frame);

    frame_t *get_next_frame() {
      return current_frame == &frames[0] ? &frames[1] : &frames[0];
    }

    status_e status;
    std::array<frame_t, 2> frames;
    frame_t *current_frame;
    zwlr_screencopy_frame_v1_listener listener;

  private:
    bool init_gbm();
    void cleanup_gbm();
    void create_and_copy_dmabuf(zwlr_screencopy_frame_v1 *frame);
    void icc_create_and_copy_dmabuf(ext_image_copy_capture_frame_v1 *frame);
    void icc_begin_constraints();
    void icc_destroy_session();

    zwp_linux_dmabuf_v1 *dmabuf_interface {nullptr};

    struct {
      bool supported {false};
      std::uint32_t format;
      std::uint32_t width;
      std::uint32_t height;
      std::uint32_t stride;
    } shm_info;

    struct {
      bool supported {false};
      std::uint32_t format;
      std::uint32_t width;
      std::uint32_t height;
    } dmabuf_info;

    struct {
      ext_image_copy_capture_session_v1 *session {nullptr};
      ext_image_copy_capture_frame_v1 *frame {nullptr};
      std::uint32_t width {0};
      std::uint32_t height {0};
      std::uint32_t format {0};
      // Keyed by format because the compositor sends one dmabuf_format event
      // per format it accepts, each with its own modifier list.
      std::map<std::uint32_t, std::vector<std::uint64_t>> modifiers;
      bool dmabuf_supported {false};
      bool cursor {false};
      bool done {false};
    } icc_session;

    struct gbm_device *gbm_device {nullptr};
    // gbm_create_device() borrows the fd, it does not adopt it, so the device
    // cannot be the only thing holding on to it.
    int drm_fd {-1};
    struct gbm_bo *current_bo {nullptr};
    struct wl_buffer *current_wl_buffer {nullptr};
    bool y_invert {false};
  };

  class monitor_t {
  public:
    explicit monitor_t(wl_output *output);

    monitor_t(monitor_t &&) = delete;
    monitor_t(const monitor_t &) = delete;
    monitor_t &operator=(const monitor_t &) = delete;
    monitor_t &operator=(monitor_t &&) = delete;

    void listen(zxdg_output_manager_v1 *output_manager);
    void xdg_name(zxdg_output_v1 *, const char *name);
    void xdg_description(zxdg_output_v1 *, const char *description);
    void xdg_position(zxdg_output_v1 *, std::int32_t x, std::int32_t y);
    void xdg_size(zxdg_output_v1 *, std::int32_t width, std::int32_t height);

    void xdg_done(zxdg_output_v1 *) {}

    void wl_geometry(wl_output *wl_output, std::int32_t x, std::int32_t y, std::int32_t physical_width, std::int32_t physical_height, std::int32_t subpixel, const char *make, const char *model, std::int32_t transform) {}

    void wl_mode(wl_output *wl_output, std::uint32_t flags, std::int32_t width, std::int32_t height, std::int32_t refresh);

    void wl_done(wl_output *wl_output) {}

    void wl_scale(wl_output *wl_output, std::int32_t factor) {}

    wl_output *output;
    std::string name;
    std::string description;
    platf::touch_port_t viewport;
    wl_output_listener wl_listener;
    zxdg_output_v1_listener xdg_listener;
  };

  class interface_t {
    struct bind_t {
      std::uint32_t id;
      std::uint32_t version;
    };

  public:
    enum interface_e {
      XDG_OUTPUT,  ///< xdg-output
      WLR_EXPORT_DMABUF,  ///< screencopy manager
      LINUX_DMABUF,  ///< linux-dmabuf protocol
      IMAGE_COPY_CAPTURE,  ///< ext-image-copy-capture manager
      IMAGE_CAPTURE_SOURCE,  ///< ext-image-capture-source manager, names an output as a source
      MAX_INTERFACES,  ///< Maximum number of interfaces
    };

    interface_t() noexcept;

    interface_t(interface_t &&) = delete;
    interface_t(const interface_t &) = delete;
    interface_t &operator=(const interface_t &) = delete;
    interface_t &operator=(interface_t &&) = delete;

    void listen(wl_registry *registry);

    bool operator[](interface_e bit) const {
      return interface[bit];
    }

    std::vector<std::unique_ptr<monitor_t>> monitors;
    zwlr_screencopy_manager_v1 *screencopy_manager {nullptr};
    zwp_linux_dmabuf_v1 *dmabuf_interface {nullptr};
    zxdg_output_manager_v1 *output_manager {nullptr};
    ext_image_copy_capture_manager_v1 *image_copy_capture_manager {nullptr};
    ext_output_image_capture_source_manager_v1 *image_capture_source_manager {nullptr};

  private:
    void add_interface(wl_registry *registry, std::uint32_t id, const char *interface, std::uint32_t version);
    void del_interface(wl_registry *registry, uint32_t id);

    std::bitset<MAX_INTERFACES> interface;
    wl_registry_listener listener;
  };

  class display_t {
  public:
    /**
     * @brief Initialize display.
     * If display_name == nullptr -> display_name = std::getenv("WAYLAND_DISPLAY")
     * @param display_name The name of the display.
     * @return 0 on success, -1 on failure.
     */
    int init(const char *display_name = nullptr);

    // Roundtrip with Wayland connection
    void roundtrip();

    // Wait up to the timeout to read and dispatch new events
    bool dispatch(std::chrono::milliseconds timeout);

    // Get the registry associated with the display
    // No need to manually free the registry
    wl_registry *registry();

    inline display_internal_t::pointer get() {
      return display_internal.get();
    }

  private:
    display_internal_t display_internal;
  };

  std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name = nullptr);
  /**
   * Configure a virtual output through wlroots' output-management protocol.
   * Returns false when the compositor does not expose that protocol or rejects
   * the requested layout.
   */
  bool configure_virtual_output(const std::string &output_name, int width, int height, int refresh_rate, bool exclusive);
  /** Restore the physical output layout saved by configure_virtual_output(). */
  bool restore_virtual_output_layout();
  /** Check whether the current compositor exposes wlroots output management. */
  bool output_management_supported();

  /**
   * @brief Whether GBM can actually allocate a buffer on this render node.
   *
   * Opening a render node proves nothing about whether buffers can come from
   * it. Mesa loads a driver for whatever the node turns out to be, and where
   * there is none to load - a proprietary-NVIDIA node has no nouveau behind it
   * - it falls back to kms_swrast without complaint, which allocates through
   * DRM_IOCTL_MODE_CREATE_DUMB and is refused by every render node. So the
   * device looks healthy right up to the first buffer. Asking for a throwaway
   * one is what separates the two cases.
   */
  bool render_node_can_allocate(const char *node);

  /**
   * @brief Which Wayland globals the running session advertises.
   *
   * Hermes' features are not "Wayland" features. Each one needs particular
   * protocols, and which of them a compositor implements is the whole
   * difference between a working stream and a black one - COSMIC has
   * wlr-output-management but no wlr-screencopy, Hyprland has both plus
   * ext-image-copy-capture. Asking the registry once is cheaper and far more
   * informative than attempting each feature and reading the failure, and it is
   * the only way to say *before* a stream starts what this session can do.
   */
  struct session_protocols_t {
    bool connected;  ///< A Wayland display was reachable at all.
    bool output_management;  ///< zwlr_output_manager_v1: enable/mode/position an output.
    bool screencopy;  ///< zwlr_screencopy_manager_v1: the wlgrab capture backend.
    bool image_copy_capture;  ///< ext_image_copy_capture_manager_v1: the successor to screencopy.
    bool output_capture_source;  ///< ext_output_image_capture_source_manager_v1: names an output as a source.
    bool linux_dmabuf;  ///< zwp_linux_dmabuf_v1: without it capture is a CPU copy.
    bool xdg_output;  ///< zxdg_output_manager_v1: logical geometry of each output.
  };

  /**
   * @brief Probe the session's globals in a single registry roundtrip.
   *
   * Binds nothing: it records which interfaces the compositor advertises, so it
   * is safe to call from diagnostics without disturbing a running stream.
   */
  session_protocols_t probe_protocols();

  /**
   * @brief Whether the dmabuf capture path has failed too often to be retried.
   *
   * A frame that cannot get a buffer ends the capture in capture_e::reinit, and
   * the reinit builds a fresh display_t around a fresh dmabuf_t - so a counter
   * kept on the object restarts at zero on every retry and never learns that
   * the same failure has been happening all session. That is how a capture path
   * that cannot work stays invisible: the same error forty times a second, each
   * one reported by a new object that believes it is the first, while the
   * encoders keep encoding the untouched black buffers. The streak therefore
   * lives beside the objects rather than inside them, and once it is long
   * enough the caller is expected to give up loudly instead of retrying.
   */
  bool dmabuf_capture_exhausted();

  int init();
}  // namespace wl
#else

struct wl_output;
struct zxdg_output_manager_v1;

namespace wl {
  class monitor_t {
  public:
    monitor_t(wl_output *output);

    monitor_t(monitor_t &&) = delete;
    monitor_t(const monitor_t &) = delete;
    monitor_t &operator=(const monitor_t &) = delete;
    monitor_t &operator=(monitor_t &&) = delete;

    void listen(zxdg_output_manager_v1 *output_manager);

    wl_output *output;
    std::string name;
    std::string description;
    platf::touch_port_t viewport;
  };

  inline std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name = nullptr) {
    return {};
  }

  inline int init() {
    return -1;
  }

  inline bool configure_virtual_output(const std::string &, int, int, int, bool) {
    return false;
  }

  inline bool restore_virtual_output_layout() {
    return false;
  }

  inline bool render_node_can_allocate(const char *) {
    // No GBM linked in without Wayland, so there is nothing to ask. Answering
    // yes leaves every caller on the behaviour it had before the probe existed.
    return true;
  }

  inline bool output_management_supported() {
    return false;
  }

  struct session_protocols_t {
    bool connected;
    bool output_management;
    bool screencopy;
    bool image_copy_capture;
    bool output_capture_source;
    bool linux_dmabuf;
    bool xdg_output;
  };

  inline session_protocols_t probe_protocols() {
    return {};
  }
}  // namespace wl
#endif
