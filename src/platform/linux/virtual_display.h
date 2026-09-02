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
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

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
   * @brief Output-lifetime counters read from the Hermes-KMS GET_METRICS ioctl.
   *
   * These are cumulative since the driver created the output (not per-session)
   * and are exposed by the diagnostics endpoint while Apollo owns an active
   * Hermes-KMS session. `available` is false when there is no active authorized
   * output or the device does not advertise the metrics capability.
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
    /// True when the driver reports the session-capability lifecycle counters
    /// below (uapi >= 13); they read as zero on older drivers, which is why
    /// they are reported only when this is set.
    bool session_lifecycle = false;
    /// Descriptors bound to the live session, including the short-lived one
    /// this snapshot binds to read the counters. Streaming with a single
    /// capture worker therefore reads 2; more than that is a leaked capability.
    uint64_t bound_fd_count = 0;
    uint64_t bind_count = 0;  ///< Bindings granted over the output's lifetime.
    uint64_t bind_reject_count = 0;  ///< Binds refused: bad token, wrong session, revoked.
    uint64_t unbind_count = 0;  ///< Bindings given up by their holder.
    uint64_t binding_revoke_count = 0;  ///< Bindings dropped by an owner's revocation.
    /// Frames exported while the same buffer was another session's scanout.
    /// Legitimate when a compositor mirrors one buffer onto two outputs, but
    /// those consumers are not isolated from each other.
    uint64_t cross_session_buffer_export_count = 0;
  };

  /**
   * @brief Read the Hermes-KMS device metrics, if a metrics-capable device is
   *        present. It opens a short-lived render fd and binds the active
   *        generic session capability before reading protected counters.
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
   * @brief Where the host compositor places a newly created virtual output.
   *
   * The KScreen path either appends the output beside the physical monitors
   * (extend) or overlaps the primary output (mirror). Mirror reproduces the
   * pre-0.5.0 behaviour, where KWin adopted the hotplugged connector at the
   * primary's position and cloned the desktop onto it - which is also what
   * makes windows the compositor places on the physical monitor (such as a
   * nested Gamescope) visible in the captured stream.
   */
  enum class virtual_display_layout_e {
    extend,  ///< Side-by-side with the physical outputs (the default).
    mirror,  ///< Overlapped with the primary output, so the desktop is cloned.
  };

  /**
   * @brief Create a virtual display.
   * @param s_client_uid The unique identifier of the client.
   * @param s_client_name The name of the client.
   * @param width The width of the virtual display.
   * @param height The height of the virtual display.
   * @param fps The refresh rate of the virtual display (in mHz).
   * @param guid The GUID for the virtual display.
   * @param session_owner_uid When set, a Hermes-KMS card the broker creates
   *        for this display belongs to this uid (an isolated session's
   *        account) rather than to Hermes itself.
   * @param layout Where the compositor should place the output. Honoured by
   *        the KScreen (KWin) and Mutter (GNOME) activation paths; other
   *        backends keep their own behaviour. The two compositors clone
   *        differently: KWin overlaps the virtual output with the primary,
   *        while Mutter refuses overlapping monitors and clones by putting
   *        both connectors in one logical monitor - which it only accepts when
   *        they advertise the same mode size, so mirroring there runs the
   *        stream at the physical monitor's resolution.
   * @return The name of the created virtual display, or empty string on failure.
   */
  std::string createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const uuid_util::uuid_t &guid,
    std::optional<uid_t> session_owner_uid = std::nullopt,
    virtual_display_layout_e layout = virtual_display_layout_e::extend
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
   * @param deadline_ms How long the compositor-side mode apply may poll before
   *        giving up. A hot path answering a waiting client - `/resume` - passes
   *        a shorter budget than a launch does.
   * @return 0 on success, -1 on failure, and 1 when the driver accepted the
   *         mode but the compositor kept the previously active one. That last
   *         case is not fatal: capture reads the real scanout geometry, so the
   *         stream follows the display rather than the request.
   */
  int changeDisplaySettings(const char *deviceName, int width, int height, int refresh_rate, int deadline_ms = 4000);

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
   *
   * The wlroots family is named rather than left unknown because "unknown" is
   * the answer for a session Hermes cannot advise at all, while sway, wayfire,
   * river and labwc are a case it *can* advise: they take the plain
   * wlr-output-management path with no per-compositor workaround. Reporting
   * them as unknown means the diagnostics cannot tell a user their session is
   * expected to work from one where nobody knows.
   */
  enum class compositor_e {
    unknown,   ///< X11, no session, or a compositor Hermes cannot name.
    kwin,      ///< KDE Plasma.
    mutter,    ///< GNOME.
    hyprland,  ///< Hyprland - aquamarine, not wlroots, and it does not behave like it.
    wlroots,   ///< sway, wayfire, river, labwc: the plain wlr-output-management path.
    cosmic,    ///< COSMIC: wlr-output-management, but no wlr-screencopy for capture.
  };

  /** Human-readable name for a compositor class, for logs and diagnostics. */
  std::string compositorName(compositor_e compositor);

  /**
   * @brief A thing Hermes does that a session either supports or does not.
   *
   * These are the promises a user reads on the box, and each one rests on a
   * different mechanism: driving a mode is wlr-output-management or
   * kscreen-doctor, blanking the physical monitor is the same protocols used
   * differently, and isolating a session does not involve the session
   * compositor at all. Reporting "virtual displays: unsupported" for a session
   * that can do four of the five is what makes the failures unactionable.
   */
  enum class feature_e {
    virtual_display,  ///< Create a virtual display and have it composited onto.
    client_requested_mode,  ///< Drive it at the geometry the client asked for.
    exclusive_mode,  ///< Blank the physical monitors for the duration of a session.
    multiple_displays,  ///< More than one virtual display in the same session.
    isolated_sessions,  ///< One seat and one compositor per client.
    zero_copy_capture,  ///< Capture over DMA-BUF rather than through a CPU copy.
  };

  /**
   * @brief How well a session supports one feature.
   *
   * `degraded` and `unknown` are deliberately distinct from `unavailable`:
   * "works, but not the way you asked" and "could not be determined" are
   * different messages to a user, and collapsing either into "unsupported" is
   * how a fixable configuration ends up read as a missing capability.
   */
  enum class readiness_e {
    ready,  ///< Verified available in this session.
    degraded,  ///< Usable, but not as asked for - the detail says how.
    unavailable,  ///< Cannot work here; remediation says what would change that.
    unknown,  ///< Could not be probed - never assume this means broken.
  };

  struct FeatureReport {
    feature_e feature;
    readiness_e readiness;
    std::string detail;  ///< Why this verdict, in terms of what was observed.
    std::string remediation;  ///< What the user can do; empty when nothing can be.
  };

  /**
   * @brief What was observed about the session, before any verdict is drawn.
   *
   * Separated from the assessment so the rules that turn observations into
   * advice are a pure function: every combination a user can present - COSMIC
   * without screencopy, Hyprland with the isolation rule missing, an
   * unrecognised compositor that nonetheless speaks wlr-output-management - is
   * then a table row in a test rather than a machine somebody has to own.
   */
  struct SessionFacts {
    compositor_e compositor {compositor_e::unknown};
    // Deliberately two flags rather than one: "not Wayland" and "X11" are
    // different claims, and a session that is neither - a headless service, a
    // daemon started before any window system - must not inherit X11's answers.
    // Reporting such a session as a working X11 one is the failure mode a
    // diagnostic can least afford.
    bool wayland {false};  ///< A Wayland session Hermes will capture from.
    bool x11 {false};  ///< An X11 session, which has its own xrandr layout path.
    bool output_management {false};  ///< zwlr_output_manager_v1 is advertised.
    bool screencopy {false};  ///< zwlr_screencopy_manager_v1 is advertised.
    bool image_copy_capture {false};  ///< ext_image_copy_capture_manager_v1 is advertised.
    bool linux_dmabuf {false};  ///< zwp_linux_dmabuf_v1 is advertised.
    bool kscreen {false};  ///< kscreen-doctor is usable.
    bool mutter {false};  ///< org.gnome.Mutter.DisplayConfig answers.
    bool hermes_kms_present {false};  ///< A Hermes-KMS device was opened.
    bool hermes_kms_multi_output {false};  ///< The device advertises more than one output.
    bool hermes_kms_multi_device {false};  ///< The driver exposes a session-device pool.
    bool drm_seat_isolation {false};  ///< Session DRM cards carry a private ID_SEAT.
    bool isolated_sessions_requested {false};  ///< The user turned isolation on.
    /// Hyprland's control socket is reachable, so Hermes can create a headless
    /// output. Recorded separately from the compositor because a service
    /// started outside the session sees Hyprland running and still cannot
    /// reach it - and that is a fixable configuration, not a missing feature.
    bool hyprland_control {false};
  };

  /** Human-readable names, for logs, diagnostics and the Web UI. */
  std::string featureName(feature_e feature);
  std::string readinessName(readiness_e readiness);

  /**
   * @brief Turn observations into a per-feature verdict with remediation.
   *
   * Pure: it reads @p facts and nothing else, so it is the whole of Hermes'
   * advice and can be tested for every session shape without one existing.
   */
  std::vector<FeatureReport> assessSession(const SessionFacts &facts);

  /** Observe the running session. Safe to call while a stream is running. */
  SessionFacts probeSessionFacts();

  /**
   * @brief Probe, assess and log what this session can and cannot do.
   *
   * Written for the user who only ever sees the log: a working feature is one
   * line, and one that is not carries both the reason and the fix.
   */
  void logSessionAssessment();

  /**
   * @brief Whether Hermes-KMS session cards carry a private DRM seat.
   *
   * Isolation has two halves and they are installed separately: Hermes' own
   * udev rules place a session's input devices on a private seat, while the
   * driver's `72-hermes-kms-session-seats.rules` does the same for its DRM
   * cards. With only the first installed a session still starts, still gets its
   * own input, and shares the host compositor's screen - and every check Hermes
   * performs today passes. This is the missing check: a card with no ID_SEAT is
   * on seat0, where the host compositor claims it.
   */
  bool hermesKmsSeatIsolationActive();

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
   * to the sockets a compositor exports whether or not a session file named it:
   * HYPRLAND_INSTANCE_SIGNATURE, SWAYSOCK and WAYFIRE_SOCKET. A session started
   * from a TTY or a bare `exec sway` frequently leaves both desktop variables
   * empty, which is the case that used to reach the generic path unnamed.
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
   * @brief Build the ApplyMonitorsConfig payload that hands the desktop to
   *        @p connector alone, which is how a monitor is turned off on GNOME:
   *        Mutter disables every connected monitor absent from the config.
   *
   * Exposed so an all-or-nothing payload can be checked without a GNOME
   * session.
   *
   * @return true when a config was built; false when the reply is unusable, the
   *         connector is not being driven, or it already has the desktop to
   *         itself (nothing to apply).
   */
  bool buildMutterExclusiveLayout(
    const std::string &current_state,
    const std::string &connector,
    std::string &serial,
    std::string &argument
  );

  /**
   * @brief Build the payload that clones the primary output onto @p connector.
   *
   * Mutter refuses overlapping logical monitors, so cloning means one logical
   * monitor holding both connectors - and it refuses that unless their modes
   * have identical dimensions. The payload therefore exists only at a
   * resolution the virtual output advertises.
   *
   * @return true when a config was built; false when the reply is unusable, no
   *         shared mode size exists, or the outputs are already mirrored.
   */
  bool buildMutterMirrorLayout(
    const std::string &current_state,
    const std::string &connector,
    std::string &serial,
    std::string &argument
  );

  /**
   * @brief Build the payload that reproduces the layout in @p current_state.
   *
   * This is what a session captures before changing the layout, and submits
   * again when it ends.
   */
  bool buildMutterRestoreLayout(
    const std::string &current_state,
    std::string &serial,
    std::string &argument
  );

  /**
   * @brief A counter bumped whenever the desktop layout changes underneath a
   *        running session.
   *
   * The offsets a capture resolves at start-up are a snapshot: the compositor
   * can move outputs, change their modes or replace a temporary configuration
   * at any time, and the geometry absolute input is measured against then goes
   * quietly wrong. A capture records this value when it starts and reinitialises
   * when it no longer matches, which re-resolves the geometry through the path
   * that already exists for a resolution change.
   *
   * Only the GNOME backend moves it today, and only when built with sd-bus;
   * everywhere else it is constant and the comparison costs nothing.
   */
  uint64_t displayLayoutGeneration();

  /**
   * @brief The ids GNOME's per-device settings path is built from.
   *
   * GNOME binds a touchscreen or tablet to a monitor through a key whose path
   * it derives from the device's vendor and product. These must be the ids
   * inputtino gives Hermes' virtual devices, or the binding addresses a device
   * that does not exist - which fails silently, since nothing errors and touch
   * input simply goes on being placed by guesswork.
   * `platf::VIRTUAL_INPUT_VENDOR_ID` is checked against these at compile time.
   */
  constexpr uint16_t VIRTUAL_INPUT_SETTINGS_VENDOR_ID = 0xBEEF;
  constexpr uint16_t VIRTUAL_INPUT_SETTINGS_PRODUCT_ID = 0xDEAD;

  /**
   * @brief The GNOME `output` value that binds an input device to @p connector.
   *
   * GNOME binds a touchscreen or tablet to one monitor, matching a list of at
   * least three strings against the monitor's EDID vendor, product and serial.
   * The connector is appended as a fourth element, which Mutter uses to
   * disambiguate two monitors sharing an EDID.
   *
   * @return the value to write, or an empty string when the monitor is unknown,
   *         carries no identity at all, or spells one that cannot be put on a
   *         command line safely.
   */
  std::string mutterInputDeviceOutputValue(
    const std::string &current_state,
    const std::string &connector
  );

  /**
   * @brief The relocatable-schema targets whose `output` key binds Hermes' own
   *        touch and pen devices, as `<schema>:<path>`.
   *
   * Mutter derives the path from the device's vendor and product ids, so these
   * only reach Hermes' devices while they match the ids inputtino is given.
   */
  void mutterInputDeviceSettingsTargets(std::string &touch, std::string &pen);

  /**
   * @brief Where a connector sits on a GNOME desktop, for absolute input.
   *
   * Mutter feeds an absolute pointing device the extents of the whole stage, so
   * a client's coordinates only land on the streamed output once they carry
   * that output's offset within the desktop envelope. Both are read from the
   * logical monitor layout - the space the stage is measured in - which means
   * scale and rotation, not the mode size.
   *
   * The values come back in the connector's own pixel space rather than in
   * logical pixels, so a consumer holding coordinates in the captured mode's
   * pixels can use them unchanged; at scale 1 the two are the same thing.
   *
   * Exposed so the arithmetic can be checked against a captured
   * GetCurrentState reply, without a running GNOME session.
   *
   * @param current_state The GetCurrentState reply, as printed by `gdbus call`.
   * @param connector The connector to locate, e.g. "Virtual-1".
   * @return true when the connector is in the layout and the envelope could be
   *         measured; false leaves the outputs untouched.
   */
  bool mutterDisplayGeometry(
    const std::string &current_state,
    const std::string &connector,
    int &offset_x,
    int &offset_y,
    int &environment_width,
    int &environment_height
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
   * @brief The Hyprland output name backing a virtual display, or empty.
   *
   * Non-empty only for a display Hyprland created as a headless output, which
   * is also the test for "this display works on this session": a Hermes-KMS or
   * EVDI display in a Hyprland session cannot be composited onto.
   */
  std::string getHyprlandOutputName(const std::string &displayName);

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

  /** Result of waiting for either independently captured plane to advance. */
  struct HermesKmsUpdate {
    bool frame_ready {false};
    bool cursor_ready {false};
    uint64_t frame_sequence {0};
    uint64_t cursor_sequence {0};
  };

  /** Latest cursor-plane state exported by the generic Hermes-KMS UAPI. */
  struct HermesKmsCursor {
    bool visible {false};
    bool position_valid {false};
    bool geometry_valid {false};
    bool buffer_valid {false};
    int32_t position_x {0};
    int32_t position_y {0};
    int32_t crtc_x {0};
    int32_t crtc_y {0};
    uint32_t crtc_w {0};
    uint32_t crtc_h {0};
    uint32_t src_x {0};  ///< DRM 16.16 fixed point.
    uint32_t src_y {0};  ///< DRM 16.16 fixed point.
    uint32_t src_w {0};  ///< DRM 16.16 fixed point.
    uint32_t src_h {0};  ///< DRM 16.16 fixed point.
    int32_t hotspot_x {0};
    int32_t hotspot_y {0};
    uint32_t width {0};
    uint32_t height {0};
    uint32_t fourcc {0};
    uint64_t modifier {0};
    uint32_t plane_count {0};
    int dma_buf_fd[4] {-1, -1, -1, -1};
    uint32_t pitch[4] {0, 0, 0, 0};
    uint32_t offset[4] {0, 0, 0, 0};
    int sync_file_fd {-1};
    uint64_t sequence {0};
    uint64_t image_sequence {0};

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

  /** Wait until the primary frame or cursor stream advances. */
  bool hermesKmsWaitUpdate(int render_fd, uint64_t after_frame_sequence,
                           uint64_t after_cursor_sequence, uint32_t timeout_ms,
                           HermesKmsUpdate &out);

  /** Acquire current cursor metadata and, when requested, its DMA-BUF. */
  bool hermesKmsAcquireCursor(int render_fd, bool request_buffer,
                              HermesKmsCursor &out);

}  // namespace VDISPLAY
