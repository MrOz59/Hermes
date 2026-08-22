/**
 * @file src/platform/linux/virtual_display.h
 * @brief Virtual display declarations for Linux.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// local includes
#include "src/uuid.h"

namespace VDISPLAY {

  /**
   * @brief Status of the virtual display driver.
   */
  enum class DRIVER_STATUS {
    UNKNOWN = 1,  ///< Driver status unknown
    OK = 0,  ///< Driver is operational
    FAILED = -1,  ///< Driver failed to initialize
    VERSION_INCOMPATIBLE = -2,  ///< Driver version incompatible
    WATCHDOG_FAILED = -3,  ///< Driver watchdog failed
    NOT_SUPPORTED = -4  ///< Virtual display not supported on this system
  };

  /**
   * @brief Actionable EVDI state exposed to the Web UI.
   *
   * The generic driver status only tells callers that virtual displays are
   * unavailable. This enum identifies the host-side condition a user can fix.
   */
  enum class EVDI_DIAGNOSTIC {
    READY,
    INITIAL_DEVICE_CONFIGURATION_REQUIRED,
    LIBRARY_MISSING,
    MODULE_NOT_INSTALLED,
    MODULE_NOT_LOADED,
    DKMS_BUILD_FAILED,
  };

  struct EvdiVirtualDisplayStatus {
    std::string name;
    std::string client_name;
    int device_index;
    int output_index;  ///< 0-based Hermes-KMS output, or -1 for EVDI.
    int drm_card_index;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint64_t frame_updates;
  };

  /**
   * @brief Actionable Hermes-KMS state exposed to the Web UI.
   *
   * Mirrors EVDI_DIAGNOSTIC for the hermes_kms backend. `open_device` already
   * distinguishes these failure modes internally; this surfaces the reason so
   * the UI can offer the matching install/repair guidance instead of a generic
   * "unavailable".
   */
  enum class HERMES_KMS_DIAGNOSTIC {
    READY,
    MODULE_NOT_LOADED,
    MODULE_NOT_INSTALLED,
    DKMS_BUILD_FAILED,
    UAPI_TOO_OLD,
    MISSING_CAPABILITIES,
    DEVICE_NODE_MISSING,
  };

  struct HermesKmsStatus {
    HERMES_KMS_DIAGNOSTIC diagnostic;
    bool module_loaded;
    bool module_installed;
    bool device_present;  ///< A hermes-kms DRM card node was found and opened.
    int card_index;  ///< DRM card index of the Hermes-KMS device, or -1.
    uint32_t uapi_version;  ///< UAPI version reported by the device, or 0.
    uint32_t required_uapi_version;  ///< Minimum UAPI version Hermes needs.
    bool experimental_multi_output_enabled;
    bool multi_output_capable;
    bool experimental_isolated_sessions_enabled;
    bool multi_device_capable;
    uint32_t device_count;
    uint32_t output_count;
    uint32_t private_seat_broker_count;
    std::vector<uint32_t> missing_private_seat_brokers;
    std::string driver_version;  ///< "major.minor.patch" from the device, if present.
    std::string running_kernel;
    std::vector<std::string> dkms_kernels;
    std::vector<EvdiVirtualDisplayStatus> active_displays;
  };

  struct EvdiStatus {
    EVDI_DIAGNOSTIC diagnostic;
    bool library_installed;
    bool library_loaded;
    bool module_loaded;
    bool module_installed;
    int device_count;
    std::string session_type;
    bool exclusive_layout_supported;
    std::string output_layout_backend;
    bool capture_fallback_active;
    std::string library_version;
    std::string running_kernel;
    std::vector<std::string> dkms_kernels;
    std::vector<EvdiVirtualDisplayStatus> active_displays;
  };

  /**
   * @brief Device-lifetime counters read from the Hermes-KMS GET_METRICS ioctl.
   *
   * These are cumulative since the driver bound the device (not per-session) and
   * are exposed by the diagnostics endpoint when the hermes_kms backend is in
   * use. `available` is false when no Hermes-KMS device is present or the device
   * does not advertise the metrics capability.
   */
  struct HermesKmsMetrics {
    bool available = false;
    int output_index = -1;  ///< 0-based output selected for this snapshot.
    std::string output_name;
    uint64_t frame_sequence = 0;  ///< Latest frame sequence number.
    uint64_t frame_update_count = 0;  ///< Total framebuffer updates seen.
    uint64_t acquire_count = 0;  ///< ACQUIRE_FRAME ioctls served.
    uint64_t acquire_no_frame_count = 0;  ///< Acquires that found no new frame.
    uint64_t dmabuf_export_count = 0;  ///< DMA-BUFs exported to the consumer.
    uint64_t dmabuf_export_fail_count = 0;  ///< Failed DMA-BUF exports.
    uint64_t wait_count = 0;  ///< WAIT_FRAME ioctls served.
    uint64_t wait_ready_count = 0;  ///< Waits that returned a ready frame.
    uint64_t wait_timeout_count = 0;  ///< Waits that timed out.
    uint64_t output_enable_count = 0;  ///< Times the virtual output was enabled.
    uint64_t output_disable_count = 0;  ///< Times the virtual output was disabled.
    uint64_t hotplug_event_count = 0;  ///< Hotplug uevents emitted.
    uint64_t last_update_ns = 0;  ///< Timestamp of the last framebuffer update.
    uint64_t last_wait_duration_ns = 0;  ///< Duration of the last frame wait.
  };

  /**
   * @brief Read the Hermes-KMS device metrics, if a metrics-capable device is
   *        present. Opens and closes its own short-lived fd, so it is safe to
   *        call from the diagnostics endpoint regardless of session state.
   * @return Metrics with `available=true` on success; a default (`available=false`)
   *         value when no metrics-capable Hermes-KMS device is found.
   */
  HermesKmsMetrics getHermesKmsMetrics();

  /**
   * @brief Initialize the virtual display driver.
   * @return DRIVER_STATUS indicating the result of initialization.
   */
  DRIVER_STATUS openVDisplayDevice();

  /**
   * @brief Whether EVDI needs a device pre-created at module load time.
   *
   * This occurs when the loaded module has no devices and the current Apollo
   * process cannot write to EVDI's root-only sysfs add endpoint.
   */
  bool needsInitialDeviceConfiguration();

  /** Return the most specific available EVDI diagnostic for the current host. */
  EVDI_DIAGNOSTIC getEvdiDiagnostic();

  /** Return runtime, DKMS, and frame-update details for the Audio/Video UI. */
  EvdiStatus getEvdiStatus();

  /** Return the most specific available Hermes-KMS diagnostic for the current host. */
  HERMES_KMS_DIAGNOSTIC getHermesKmsDiagnostic();

  /** Return module, DKMS, and device details for the Audio/Video UI. */
  HermesKmsStatus getHermesKmsStatus();

  /**
   * @brief Close the virtual display driver.
   */
  void closeVDisplayDevice();

  /**
   * @brief Start a ping thread to keep the virtual display alive.
   * @param failCb Callback to invoke if the watchdog fails.
   * @return true if the ping thread was started successfully, false otherwise.
   */
  bool startPingThread(std::function<void()> failCb);

  /**
   * @brief Set the render adapter by name.
   * @param adapterName The name of the adapter to use for rendering.
   * @return true if the adapter was set successfully, false otherwise.
   */
  bool setRenderAdapterByName(const std::string &adapterName);

  /**
   * @brief Create a virtual display.
   * @param s_client_uid The unique identifier of the client.
   * @param s_client_name The name of the client.
   * @param width The width of the virtual display.
   * @param height The height of the virtual display.
   * @param fps The refresh rate of the virtual display (in mHz).
   * @param guid The GUID for the virtual display.
   * @return The name of the created virtual display, or empty string on failure.
   */
  std::string createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const uuid_util::uuid_t &guid
  );

  /**
   * @brief Remove a virtual display.
   * @param guid The GUID of the virtual display to remove.
   * @return true if the virtual display was removed successfully, false otherwise.
   */
  bool removeVirtualDisplay(const uuid_util::uuid_t &guid);

  /**
   * @brief Change the display settings of a virtual display.
   * @param deviceName The name of the virtual display.
   * @param width The new width.
   * @param height The new height.
   * @param refresh_rate The new refresh rate (in mHz).
   * @return 0 on success, non-zero on failure.
   */
  int changeDisplaySettings(const char *deviceName, int width, int height, int refresh_rate);

  /**
   * @brief Change the display settings with isolated display option.
   * @param deviceName The name of the virtual display.
   * @param width The new width.
   * @param height The new height.
   * @param refresh_rate The new refresh rate (in mHz).
   * @param bApplyIsolated Whether to apply isolated display settings.
   * @return 0 on success, non-zero on failure.
   */
  int changeDisplaySettings2(const char *deviceName, int width, int height, int refresh_rate, bool bApplyIsolated = false);

  /** Return the compositor-facing connector name for an EVDI display. */
  std::string getEvdiConnectorName(const std::string &displayName);

  /** Return the compositor-facing connector name for a Hermes-KMS display. */
  std::string getHermesKmsConnectorName(const std::string &displayName);

  /**
   * @brief The Wayland compositor driving this session.
   *
   * Hermes' virtual-display strategy differs per compositor in kind, not in
   * detail: KWin is configured through kscreen-doctor, Mutter only through
   * org.gnome.Mutter.DisplayConfig, and Hyprland accepts the output through
   * wlr-output-management and then cannot composite onto it. Treating
   * "wayland" as a single case hides those differences until someone reports a
   * black stream, so the session is classified once and dispatched on.
   */
  enum class compositor_e {
    unknown,   ///< X11, no session, or a compositor with no dedicated strategy.
    kwin,      ///< KDE Plasma.
    mutter,    ///< GNOME.
    hyprland,  ///< Hyprland.
  };

  /**
   * @brief Classify a compositor from an XDG_CURRENT_DESKTOP-style value.
   *
   * The variable holds a colon-separated list whose case is not guaranteed
   * ("KDE", "Hyprland", "ubuntu:GNOME"), so it is matched token by token: a
   * substring test would classify a desktop merely *mentioning* another one.
   * A token is accepted when it equals the compositor's desktop name or is a
   * variant of it ("GNOME-Classic").
   *
   * Exposed so the classification can be tested without a session.
   */
  compositor_e compositorFromDesktopNames(const std::string &desktop_names);

  /**
   * @brief Classify the compositor of the running session.
   *
   * Reads XDG_CURRENT_DESKTOP, falls back to XDG_SESSION_DESKTOP, and finally
   * to HYPRLAND_INSTANCE_SIGNATURE - which Hyprland always exports even when a
   * session file left the desktop name unset.
   */
  compositor_e sessionCompositor();

  /**
   * @brief Build the Mutter ApplyMonitorsConfig payload that drops a connector.
   *
   * Pure text transformation over an org.gnome.Mutter.DisplayConfig
   * GetCurrentState reply: it keeps every logical monitor except
   * @p virtual_connector, moving the primary flag to the first survivor when the
   * dropped monitor held it. Exposed so the payload - which is submitted to a
   * compositor as an all-or-nothing config - can be validated without GNOME.
   *
   * @param current_state The GetCurrentState reply, as printed by `gdbus call`.
   * @param virtual_connector The connector to remove, e.g. "Virtual-1".
   * @param serial Receives the config serial the payload must be applied with.
   * @param argument Receives the a(iiduba(ssa{sv})) argument.
   * @return true when a repair config was built; false when the connector is
   *         absent from the layout, the reply is unusable, or dropping it would
   *         leave Mutter with no monitor.
   */
  bool buildMutterLayoutWithoutConnector(
    const std::string &current_state,
    const std::string &virtual_connector,
    std::string &serial,
    std::string &argument
  );

  /**
   * @brief Build the Mutter ApplyMonitorsConfig payload that drives a connector
   *        at a requested geometry.
   *
   * Mutter adopts a hotplugged virtual connector on its own, but at whichever
   * mode it prefers. If that is not the mode the streaming client asked for,
   * the compositor scans out one resolution while Hermes captures another and
   * the client receives a black image. This selects the advertised mode that
   * matches @p width x @p height with the closest refresh rate (@p refresh_mhz
   * is in mHz, as Hermes carries it internally; Mutter reports Hz), keeping every
   * other logical monitor as Mutter has it, and appending a logical monitor at
   * the current right edge when Mutter adopted the connector without placing it
   * (Mutter rejects layouts with gaps between logical monitors).
   *
   * Exposed so the payload can be validated without a running GNOME session.
   *
   * @return true when a config was built; false when the reply is unusable, the
   *         connector is absent or does not advertise the geometry, or Mutter
   *         already drives it at that mode (nothing to apply).
   */
  bool buildMutterLayoutWithMode(
    const std::string &current_state,
    const std::string &connector,
    uint32_t width,
    uint32_t height,
    uint32_t refresh_mhz,
    std::string &serial,
    std::string &argument
  );

  /**
   * @brief Build the kscreen-doctor invocation that enables a virtual output,
   *        places it, and drives it at a requested mode.
   *
   * KWin adopts a hotplugged connector at whichever mode it prefers rather than
   * the one the streaming client negotiated: Hermes-KMS marks the client's
   * exact CVT mode preferred but still exposes the standard mode ladder, and
   * KWin has been seen taking 1920x1080 from that ladder for an 854x480
   * session. Because the capture path reports the real scanout, the client then
   * receives a full-resolution stream of a display it asked to be small, so the
   * mode has to be pushed rather than assumed.
   *
   * Every output in @p enabled_before is re-enabled at its own priority and the
   * virtual output takes the next one, keeping the local displays lit. Passing
   * a non-positive component in @p mode_width, @p mode_height or
   * @p mode_refresh_hz omits the mode, which is also how the caller retries a
   * layout that KWin rejected for the mode alone.
   *
   * Exposed so the invocation can be checked without a running KDE session.
   *
   * @return the complete command line.
   */
  std::string buildKScreenLayoutCommand(
    const std::string &virtual_output,
    const std::map<std::string, int> &enabled_before,
    int target_x,
    int target_y,
    int mode_width,
    int mode_height,
    int mode_refresh_hz
  );

  /** @brief What `kscreen-doctor -j` says about a mode on one output. */
  enum class kscreen_mode_state_e {
    unknown_output,  ///< KWin does not report that output at all.
    not_advertised,  ///< The output exists but offers no such mode.
    advertised,      ///< The mode exists and is not the one being driven.
    current,         ///< KWin already drives the output at that mode.
  };

  /**
   * @brief Find a mode on one output in a `kscreen-doctor -j` reply.
   *
   * Pushing a mode blind gives the same generic failure whether KWin refused a
   * valid request, the connector never advertised the geometry, or it was
   * already being driven at it. Reading the reply first separates those, and
   * the last case removes a needless kscreen-doctor call per session.
   *
   * Refresh rates are compared rounded to whole Hz, which is how kscreen-doctor
   * itself matches a `WxH@R` argument against the mode list: a connector
   * advertising 89.991 Hz is driven by asking for 90.
   *
   * Exposed so the reply can be interpreted without a running KDE session.
   */
  kscreen_mode_state_e kscreenModeState(
    const std::string &json_text,
    const std::string &output,
    int width,
    int height,
    int refresh_hz
  );

  /** Record whether capture was routed away from an uncomposited virtual output. */
  void setVirtualDisplayCaptureFallbackActive(bool active);

  /** Activate a virtual output using the current session's display protocol. */
  bool activateVirtualDisplayOutput(const std::string &displayName);
  /** Enable/restore exclusive layout for an EVDI virtual display. */
  bool enableExclusiveVirtualDisplay(const std::string &displayName);
  void restoreExclusiveVirtualDisplay();

  /**
   * @brief Get the primary display name.
   * @return The name of the primary display.
   */
  std::string getPrimaryDisplay();

  /**
   * @brief Set the primary display by name.
   * @param primaryDeviceName The name of the display to set as primary.
   * @return true if the primary display was set successfully, false otherwise.
   */
  bool setPrimaryDisplay(const char *primaryDeviceName);

  /**
   * @brief Get the HDR status of a display by name.
   * @param displayName The name of the display.
   * @return true if HDR is enabled, false otherwise.
   */
  bool getDisplayHDRByName(const char *displayName);

  /**
   * @brief Set the HDR status of a display by name.
   * @param displayName The name of the display.
   * @param enableAdvancedColor Whether to enable HDR.
   * @return true if the HDR status was set successfully, false otherwise.
   */
  bool setDisplayHDRByName(const char *displayName, bool enableAdvancedColor);

  /**
   * @brief Match displays by a given pattern.
   * @param sMatch The pattern to match.
   * @return A vector of matching display names.
   */
  std::vector<std::string> matchDisplay(const std::string &sMatch);

  /**
   * @brief Check if a display is an EVDI virtual display.
   * @param displayName The name of the display to check.
   * @return true if the display is an EVDI virtual display, false otherwise.
   */
  bool isEvdiDisplay(const std::string &displayName);

  /** Check if a display is a Hermes-KMS virtual display. */
  bool isHermesKmsDisplay(const std::string &displayName);

  /**
   * @brief Get the DRM card index for an EVDI display.
   * @param displayName The name of the EVDI display.
   * @return The card index, or -1 if not found or not an EVDI display.
   */
  int getEvdiCardIndex(const std::string &displayName);

  /** Get the DRM card index for a Hermes-KMS display. */
  int getHermesKmsCardIndex(const std::string &displayName);

  /** Check whether a usable Hermes-KMS DRM device exists on this host. */
  bool isHermesKmsDriverPresent();

  /** List the display names of all registered Hermes-KMS virtual displays. */
  std::vector<std::string> listHermesKmsDisplayNames();

  /** Get the primary DRM node path assigned to a Hermes-KMS display. */
  std::string getHermesKmsDevicePath(const std::string &displayName);

  /** Get the stable udev/libseat name assigned to an independent DRM card. */
  std::string getHermesKmsSeatName(const std::string &displayName);

  /**
   * CPU-side BGRA buffer filled directly by libevdi. EVDI is a virtual DRM
   * device rather than a render GPU, so this avoids sending its card through
   * GBM/EGL, which can crash in Mesa when no render node exists.
   */
  class EvdiBuffer {
  public:
    EvdiBuffer(uint32_t width, uint32_t height);
    EvdiBuffer(const EvdiBuffer &) = delete;
    EvdiBuffer &operator=(const EvdiBuffer &) = delete;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t stride() const { return width_ * 4; }
    uint64_t frame_number() const { return frame_number_.load(std::memory_order_acquire); }
    void *raw_buffer() { return data_.data(); }
    uint64_t copy_to(uint8_t *dst, uint32_t dst_stride) const;
    uint64_t wait_for_update(uint64_t last_frame, std::chrono::milliseconds timeout);
    void begin_write();
    void end_write();
    void mark_updated();

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<uint8_t> data_;
    uint32_t width_;
    uint32_t height_;
    std::atomic<uint64_t> frame_number_ {0};
  };

  /** Return the active CPU capture buffer for an EVDI virtual display. */
  std::shared_ptr<EvdiBuffer> getEvdiBuffer(const std::string &display_name);

  /**
   * Zero-copy capture of a Hermes-KMS virtual display.
   *
   * The compositor (KWin/GNOME) owns the primary card node and scans out the
   * desktop; this side opens the render node and pulls the current scanout
   * framebuffer as DMA-BUFs via DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME. No DRM
   * master and no KMS access are required, so it coexists with the compositor.
   */
  struct HermesKmsFrame {
    int width {0};
    int height {0};
    uint32_t fourcc {0};
    uint64_t modifier {0};
    uint32_t plane_count {0};
    int dma_buf_fd[4] {-1, -1, -1, -1};
    uint32_t pitch[4] {0, 0, 0, 0};
    uint32_t offset[4] {0, 0, 0, 0};
    int sync_file_fd {-1};
    uint64_t sequence {0};
    long long acquire_ns {0};  ///< Time spent in the ACQUIRE_FRAME ioctl only.

    void close();  ///< Close all owned fds (dma_buf_fd[] and sync_file_fd).
  };

  /**
   * Open the render node of the Hermes-KMS card behind @p display_name.
   * @return a render-node fd >= 0 on success, or -1 on failure.
   */
  int hermesKmsOpenCapture(const std::string &display_name);

  /**
   * Resolve the compositor position of a Hermes-KMS output for absolute input.
   * Returns false when the desktop integration cannot expose a geometry.
   */
  bool getHermesKmsDisplayGeometry(const std::string &display_name,
                                   int &offset_x, int &offset_y,
                                   int &environment_width, int &environment_height);

  /** Query the active scanout geometry. @return true on success. */
  bool hermesKmsCaptureSize(int render_fd, int &width, int &height);

  /**
   * Acquire the current scanout frame as DMA-BUFs. Blocks up to @p timeout_ms
   * for a frame newer than @p after_sequence (pass 0 to take whatever is
   * current). On success @p out owns the returned fds; the caller must call
   * out.close() when done. @return true on success.
   */
  bool hermesKmsAcquireFrame(int render_fd, uint64_t after_sequence,
                             uint32_t timeout_ms, HermesKmsFrame &out);

}  // namespace VDISPLAY
