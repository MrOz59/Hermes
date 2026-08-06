/**
 * @file src/process.cpp
 * @brief Definitions for the startup and shutdown of the apps started by a streaming Session.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#ifndef BOOST_PROCESS_VERSION
 #define BOOST_PROCESS_VERSION 1
#endif
// standard includes
#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/process/v1/search_path.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

// local includes
#include "config.h"
#include "crypto.h"
#include "display_device.h"
#include "file_handler.h"
#include "logging.h"
#include "platform/common.h"
#include "process.h"
#include "httpcommon.h"
#include "system_tray.h"
#include "utility.h"
#include "video.h"
#include "uuid.h"

#ifdef _WIN32
  // from_utf8() string conversion function
  #include "platform/windows/misc.h"
  #include "platform/windows/utils.h"
  #include "platform/windows/virtual_display.h"

  // _SH constants for _wfsopen()
  #include <share.h>
#else
  #include "platform/linux/virtual_display.h"
  #include <sys/stat.h>
  #include <unistd.h>
#endif

#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png"

namespace proc {
  using namespace std::literals;
  namespace pt = boost::property_tree;

  proc_t proc;

  int input_only_app_id = -1;
  std::string input_only_app_id_str;
  int terminate_app_id = -1;
  std::string terminate_app_id_str;

  VDISPLAY::DRIVER_STATUS vDisplayDriverStatus = VDISPLAY::DRIVER_STATUS::UNKNOWN;

#ifndef _WIN32
  namespace {
    struct isolated_runtime_t {
      uint32_t launch_session_id {};
      std::string client_uuid;
      std::string client_name;
      ctx_t app;
      std::string profile;
      std::string runtime_id;
      std::string seat_id;
      std::string seatd_socket;
      std::string runtime_dir;
      std::string wayland_display;
      std::string drm_device_path;
      std::string display_name;
      uuid_util::uuid_t display_guid {};
      boost::process::v1::environment env;
      boost::process::v1::child process;
      boost::process::v1::group process_group;
      file_t pipe;
      size_t prep_completed {};
      std::atomic_bool stopping {false};
    };

    std::mutex isolated_runtimes_mutex;
    std::unordered_map<uint32_t, std::shared_ptr<isolated_runtime_t>> isolated_runtimes;
    std::unordered_map<std::string, std::shared_ptr<std::atomic_bool>>
      isolated_launching_clients;

    std::string shell_quote(std::string_view value) {
      std::string quoted {"'"};
      for (const char ch : value) {
        if (ch == '\'') {
          quoted += "'\\''";
        } else {
          quoted += ch;
        }
      }
      quoted += '\'';
      return quoted;
    }

    std::string isolated_runtime_root() {
      if (const char *xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR");
          xdg_runtime_dir && *xdg_runtime_dir) {
        return (std::filesystem::path {xdg_runtime_dir} / "hermes" / "sessions").string();
      }
      return (std::filesystem::path {"/tmp"} /
              ("hermes-" + std::to_string(static_cast<unsigned long>(::getuid()))) /
              "sessions").string();
    }

    std::string card_basename(const std::string &device_path) {
      return std::filesystem::path {device_path}.filename().string();
    }

    std::string discover_wayland_socket(const std::string &runtime_dir) {
      std::error_code ec;
      std::vector<std::string> candidates;
      for (std::filesystem::directory_iterator it {runtime_dir, ec}, end;
           !ec && it != end;
           it.increment(ec)) {
        const auto name = it->path().filename().string();
        if ((name.starts_with("gamescope-") || name.starts_with("wayland-")) &&
            std::filesystem::is_socket(it->symlink_status(ec))) {
          candidates.emplace_back(name);
        }
      }
      std::sort(candidates.begin(), candidates.end());
      return candidates.empty() ? std::string {} : candidates.front();
    }

    std::string choose_isolated_profile(const ctx_t &app) {
      if (app.session_type == "application" || app.session_type == "desktop") {
        return app.session_type;
      }
      return app.cmd.empty() ? "desktop" : "application";
    }

    void populate_session_environment(
      boost::process::v1::environment &env,
      const ctx_t &app,
      const rtsp_stream::launch_session_t &session,
      const isolated_runtime_t &runtime
    ) {
      const auto render_width = session.width ? session.width : 1920;
      const auto render_height = session.height ? session.height : 1080;
      const auto fps_millihz = session.fps ? session.fps : 60000;
      char fps_buffer[16] {};
      std::snprintf(fps_buffer, sizeof(fps_buffer), "%.3f", static_cast<double>(fps_millihz) / 1000.0);

      env["XDG_RUNTIME_DIR"] = runtime.runtime_dir;
      env["XDG_SESSION_TYPE"] = "wayland";
      env["XDG_SESSION_CLASS"] = "user";
      env["XDG_SEAT"] = runtime.seat_id;
      env["WAYLAND_DISPLAY"] = runtime.wayland_display;
      env["DISPLAY"] = "";
      env["LIBSEAT_BACKEND"] = "seatd";
      env["SEATD_SOCK"] = runtime.seatd_socket;
      env["WLR_DRM_DEVICES"] = runtime.drm_device_path;

      env["SUNSHINE_APP_ID"] = app.id;
      env["SUNSHINE_APP_NAME"] = app.name;
      env["SUNSHINE_CLIENT_WIDTH"] = std::to_string(render_width);
      env["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(render_height);
      env["SUNSHINE_CLIENT_FPS"] = config::sunshine.envvar_compatibility_mode ?
                                     std::to_string(std::lround(static_cast<double>(fps_millihz) / 1000.0)) :
                                     fps_buffer;
      env["SUNSHINE_CLIENT_HDR"] = session.enable_hdr ? "true" : "false";

      env["APOLLO_APP_ID"] = app.id;
      env["APOLLO_APP_NAME"] = app.name;
      env["APOLLO_APP_UUID"] = app.uuid;
      env["APOLLO_APP_STATUS"] = "STARTING";
      env["APOLLO_CLIENT_UUID"] = session.unique_id;
      env["APOLLO_CLIENT_NAME"] = session.device_name;
      env["APOLLO_CLIENT_WIDTH"] = std::to_string(render_width);
      env["APOLLO_CLIENT_HEIGHT"] = std::to_string(render_height);
      env["APOLLO_CLIENT_RENDER_WIDTH"] = std::to_string(render_width);
      env["APOLLO_CLIENT_RENDER_HEIGHT"] = std::to_string(render_height);
      env["APOLLO_CLIENT_FPS"] = fps_buffer;

      env["HERMES_APP_ID"] = app.id;
      env["HERMES_APP_NAME"] = app.name;
      env["HERMES_APP_UUID"] = app.uuid;
      env["HERMES_APP_STATUS"] = "STARTING";
      env["HERMES_CLIENT_UUID"] = session.unique_id;
      env["HERMES_CLIENT_NAME"] = session.device_name;
      env["HERMES_CLIENT_WIDTH"] = std::to_string(render_width);
      env["HERMES_CLIENT_HEIGHT"] = std::to_string(render_height);
      env["HERMES_CLIENT_RENDER_WIDTH"] = std::to_string(render_width);
      env["HERMES_CLIENT_RENDER_HEIGHT"] = std::to_string(render_height);
      env["HERMES_CLIENT_FPS"] = fps_buffer;
      env["HERMES_SESSION_ID"] = runtime.runtime_id;
      env["HERMES_SESSION_SEAT"] = runtime.seat_id;
      env["HERMES_SEATD_SOCKET"] = runtime.seatd_socket;
      env["HERMES_SESSION_PROFILE"] = runtime.profile;
      env["HERMES_DRM_DEVICE"] = runtime.drm_device_path;
      env["HERMES_WAYLAND_DISPLAY"] = runtime.wayland_display;

      if (!session.enable_hdr) {
        env["DXVK_HDR"] = "0";
        env["PROTON_ENABLE_HDR"] = "0";
      }
    }

    bool isolated_runtime_running(const std::shared_ptr<isolated_runtime_t> &runtime) {
      return runtime && runtime->process.valid() && runtime->process.running();
    }

    void stop_isolated_runtime(const std::shared_ptr<isolated_runtime_t> &runtime) {
      if (!runtime || runtime->stopping.exchange(true)) {
        return;
      }

      BOOST_LOG(info) << "[IsolatedSession] Stopping " << runtime->runtime_id
                      << " for client " << runtime->client_name;
      terminate_process_group(
        runtime->process,
        runtime->process_group,
        runtime->app.exit_timeout
      );
      runtime->process = boost::process::v1::child {};
      runtime->process_group = boost::process::v1::group {};
      runtime->env["APOLLO_APP_STATUS"] = "TERMINATING";
      runtime->env["HERMES_APP_STATUS"] = "TERMINATING";

      for (size_t index = runtime->prep_completed; index > 0; --index) {
        const auto &cmd = runtime->app.prep_cmds[index - 1];
        if (cmd.undo_cmd.empty()) {
          continue;
        }
        std::error_code ec;
        auto working_dir = runtime->app.working_dir.empty() ?
                             find_working_directory(cmd.undo_cmd, runtime->env) :
                             boost::filesystem::path {runtime->app.working_dir};
        BOOST_LOG(info) << "[IsolatedSession] Executing Undo Cmd: [" << cmd.undo_cmd << ']';
        auto child = platf::run_command(
          cmd.elevated,
          true,
          cmd.undo_cmd,
          working_dir,
          runtime->env,
          runtime->pipe.get(),
          ec,
          nullptr
        );
        if (!ec) {
          child.wait();
        } else {
          BOOST_LOG(warning) << "[IsolatedSession] Undo command failed to start: " << ec.message();
        }
      }

      runtime->pipe.reset();
      if (!runtime->display_name.empty()) {
        VDISPLAY::removeVirtualDisplay(runtime->display_guid);
      }
      std::error_code remove_ec;
      std::filesystem::remove_all(runtime->runtime_dir, remove_ec);
      if (remove_ec) {
        BOOST_LOG(debug) << "[IsolatedSession] Could not remove runtime directory "
                         << runtime->runtime_dir << ": " << remove_ec.message();
      }
    }

    std::shared_ptr<isolated_runtime_t> find_isolated_client_locked(const std::string &client_uuid) {
      for (const auto &[id, runtime] : isolated_runtimes) {
        if (runtime && runtime->client_uuid == client_uuid) {
          return runtime;
        }
      }
      return {};
    }
  }  // namespace
#endif

  void onVDisplayWatchdogFailed() {
    vDisplayDriverStatus = VDISPLAY::DRIVER_STATUS::WATCHDOG_FAILED;
    VDISPLAY::closeVDisplayDevice();
  }

  void initVDisplayDriver() {
    vDisplayDriverStatus = VDISPLAY::openVDisplayDevice();
    if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      if (!VDISPLAY::startPingThread(onVDisplayWatchdogFailed)) {
        onVDisplayWatchdogFailed();
        return;
      }
    }
  }

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() {
      proc.terminate_all_isolated();
      proc.terminate();
      // Join the watchdog thread before global destructors run, otherwise a
      // still-joinable std::thread aborts the process on exit.
      VDISPLAY::closeVDisplayDevice();
    }
  };

  std::unique_ptr<platf::deinit_t> init() {
    return std::make_unique<deinit_t>();
  }

  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout) {
    if (group.valid() && platf::process_group_running((std::uintptr_t) group.native_handle())) {
      if (exit_timeout.count() > 0) {
        // Request processes in the group to exit gracefully
        if (platf::request_process_group_exit((std::uintptr_t) group.native_handle())) {
          // If the request was successful, wait for a little while for them to exit.
          BOOST_LOG(info) << "Successfully requested the app to exit. Waiting up to "sv << exit_timeout.count() << " seconds for it to close."sv;

          // group::wait_for() and similar functions are broken and deprecated, so we use a simple polling loop
          while (platf::process_group_running((std::uintptr_t) group.native_handle()) && (--exit_timeout).count() >= 0) {
            std::this_thread::sleep_for(1s);
          }

          if (exit_timeout.count() < 0) {
            BOOST_LOG(warning) << "App did not fully exit within the timeout. Terminating the app's remaining processes."sv;
          } else {
            BOOST_LOG(info) << "All app processes have successfully exited."sv;
          }
        } else {
          BOOST_LOG(info) << "App did not respond to a graceful termination request. Forcefully terminating the app's processes."sv;
        }
      } else {
        BOOST_LOG(info) << "No graceful exit timeout was specified for this app. Forcefully terminating the app's processes."sv;
      }

      // We always call terminate() even if we waited successfully for all processes above.
      // This ensures the process group state is consistent with the OS in boost.
      std::error_code ec;
      group.terminate(ec);
      group.detach();
    }

    if (proc.valid()) {
      // avoid zombie process
      proc.detach();
    }
  }

  boost::filesystem::path find_working_directory(const std::string &cmd, const boost::process::v1::environment &env) {
    // Parse the raw command string into parts to get the actual command portion
#ifdef _WIN32
    auto parts = boost::program_options::split_winmain(cmd);
#else
    auto parts = boost::program_options::split_unix(cmd);
#endif
    if (parts.empty()) {
      BOOST_LOG(error) << "Unable to parse command: "sv << cmd;
      return boost::filesystem::path();
    }

    BOOST_LOG(debug) << "Parsed target ["sv << parts.at(0) << "] from command ["sv << cmd << ']';

    // If the target is a URL, don't parse any further here
    if (parts.at(0).find("://") != std::string::npos) {
      return boost::filesystem::path();
    }

    // If the cmd path is not an absolute path, resolve it using our PATH variable
    boost::filesystem::path cmd_path(parts.at(0));
    if (!cmd_path.is_absolute()) {
      cmd_path = boost::process::v1::search_path(parts.at(0));
      if (cmd_path.empty()) {
        BOOST_LOG(error) << "Unable to find executable ["sv << parts.at(0) << "]. Is it in your PATH?"sv;
        return boost::filesystem::path();
      }
    }

    BOOST_LOG(debug) << "Resolved target ["sv << parts.at(0) << "] to path ["sv << cmd_path << ']';

    // Now that we have a complete path, we can just use parent_path()
    return cmd_path.parent_path();
  }

  void proc_t::launch_input_only() {
    _app_id = input_only_app_id;
    _app_name = "Remote Input";
    _app.uuid = REMOTE_INPUT_UUID;
    _app.terminate_on_pause = true;
    allow_client_commands = false;
    placebo = true;

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::update_tray_playing(_app_name);
#endif
  }

  int proc_t::prepare_session_virtual_display(
    std::shared_ptr<rtsp_stream::launch_session_t> launch_session,
    bool apply_scale,
    const ctx_t *session_app
  ) {
#ifdef _WIN32
    (void) launch_session;
    (void) apply_scale;
    (void) session_app;
    return 0;
#else
    if (config::video.virtual_display_backend != "hermes_kms" ||
        (!config::video.hermes_kms_multi_output &&
         !config::video.hermes_kms_isolated_sessions)) {
      return 0;
    }

    const auto &app = session_app ? *session_app : _app;
    if (apply_scale) {
      uint32_t render_width = launch_session->width ? launch_session->width : 1920;
      uint32_t render_height = launch_session->height ? launch_session->height : 1080;
      int scale_factor = launch_session->scale_factor;
      if (app.scale_factor != 100) {
        scale_factor = app.scale_factor;
      } else if (scale_factor == 100 && config::video.default_scale_factor != 100) {
        scale_factor = config::video.default_scale_factor;
      }
      if (scale_factor != 100) {
        render_width = static_cast<uint32_t>(render_width * (static_cast<float>(scale_factor) / 100.0f)) & ~1U;
        render_height = static_cast<uint32_t>(render_height * (static_cast<float>(scale_factor) / 100.0f)) & ~1U;
      }
      launch_session->width = render_width;
      launch_session->height = render_height;
    }

    if (vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
      initVDisplayDriver();
    }
    if (vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
      BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Cannot allocate a session output because the driver is unavailable.";
      return 503;
    }

    std::string device_name;
    std::string device_uuid_str;
    uuid_util::uuid_t device_uuid;
    if (app.use_app_identity) {
      device_name = app.name;
      if (app.per_client_app_identity) {
        device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);
        auto app_uuid_string = app.uuid;
        const auto app_uuid = uuid_util::uuid_t::parse(app_uuid_string);
        device_uuid.b64[0] ^= app_uuid.b64[0];
        device_uuid.b64[1] ^= app_uuid.b64[1];
        device_uuid_str = device_uuid.string();
      } else {
        // Session-scoped outputs must remain distinct for simultaneous
        // clients, even when the app normally requests one shared identity.
        device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);
        device_uuid_str = device_uuid.string();
      }
    } else {
      device_name = launch_session->device_name;
      device_uuid_str = launch_session->unique_id;
      device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);
    }

    if (config::video.hermes_kms_isolated_sessions) {
      // The display is a session resource. Mix in the launch ID so a stale
      // connection from the same paired client cannot collide with a new one.
      device_uuid.b32[2] ^= launch_session->id;
      device_uuid_str = device_uuid.string();
    }

    launch_session->display_guid = device_uuid;
    int target_fps = launch_session->fps ? launch_session->fps : 60000;
    if (target_fps < 1000) {
      target_fps *= 1000;
    }
    if (config::video.double_refreshrate) {
      target_fps *= 2;
    }

    const auto display_name = VDISPLAY::createVirtualDisplay(
      device_uuid_str.c_str(),
      device_name.c_str(),
      launch_session->width,
      launch_session->height,
      target_fps,
      launch_session->display_guid
    );
    if (display_name.empty()) {
      return 503;
    }

    VDISPLAY::changeDisplaySettings(
      display_name.c_str(),
      launch_session->width,
      launch_session->height,
      target_fps
    );
    if (!config::video.hermes_kms_isolated_sessions &&
        !VDISPLAY::activateVirtualDisplayOutput(display_name)) {
      BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] The compositor did not activate session output "
                       << display_name << "; removing it instead of capturing another monitor.";
      VDISPLAY::removeVirtualDisplay(launch_session->display_guid);
      return 503;
    }

    launch_session->virtual_display = true;
    launch_session->session_scoped_virtual_display = true;
    launch_session->session_virtual_display_cleanup_pending = true;
    launch_session->display_name = display_name;
    launch_session->drm_device_path = VDISPLAY::getHermesKmsDevicePath(display_name);
    if (config::video.hermes_kms_isolated_sessions &&
        launch_session->drm_device_path.empty()) {
      BOOST_LOG(error) << "[VDISPLAY/Hermes-KMS] Could not resolve the independent DRM card for "
                       << display_name;
      VDISPLAY::removeVirtualDisplay(launch_session->display_guid);
      launch_session->session_virtual_display_cleanup_pending = false;
      launch_session->session_scoped_virtual_display = false;
      launch_session->display_name.clear();
      return 503;
    }
    BOOST_LOG(info) << "[VDISPLAY/Hermes-KMS] Experimental session-scoped output "
                    << display_name << " assigned to client " << launch_session->device_name;
    if (config::video.isolated_virtual_display_option) {
      BOOST_LOG(warning) << "[VDISPLAY/Hermes-KMS] Host exclusive-display mode does not apply to "
                            "experimental session-scoped outputs; physical and other client "
                            "sessions remain active.";
    }
    return 0;
#endif
  }

  int proc_t::execute_isolated(
    const ctx_t &app,
    std::shared_ptr<rtsp_stream::launch_session_t> launch_session
  ) {
#ifdef _WIN32
    (void) app;
    (void) launch_session;
    return -1;
#else
    if (!config::video.hermes_kms_isolated_sessions ||
        config::video.virtual_display_backend != "hermes_kms") {
      return execute(app, std::move(launch_session));
    }
    if (app.session_type == "shared") {
      BOOST_LOG(error) << "[IsolatedSession] The legacy shared-host profile cannot run "
                          "concurrently with independent sessions. Disable "
                          "hermes_kms_isolated_sessions to use the host desktop.";
      return 400;
    }

    std::shared_ptr<isolated_runtime_t> stale_runtime;
    auto launch_cancelled = std::make_shared<std::atomic_bool>(false);
    {
      std::lock_guard<std::mutex> lock(isolated_runtimes_mutex);
      if (isolated_launching_clients.contains(launch_session->unique_id)) {
        BOOST_LOG(warning) << "[IsolatedSession] Client " << launch_session->device_name
                           << " already has a launch in progress.";
        return 409;
      }
      if (const auto existing = find_isolated_client_locked(launch_session->unique_id);
          isolated_runtime_running(existing)) {
        BOOST_LOG(warning) << "[IsolatedSession] Client " << launch_session->device_name
                           << " already owns runtime " << existing->runtime_id;
        return 409;
      } else if (existing) {
        stale_runtime = existing;
        isolated_runtimes.erase(existing->launch_session_id);
      }
      isolated_launching_clients.emplace(
        launch_session->unique_id,
        launch_cancelled
      );
    }
    stop_isolated_runtime(stale_runtime);
    auto release_launch_reservation = util::fail_guard([
      client_uuid = launch_session->unique_id,
      launch_cancelled
    ]() {
      std::lock_guard<std::mutex> lock(isolated_runtimes_mutex);
      const auto it = isolated_launching_clients.find(client_uuid);
      if (it != isolated_launching_clients.end() &&
          it->second == launch_cancelled) {
        isolated_launching_clients.erase(it);
      }
    });
    auto launch_was_cancelled = [&]() {
      if (!launch_cancelled->load(std::memory_order_relaxed)) {
        return false;
      }
      BOOST_LOG(info) << "[IsolatedSession] Launch cancelled for client "
                      << launch_session->device_name;
      return true;
    };

    if (launch_was_cancelled()) {
      return 503;
    }

    auto runtime = std::make_shared<isolated_runtime_t>();
    runtime->launch_session_id = launch_session->id;
    runtime->client_uuid = launch_session->unique_id;
    runtime->client_name = launch_session->device_name;
    runtime->app = app;
    runtime->profile = choose_isolated_profile(app);
    runtime->runtime_id = "hermes-s" + std::to_string(launch_session->id);
    runtime->wayland_display = "wayland-" + runtime->runtime_id;
    runtime->runtime_dir =
      (std::filesystem::path {isolated_runtime_root()} / runtime->runtime_id).string();

    std::error_code dir_ec;
    std::filesystem::create_directories(runtime->runtime_dir, dir_ec);
    if (dir_ec || ::chmod(runtime->runtime_dir.c_str(), S_IRWXU) != 0) {
      BOOST_LOG(error) << "[IsolatedSession] Could not create private runtime directory "
                       << runtime->runtime_dir << ": "
                       << (dir_ec ? dir_ec.message() : std::strerror(errno));
      return -1;
    }

    auto cleanup = util::fail_guard([&]() {
      stop_isolated_runtime(runtime);
      if (launch_session->session_virtual_display_cleanup_pending) {
        // stop_isolated_runtime() owns the actual display removal once the
        // runtime has copied display_name/display_guid.
        if (runtime->display_name.empty()) {
          VDISPLAY::removeVirtualDisplay(launch_session->display_guid);
        }
        launch_session->session_virtual_display_cleanup_pending = false;
        launch_session->session_scoped_virtual_display = false;
        launch_session->display_name.clear();
      }
    });

    if (const int result = prepare_session_virtual_display(
          launch_session,
          true,
          &app
        );
        result != 0) {
      return result;
    }
    if (launch_was_cancelled()) {
      return 503;
    }

    runtime->display_name = launch_session->display_name;
    runtime->display_guid = launch_session->display_guid;
    runtime->drm_device_path = launch_session->drm_device_path;
    runtime->seat_id = VDISPLAY::getHermesKmsSeatName(runtime->display_name);
    if (runtime->seat_id.empty()) {
      BOOST_LOG(error) << "[IsolatedSession] Could not resolve the independent seat for "
                       << runtime->display_name;
      return 503;
    }
    constexpr std::string_view seat_prefix {"hermes-kms-"};
    if (!runtime->seat_id.starts_with(seat_prefix) ||
        runtime->seat_id.size() == seat_prefix.size()) {
      BOOST_LOG(error) << "[IsolatedSession] Invalid Hermes-KMS seat name "
                       << runtime->seat_id;
      return 503;
    }
    const auto seat_instance = runtime->seat_id.substr(seat_prefix.size());
    if (!std::ranges::all_of(seat_instance, [](unsigned char ch) {
          return std::isdigit(ch);
        })) {
      BOOST_LOG(error) << "[IsolatedSession] Invalid Hermes-KMS seat instance "
                       << runtime->seat_id;
      return 503;
    }
    runtime->seatd_socket =
      (std::filesystem::path {"/run/hermes-kms-seatd"} /
       seat_instance /
       "seatd.sock")
        .string();
    runtime->env = _env;
    populate_session_environment(runtime->env, app, *launch_session, *runtime);
    std::error_code socket_ec;
    if (!std::filesystem::is_socket(runtime->seatd_socket, socket_ec) ||
        ::access(runtime->seatd_socket.c_str(), R_OK | W_OK) != 0) {
      BOOST_LOG(error) << "[IsolatedSession] Private seat broker is unavailable at "
                       << runtime->seatd_socket
                       << "; enable hermes-kms-seatd@" << seat_instance
                       << ".service and grant this user access to the seat group.";
      return 503;
    }

    if (!app.output.empty() && app.output != "null") {
      runtime->pipe.reset(std::fopen(app.output.c_str(), "a"));
    }

    for (size_t index = 0; index < app.prep_cmds.size(); ++index) {
      if (launch_was_cancelled()) {
        return 503;
      }
      const auto &cmd = app.prep_cmds[index];
      if (cmd.do_cmd.empty()) {
        runtime->prep_completed = index + 1;
        continue;
      }

      std::error_code ec;
      auto working_dir = app.working_dir.empty() ?
                           find_working_directory(cmd.do_cmd, runtime->env) :
                           boost::filesystem::path {app.working_dir};
      BOOST_LOG(info) << "[IsolatedSession] Executing Do Cmd: [" << cmd.do_cmd
                      << "] elevated: " << cmd.elevated;
      auto child = platf::run_command(
        cmd.elevated,
        true,
        cmd.do_cmd,
        working_dir,
        runtime->env,
        runtime->pipe.get(),
        ec,
        nullptr
      );
      if (ec) {
        BOOST_LOG(error) << "[IsolatedSession] Could not start prep command: " << ec.message();
        return -1;
      }
      child.wait();
      if (child.exit_code() != 0) {
        BOOST_LOG(error) << "[IsolatedSession] Prep command failed with code "
                         << child.exit_code();
        return -1;
      }
      runtime->prep_completed = index + 1;
      if (launch_was_cancelled()) {
        return 503;
      }
    }

    const int width = static_cast<int>(launch_session->width ? launch_session->width : 1920);
    const int height = static_cast<int>(launch_session->height ? launch_session->height : 1080);
    const int refresh_hz = std::max(1, (launch_session->fps ? launch_session->fps : 60000) / 1000);
    const auto connector = VDISPLAY::getHermesKmsConnectorName(runtime->display_name);
    std::string launch_command;

    if (runtime->profile == "application") {
      if (app.cmd.empty()) {
        BOOST_LOG(error) << "[IsolatedSession] Application profile requires a command.";
        return -1;
      }
      if (boost::process::v1::search_path("gamescope").empty()) {
        BOOST_LOG(error) << "[IsolatedSession] Application profile requires gamescope.";
        return -1;
      }

      launch_command = std::format(
        "gamescope --backend=drm -W {} -H {} -w {} -h {} -r {} "
        "--xwayland-count 1 --expose-wayland --force-windows-fullscreen{} -- {}",
        width,
        height,
        width,
        height,
        refresh_hz,
        connector.empty() ? std::string {} : " -O " + shell_quote(connector),
        app.cmd
      );
    } else {
      if (boost::process::v1::search_path("weston").empty()) {
        BOOST_LOG(error) << "[IsolatedSession] Desktop profile requires weston.";
        return -1;
      }

      const auto weston_log =
        (std::filesystem::path {runtime->runtime_dir} / "weston.log").string();
      launch_command = std::format(
        "weston --backend=drm --drm-device={} "
        "--seat={} --continue-without-input --socket={} --xwayland --shell=desktop "
        "--idle-time=0 --log={}",
        shell_quote(card_basename(runtime->drm_device_path)),
        shell_quote(runtime->seat_id),
        shell_quote(runtime->wayland_display),
        shell_quote(weston_log)
      );
    }

    auto working_dir = app.working_dir.empty() ?
                         boost::filesystem::path {runtime->runtime_dir} :
                         boost::filesystem::path {app.working_dir};
    std::error_code launch_ec;
    BOOST_LOG(info) << "[IsolatedSession] Starting " << runtime->profile
                    << " runtime " << runtime->runtime_id
                    << " on seat " << runtime->seat_id
                    << " on " << runtime->drm_device_path
                    << " for " << runtime->display_name;
    runtime->process = platf::run_command(
      app.elevated,
      true,
      launch_command,
      working_dir,
      runtime->env,
      runtime->pipe.get(),
      launch_ec,
      &runtime->process_group
    );
    if (launch_ec) {
      BOOST_LOG(error) << "[IsolatedSession] Could not start compositor: "
                       << launch_ec.message();
      return -1;
    }

    bool scanout_ready = false;
    int active_width = 0;
    int active_height = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (launch_was_cancelled()) {
        return 503;
      }
      if (!runtime->process.running()) {
        BOOST_LOG(error) << "[IsolatedSession] Compositor exited before scanout became ready"
                         << " (code=" << runtime->process.native_exit_code() << ").";
        return 503;
      }

      const int capture_fd = VDISPLAY::hermesKmsOpenCapture(runtime->display_name);
      if (capture_fd >= 0) {
        scanout_ready = VDISPLAY::hermesKmsCaptureSize(
          capture_fd,
          active_width,
          active_height
        );
        ::close(capture_fd);
      }
      if (scanout_ready && active_width > 0 && active_height > 0) {
        break;
      }
      std::this_thread::sleep_for(50ms);
    }
    if (!scanout_ready) {
      BOOST_LOG(error) << "[IsolatedSession] Timed out waiting for compositor scanout on "
                       << runtime->display_name;
      return 503;
    }
    if (launch_was_cancelled()) {
      return 503;
    }
    if (active_width != width || active_height != height) {
      BOOST_LOG(warning) << "[IsolatedSession] Requested " << width << 'x' << height
                         << " but compositor scanout is " << active_width << 'x'
                         << active_height << ". Capture will follow scanout.";
    }

    if (runtime->profile == "application") {
      const auto compositor_socket = discover_wayland_socket(runtime->runtime_dir);
      if (!compositor_socket.empty()) {
        runtime->wayland_display = compositor_socket;
        runtime->env["WAYLAND_DISPLAY"] = compositor_socket;
        runtime->env["HERMES_WAYLAND_DISPLAY"] = compositor_socket;
      } else {
        BOOST_LOG(warning) << "[IsolatedSession] Gamescope scanout is ready but its "
                              "Wayland socket was not discovered; detached Wayland "
                              "commands may need GAMESCOPE_WAYLAND_DISPLAY.";
      }
    }

    if (runtime->profile == "desktop" && !app.cmd.empty()) {
      std::error_code app_ec;
      auto app_working_dir = app.working_dir.empty() ?
                               find_working_directory(app.cmd, runtime->env) :
                               boost::filesystem::path {app.working_dir};
      BOOST_LOG(info) << "[IsolatedSession] Starting desktop application: ["
                      << app.cmd << ']';
      auto desktop_app = platf::run_command(
        app.elevated,
        true,
        app.cmd,
        app_working_dir,
        runtime->env,
        runtime->pipe.get(),
        app_ec,
        &runtime->process_group
      );
      if (app_ec) {
        BOOST_LOG(error) << "[IsolatedSession] Could not start desktop application: "
                         << app_ec.message();
        return -1;
      }
      desktop_app.detach();
    }
    if (launch_was_cancelled()) {
      return 503;
    }

    runtime->env["APOLLO_APP_STATUS"] = "RUNNING";
    runtime->env["HERMES_APP_STATUS"] = "RUNNING";
    for (const auto &command : app.detached) {
      std::error_code ec;
      auto detached_working_dir = app.working_dir.empty() ?
                                    find_working_directory(command, runtime->env) :
                                    boost::filesystem::path {app.working_dir};
      auto child = platf::run_command(
        app.elevated,
        true,
        command,
        detached_working_dir,
        runtime->env,
        runtime->pipe.get(),
        ec,
        nullptr
      );
      if (ec) {
        BOOST_LOG(warning) << "[IsolatedSession] Detached command failed to start: "
                           << ec.message();
      } else {
        child.detach();
      }
    }

    {
      static std::mutex encoder_probe_mutex;
      std::lock_guard<std::mutex> probe_lock(encoder_probe_mutex);
      const auto previous_output = config::video.output_name;
      config::video.output_name = display_device::map_display_name(runtime->display_name);
      auto restore_output = util::fail_guard([&]() {
        config::video.output_name = previous_output;
      });
      if (rtsp_stream::session_count() == 0 && video::probe_encoders() &&
          !config::video.ignore_encoder_probe_failure) {
        return 503;
      }
    }
    if (launch_was_cancelled()) {
      return 503;
    }

    launch_session->isolated_session = true;
    launch_session->isolated_runtime_owner_id = runtime->launch_session_id;
    launch_session->isolated_session_profile = runtime->profile;
    launch_session->isolated_runtime_id = runtime->runtime_id;
    launch_session->isolated_seat_id = runtime->seat_id;
    launch_session->wayland_display = runtime->wayland_display;
    launch_session->virtual_display = true;

    {
      std::lock_guard<std::mutex> lock(isolated_runtimes_mutex);
      // Atomically hand the launch from the cancellation registry to the
      // runtime registry. A concurrent /cancel either marks us before this
      // check or waits for the lock and finds the newly registered runtime.
      if (launch_cancelled->load(std::memory_order_relaxed)) {
        BOOST_LOG(info) << "[IsolatedSession] Launch cancelled during runtime handoff for client "
                        << launch_session->device_name;
        return 503;
      }
      isolated_runtimes[runtime->launch_session_id] = runtime;
    }
    cleanup.disable();

    BOOST_LOG(info) << "[IsolatedSession] Runtime " << runtime->runtime_id
                    << " ready at " << active_width << 'x' << active_height
                    << '@' << refresh_hz << "Hz";
    return 0;
#endif
  }

  void proc_t::terminate_isolated(uint32_t launch_session_id) {
#ifndef _WIN32
    std::shared_ptr<isolated_runtime_t> runtime;
    {
      std::lock_guard<std::mutex> lock(isolated_runtimes_mutex);
      const auto it = isolated_runtimes.find(launch_session_id);
      if (it == isolated_runtimes.end()) {
        return;
      }
      runtime = std::move(it->second);
      isolated_runtimes.erase(it);
    }
    stop_isolated_runtime(runtime);
#else
    (void) launch_session_id;
#endif
  }

  void proc_t::disconnect_isolated(uint32_t launch_session_id) {
#ifndef _WIN32
    bool terminate_on_pause = false;
    {
      std::lock_guard<std::mutex> lock(isolated_runtimes_mutex);
      const auto it = isolated_runtimes.find(launch_session_id);
      if (it == isolated_runtimes.end() || !it->second) {
        return;
      }
      terminate_on_pause = it->second->app.terminate_on_pause;
      if (!terminate_on_pause) {
        BOOST_LOG(info) << "[IsolatedSession] Keeping " << it->second->runtime_id
                        << " alive for client resume.";
        return;
      }
    }
    terminate_isolated(launch_session_id);
#else
    (void) launch_session_id;
#endif
  }

  void proc_t::terminate_isolated_client(const std::string &client_uuid) {
#ifndef _WIN32
    std::shared_ptr<isolated_runtime_t> runtime;
    {
      std::lock_guard<std::mutex> lock(isolated_runtimes_mutex);
      if (const auto launching = isolated_launching_clients.find(client_uuid);
          launching != isolated_launching_clients.end() &&
          launching->second) {
        launching->second->store(true, std::memory_order_relaxed);
      }
      runtime = find_isolated_client_locked(client_uuid);
      if (runtime) {
        isolated_runtimes.erase(runtime->launch_session_id);
      }
    }
    stop_isolated_runtime(runtime);
#else
    (void) client_uuid;
#endif
  }

  void proc_t::terminate_all_isolated() {
#ifndef _WIN32
    std::vector<std::shared_ptr<isolated_runtime_t>> runtimes;
    {
      std::lock_guard<std::mutex> lock(isolated_runtimes_mutex);
      for (const auto &[client_uuid, cancelled] : isolated_launching_clients) {
        if (cancelled) {
          cancelled->store(true, std::memory_order_relaxed);
        }
      }
      for (auto &[id, runtime] : isolated_runtimes) {
        runtimes.emplace_back(std::move(runtime));
      }
      isolated_runtimes.clear();
    }
    for (const auto &runtime : runtimes) {
      stop_isolated_runtime(runtime);
    }
#endif
  }

  int proc_t::running_for_client(const std::string &client_uuid) {
#ifndef _WIN32
    std::shared_ptr<isolated_runtime_t> stale;
    int app_id = 0;
    {
      std::lock_guard<std::mutex> lock(isolated_runtimes_mutex);
      const auto runtime = find_isolated_client_locked(client_uuid);
      if (!runtime) {
        return 0;
      }
      if (isolated_runtime_running(runtime)) {
        return util::from_view(runtime->app.id);
      }
      stale = runtime;
      isolated_runtimes.erase(runtime->launch_session_id);
    }
    stop_isolated_runtime(stale);
    return app_id;
#else
    (void) client_uuid;
    return running();
#endif
  }

  std::string proc_t::running_app_uuid_for_client(const std::string &client_uuid) {
#ifndef _WIN32
    std::lock_guard<std::mutex> lock(isolated_runtimes_mutex);
    const auto runtime = find_isolated_client_locked(client_uuid);
    return isolated_runtime_running(runtime) ? runtime->app.uuid : std::string {};
#else
    (void) client_uuid;
    return get_running_app_uuid();
#endif
  }

  bool proc_t::any_running() {
#ifndef _WIN32
    std::vector<std::shared_ptr<isolated_runtime_t>> stopped;
    bool isolated_running = false;
    {
      std::lock_guard<std::mutex> lock(isolated_runtimes_mutex);
      for (auto it = isolated_runtimes.begin(); it != isolated_runtimes.end();) {
        const auto &runtime = it->second;
        if (isolated_runtime_running(runtime)) {
          isolated_running = true;
          ++it;
          continue;
        }
        stopped.emplace_back(runtime);
        it = isolated_runtimes.erase(it);
      }
    }
    for (const auto &runtime : stopped) {
      stop_isolated_runtime(runtime);
    }
    if (isolated_running) {
      return true;
    }
#endif
    return running() != 0;
  }

  boost::process::v1::environment proc_t::get_session_env(uint32_t launch_session_id) {
#ifndef _WIN32
    std::lock_guard<std::mutex> lock(isolated_runtimes_mutex);
    const auto it = isolated_runtimes.find(launch_session_id);
    if (it != isolated_runtimes.end() && it->second) {
      return it->second->env;
    }
#endif
    return _env;
  }

  bool proc_t::prepare_isolated_resume(
    std::shared_ptr<rtsp_stream::launch_session_t> launch_session
  ) {
#ifndef _WIN32
    std::lock_guard<std::mutex> lock(isolated_runtimes_mutex);
    const auto runtime = find_isolated_client_locked(launch_session->unique_id);
    if (!isolated_runtime_running(runtime)) {
      return false;
    }

    launch_session->isolated_session = true;
    launch_session->isolated_runtime_owner_id = runtime->launch_session_id;
    launch_session->isolated_session_profile = runtime->profile;
    launch_session->isolated_runtime_id = runtime->runtime_id;
    launch_session->isolated_seat_id = runtime->seat_id;
    launch_session->session_scoped_virtual_display = true;
    launch_session->session_virtual_display_cleanup_pending = false;
    launch_session->virtual_display = true;
    launch_session->display_name = runtime->display_name;
    launch_session->display_guid = runtime->display_guid;
    launch_session->drm_device_path = runtime->drm_device_path;
    launch_session->wayland_display = runtime->wayland_display;
    return true;
#else
    (void) launch_session;
    return false;
#endif
  }

  int proc_t::execute(const ctx_t& app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    if (_app_id == input_only_app_id) {
      terminate(false, false);
      std::this_thread::sleep_for(1s);
    } else {
      // Ensure starting from a clean slate
      terminate(false, false);
    }

    _app = app;
    _app_id = util::from_view(app.id);
    _app_name = app.name;
    _launch_session = launch_session;
    allow_client_commands = app.allow_client_commands;

    uint32_t client_width = launch_session->width ? launch_session->width : 1920;
    uint32_t client_height = launch_session->height ? launch_session->height : 1080;

    uint32_t render_width = client_width;
    uint32_t render_height = client_height;

    int scale_factor = launch_session->scale_factor;
    if (_app.scale_factor != 100) {
      scale_factor = _app.scale_factor;
    } else if (scale_factor == 100 && config::video.default_scale_factor != 100) {
      scale_factor = config::video.default_scale_factor;
    }

    if (scale_factor != 100) {
      render_width *= ((float)scale_factor / 100);
      render_height *= ((float)scale_factor / 100);

      // Chop the last bit to ensure the scaled resolution is even numbered
      // Most odd resolutions won't work well
      render_width &= ~1;
      render_height &= ~1;
    }

    launch_session->width = render_width;
    launch_session->height = render_height;

    this->initial_display = config::video.output_name;
    // Executed when returning from function
    auto fg = util::fail_guard([&]() {
#ifndef _WIN32
      if (launch_session->session_virtual_display_cleanup_pending) {
        VDISPLAY::removeVirtualDisplay(launch_session->display_guid);
        launch_session->session_virtual_display_cleanup_pending = false;
        launch_session->session_scoped_virtual_display = false;
        launch_session->display_name.clear();
      }
#endif
      // Restore to user defined output name
      config::video.output_name = this->initial_display;
      terminate();
      display_device::revert_configuration();
    });

    if (!app.gamepad.empty()) {
      _saved_input_config = std::make_shared<config::input_t>(config::input);
      if (app.gamepad == "disabled") {
        config::input.controller = false;
      } else {
        config::input.controller = true;
        config::input.gamepad = app.gamepad;
      }
    }

    // In a standalone Gamescope session (e.g. SteamOS Game Mode), Gamescope is
    // itself the compositor and already provides an isolated, client-sized
    // surface on DRM/KMS. Creating a Hermes virtual display there is both
    // redundant and unactivatable (there's no desktop output-management to
    // adopt the connector), so we skip virtual-display creation and capture the
    // Gamescope output directly via the normal (non-virtual) capture path.
    const auto session_env = platf::detect_session_environment();
    const bool gamescope_session = session_env.kind == platf::session_environment_e::gamescope_session;
    if (gamescope_session) {
      BOOST_LOG(info) << "Gamescope session detected; capturing the Gamescope output directly "
                         "instead of creating a virtual display.";
      launch_session->virtual_display = false;
    }

    const bool needs_virtual_display =
      !gamescope_session &&                 // Not in a standalone Gamescope session
      (config::video.headless_mode        // Headless mode
       || launch_session->virtual_display // User requested virtual display
       || _app.virtual_display            // App is configured to use virtual display
       || !video::allow_encoder_probing() // No active display presents
      );

    bool session_scoped_display_prepared = false;
#ifndef _WIN32
    if (needs_virtual_display &&
        config::video.virtual_display_backend == "hermes_kms" &&
        config::video.hermes_kms_multi_output) {
      if (const int result = prepare_session_virtual_display(launch_session, false); result != 0) {
        return result;
      }
      session_scoped_display_prepared = true;
      this->virtual_display = true;
      this->display_name = launch_session->display_name;
      config::video.output_name = display_device::map_display_name(this->display_name);
    }
#endif

    if (needs_virtual_display && !session_scoped_display_prepared) {
      if (vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
        // Try init driver again
        initVDisplayDriver();
      }

      if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
        // Try set the render adapter matching the capture adapter if user has specified one
        if (!config::video.adapter_name.empty()) {
#ifdef _WIN32
          VDISPLAY::setRenderAdapterByName(platf::from_utf8(config::video.adapter_name));
#else
          VDISPLAY::setRenderAdapterByName(config::video.adapter_name);
#endif
        }

        std::string device_name;
        std::string device_uuid_str;
        uuid_util::uuid_t device_uuid;

        if (_app.use_app_identity) {
          device_name = _app.name;
          if (_app.per_client_app_identity) {
            device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);
            auto app_uuid = uuid_util::uuid_t::parse(_app.uuid);

            // Use XOR to mix the two UUIDs
            device_uuid.b64[0] ^= app_uuid.b64[0];
            device_uuid.b64[1] ^= app_uuid.b64[1];

            device_uuid_str = device_uuid.string();
          } else {
            device_uuid_str = _app.uuid;
            device_uuid = uuid_util::uuid_t::parse(_app.uuid);
          }
        } else {
          device_name = launch_session->device_name;
          device_uuid_str = launch_session->unique_id;
          device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);
        }

#ifdef _WIN32
        memcpy(&launch_session->display_guid, &device_uuid, sizeof(GUID));
#else
        launch_session->display_guid = device_uuid;
#endif

        int target_fps = launch_session->fps ? launch_session->fps : 60000;

        if (target_fps < 1000) {
          target_fps *= 1000;
        }

        if (config::video.double_refreshrate) {
          target_fps *= 2;
        }

#ifdef _WIN32
        std::wstring vdisplayName = VDISPLAY::createVirtualDisplay(
          device_uuid_str.c_str(),
          device_name.c_str(),
          render_width,
          render_height,
          target_fps,
          launch_session->display_guid
        );
#else
        std::string vdisplayName = VDISPLAY::createVirtualDisplay(
          device_uuid_str.c_str(),
          device_name.c_str(),
          render_width,
          render_height,
          target_fps,
          launch_session->display_guid
        );
#endif

        // No matter we get the display name or not, the virtual display might still be created.
        // We need to track it properly to remove the display when the session terminates.
        launch_session->virtual_display = true;

        if (!vdisplayName.empty()) {
          BOOST_LOG(info) << "Virtual Display created at " << vdisplayName;
          bool virtual_display_ready_for_capture = true;

          // Don't change display settings when no params are given
          if (launch_session->width && launch_session->height && launch_session->fps) {
            // Apply display settings
#ifdef _WIN32
            VDISPLAY::changeDisplaySettings(vdisplayName.c_str(), render_width, render_height, target_fps);
#else
            VDISPLAY::changeDisplaySettings(vdisplayName.c_str(), render_width, render_height, target_fps);
#endif
          }

#ifndef _WIN32
          const bool hermes_kms_display = VDISPLAY::isHermesKmsDisplay(vdisplayName);
          virtual_display_ready_for_capture = VDISPLAY::activateVirtualDisplayOutput(vdisplayName);
          if (!virtual_display_ready_for_capture) {
            const char *backend_label = hermes_kms_display ? "Hermes-KMS" : "EVDI";
            if (hermes_kms_display) {
              BOOST_LOG(error) << "The compositor did not activate the Hermes-KMS output. "
                               << "Keeping capture pinned to HERMES-1 so the session fails explicitly instead of streaming a physical display.";
            } else {
              BOOST_LOG(warning) << "The compositor did not activate the " << backend_label << " output. "
                                 << "Falling back to the configured physical display to avoid a black stream.";
            }
          }
#endif

          // Check the ISOLATED DISPLAY configuration setting and rearrange the displays
          if (virtual_display_ready_for_capture && config::video.isolated_virtual_display_option == true) {
            // Apply the isolated display settings
#ifdef _WIN32
            VDISPLAY::changeDisplaySettings2(vdisplayName.c_str(), render_width, render_height, target_fps, true);
#else
            VDISPLAY::changeDisplaySettings2(vdisplayName.c_str(), render_width, render_height, target_fps, true);
            if (!VDISPLAY::enableExclusiveVirtualDisplay(vdisplayName)) {
              BOOST_LOG(warning) << "Virtual display is active, but exclusive mode could not disable physical outputs.";
            }
#endif
          }

          // Only route capture to the virtual output after the compositor has enabled it.
          // Some Wayland compositors expose the DRM connector but cannot add it
          // to their output layout; encoding that untouched virtual buffer yields
          // a black stream while audio continues normally.
          this->virtual_display = virtual_display_ready_for_capture;
#ifdef _WIN32
          this->display_name = platf::to_utf8(vdisplayName);
#else
          if (hermes_kms_display && !virtual_display_ready_for_capture) {
            this->virtual_display = true;
            this->display_name = vdisplayName;
            VDISPLAY::setVirtualDisplayCaptureFallbackActive(false);
          } else {
            VDISPLAY::setVirtualDisplayCaptureFallbackActive(!virtual_display_ready_for_capture);
            if (virtual_display_ready_for_capture) {
              this->display_name = vdisplayName;
            }
          }
#endif

          // When using virtual display, we don't care which display user configured to use.
          // So we always set output_name to the newly created virtual display as a workaround for
          // empty name when probing graphics cards.

          if (virtual_display_ready_for_capture || this->virtual_display) {
            // map_display_name resolves a device id on Windows but returns an
            // empty string on platforms without a settings manager (Linux).
            // Wiping output_name here would make the pre-stream encoder probe
            // look up an empty display and fail, so keep the virtual display's
            // own name in that case (e.g. HERMES-1).
            auto mapped_name = display_device::map_display_name(this->display_name);
            config::video.output_name = mapped_name.empty() ? this->display_name : std::move(mapped_name);
          }
        } else {
          BOOST_LOG(warning) << "Virtual Display creation failed, or cannot get created display name in time!";
        }
      } else {
        // Driver isn't working so we don't need to track virtual display.
        launch_session->virtual_display = false;
      }
    }

    display_device::configure_display(config::video, *launch_session);

    // We should not preserve display state when using virtual display.
    // It is already handled by Windows properly.
    if (this->virtual_display) {
      display_device::reset_persistence();
    }

    // Probe encoders again before streaming to ensure our chosen
    // encoder matches the active GPU (which could have changed
    // due to hotplugging, driver crash, primary monitor change,
    // or any number of other factors).
    if (rtsp_stream::session_count() == 0 && video::probe_encoders()) {
      if (config::video.ignore_encoder_probe_failure) {
        BOOST_LOG(warning) << "Encoder probe failed, but continuing due to user configuration.";
      } else {
        return 503;
      }
    }

    std::string fps_str;
    char fps_buf[8];
    snprintf(fps_buf, sizeof(fps_buf), "%.3f", (float)launch_session->fps / 1000.0f);
    fps_str = fps_buf;

    // Add Stream-specific environment variables
    // Sunshine Compatibility
    _env["SUNSHINE_APP_ID"] = _app.id;
    _env["SUNSHINE_APP_NAME"] = _app.name;
    _env["SUNSHINE_CLIENT_WIDTH"] = std::to_string(render_width);
    _env["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(render_height);
    _env["SUNSHINE_CLIENT_FPS"] = config::sunshine.envvar_compatibility_mode ? std::to_string(std::round((float)launch_session->fps / 1000.0f)) : fps_str;
    _env["SUNSHINE_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["SUNSHINE_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["SUNSHINE_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["SUNSHINE_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";

    _env["APOLLO_APP_ID"] = _app.id;
    _env["APOLLO_APP_NAME"] = _app.name;
    _env["APOLLO_APP_UUID"] = _app.uuid;
    _env["APOLLO_APP_STATUS"] = "STARTING";
    _env["APOLLO_CLIENT_UUID"] = launch_session->unique_id;
    _env["APOLLO_CLIENT_NAME"] = launch_session->device_name;
    _env["APOLLO_CLIENT_WIDTH"] = std::to_string(render_width);
    _env["APOLLO_CLIENT_HEIGHT"] = std::to_string(render_height);
    _env["APOLLO_CLIENT_RENDER_WIDTH"] = std::to_string(launch_session->width);
    _env["APOLLO_CLIENT_RENDER_HEIGHT"] = std::to_string(launch_session->height);
    _env["APOLLO_CLIENT_SCALE_FACTOR"] = std::to_string(scale_factor);
    _env["APOLLO_CLIENT_FPS"] = fps_str;
    _env["APOLLO_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["APOLLO_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["APOLLO_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["APOLLO_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";

    // Hermes-branded env vars (mirror the APOLLO_* lineage contract). New
    // tooling (e.g. hermes-gamescope-launch) reads these; APOLLO_*/SUNSHINE_*
    // remain for client/lineage compatibility.
    _env["HERMES_APP_ID"] = _app.id;
    _env["HERMES_APP_NAME"] = _app.name;
    _env["HERMES_APP_UUID"] = _app.uuid;
    _env["HERMES_APP_STATUS"] = "STARTING";
    _env["HERMES_CLIENT_UUID"] = launch_session->unique_id;
    _env["HERMES_CLIENT_NAME"] = launch_session->device_name;
    _env["HERMES_CLIENT_WIDTH"] = std::to_string(render_width);
    _env["HERMES_CLIENT_HEIGHT"] = std::to_string(render_height);
    _env["HERMES_CLIENT_RENDER_WIDTH"] = std::to_string(launch_session->width);
    _env["HERMES_CLIENT_RENDER_HEIGHT"] = std::to_string(launch_session->height);
    _env["HERMES_CLIENT_SCALE_FACTOR"] = std::to_string(scale_factor);
    _env["HERMES_CLIENT_FPS"] = fps_str;
    _env["HERMES_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["HERMES_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["HERMES_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["HERMES_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";
    _env["HERMES_GAMESCOPE_BACKEND"] = config::video.gamescope_backend;
    _env["HERMES_SESSION_ENVIRONMENT"] = session_env.describe();
    if (!launch_session->enable_hdr) {
      _env["DXVK_HDR"] = "0";
      _env["PROTON_ENABLE_HDR"] = "0";
    }

    int channelCount = launch_session->surround_info & 65535;
    switch (channelCount) {
      case 2:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        _env["APOLLO_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        break;
      case 6:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        _env["APOLLO_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        break;
      case 8:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        _env["APOLLO_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        break;
    }
    _env["SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;
    _env["APOLLO_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;

    if (!_app.output.empty() && _app.output != "null"sv) {
#ifdef _WIN32
      // fopen() interprets the filename as an ANSI string on Windows, so we must convert it
      // to UTF-16 and use the wchar_t variants for proper Unicode log file path support.
      auto woutput = platf::from_utf8(_app.output);

      // Use _SH_DENYNO to allow us to open this log file again for writing even if it is
      // still open from a previous execution. This is required to handle the case of a
      // detached process executing again while the previous process is still running.
      _pipe.reset(_wfsopen(woutput.c_str(), L"a", _SH_DENYNO));
#else
      _pipe.reset(fopen(_app.output.c_str(), "a"));
#endif
    }

    std::error_code ec;
    _app_prep_begin = std::begin(_app.prep_cmds);
    _app_prep_it = _app_prep_begin;

    for (; _app_prep_it != std::end(_app.prep_cmds); ++_app_prep_it) {
      auto &cmd = *_app_prep_it;

      // Skip empty commands
      if (cmd.do_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.do_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Do Cmd: ["sv << cmd.do_cmd << "] elevated: " << cmd.elevated;
      auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
        // We don't want any prep commands failing launch of the desktop.
        // This is to prevent the issue where users reboot their PC and need to log in with Sunshine.
        // permission_denied is typically returned when the user impersonation fails, which can happen when user is not signed in yet.
        if (!(_app.cmd.empty() && ec == std::errc::permission_denied)) {
          return -1;
        }
      }

      child.wait();
      auto ret = child.exit_code();
      if (ret != 0 && ec != std::errc::permission_denied) {
        BOOST_LOG(error) << '[' << cmd.do_cmd << "] failed with code ["sv << ret << ']';
        return -1;
      }
    }

    _env["APOLLO_APP_STATUS"] = "RUNNING";

    for (auto &cmd : _app.detached) {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Spawning ["sv << cmd << "] in ["sv << working_dir << ']';
      auto child = platf::run_command(_app.elevated, true, cmd, working_dir, _env, _pipe.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd << "]: System: "sv << ec.message();
      } else {
        child.detach();
      }
    }

    if (_app.cmd.empty()) {
      BOOST_LOG(info) << "No commands configured, showing desktop..."sv;
      placebo = true;
    } else {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(_app.cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      std::string launch_command = _app.cmd;
      if (launch_session->launch_mode == "gamescope") {
#ifdef _WIN32
        BOOST_LOG(warning) << "Gamescope launch mode is only available on Linux; using the configured application command";
#else
        if (gamescope_session) {
          // We are already inside a standalone Gamescope session (SteamOS Game
          // Mode). Nesting another Gamescope here would be redundant, so run the
          // application directly in the existing session.
          BOOST_LOG(info) << "Gamescope launch mode requested inside an existing Gamescope session; "
                             "running the application directly.";
        } else if (boost::process::v1::search_path("gamescope").empty()) {
          BOOST_LOG(error) << "Gamescope launch mode was requested, but gamescope is not available in PATH";
          return -1;
        } else {
          const int gamescope_fps = std::max(1, (launch_session->fps ? launch_session->fps : 60000) / 1000);
          // This path is only used when a client explicitly requested Gamescope
          // on a desktop session. The primary session path above has already
          // created/activated the virtual display, so Gamescope is nested on the
          // virtual display instead of replacing it. The backend defaults to
          // wayland (nesting in the desktop compositor); override with the
          // gamescope_backend setting when a different backend is needed.
          const std::string &configured_backend = config::video.gamescope_backend;
          const std::string gamescope_backend = (configured_backend.empty() || configured_backend == "auto") ?
                                                   "wayland" :
                                                   configured_backend;
          launch_command = std::format("gamescope --backend={} -W {} -H {} -r {} -o {} -f -e -F fsr --fsr-sharpness 4 -- {}",
                                       gamescope_backend,
                                       launch_session->width,
                                       launch_session->height,
                                       gamescope_fps,
                                       gamescope_fps,
                                       _app.cmd);
        }
#endif
      }

      BOOST_LOG(info) << "Executing: ["sv << launch_command << "] in ["sv << working_dir << ']';
      _process = platf::run_command(_app.elevated, true, launch_command, working_dir, _env, _pipe.get(), ec, &_process_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << launch_command << "]: System: "sv << ec.message();
        return -1;
      }
    }

    _app_launch_time = std::chrono::steady_clock::now();

  #ifdef _WIN32
    auto resetHDRThread = std::thread([this, enable_hdr = launch_session->enable_hdr]{
      // Windows doesn't seem to be able to set HDR correctly when a display is just connected / changed resolution,
      // so we have tooggle HDR for the virtual display manually after a delay.
      auto retryInterval = 200ms;
      while (is_changing_settings_going_to_fail()) {
        if (retryInterval > 2s) {
          BOOST_LOG(warning) << "Restoring HDR settings failed due to retry timeout!";
          return;
        }
        std::this_thread::sleep_for(retryInterval);
        retryInterval *= 2;
      }

      retryInterval = 200ms;
      while (this->display_name.empty()) {
        if (retryInterval > 2s) {
          BOOST_LOG(warning) << "Not getting current display in time! HDR will not be toggled.";
          return;
        }
        std::this_thread::sleep_for(retryInterval);
        retryInterval *= 2;
      }

      // We should have got the actual streaming display by now
      std::string currentDisplay = this->display_name;
      auto currentDisplayW = platf::from_utf8(currentDisplay);

      initial_hdr = VDISPLAY::getDisplayHDRByName(currentDisplayW.c_str());

      if (config::video.dd.hdr_option == config::video_t::dd_t::hdr_option_e::automatic) {
        mode_changed_display = currentDisplay;

        // Try turn off HDR whatever
        // As we always have to apply the workaround by turining off HDR first
        VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), false);

        if (enable_hdr) {
          if (VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), true)) {
            BOOST_LOG(info) << "HDR enabled for display " << currentDisplay;
          } else {
            BOOST_LOG(info) << "HDR enable failed for display " << currentDisplay;
          }
        }
      } else if (initial_hdr) {
        if (VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), false) && VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), true)) {
          BOOST_LOG(info) << "HDR toggled successfully for display " << currentDisplay;
        } else {
          BOOST_LOG(info) << "HDR toggle failed for display " << currentDisplay;
        }
      }
    });

    resetHDRThread.detach();
  #endif

    fg.disable();

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::update_tray_playing(_app.name);
#endif

    return 0;
  }

  int proc_t::running() {
#ifndef _WIN32
    // On POSIX OSes, we must periodically wait for our children to avoid
    // them becoming zombies. This must be synchronized carefully with
    // calls to bp::wait() and platf::process_group_running() which both
    // invoke waitpid() under the hood.
    auto reaper = util::fail_guard([]() {
      while (waitpid(-1, nullptr, WNOHANG) > 0);
    });
#endif

    if (placebo) {
      return _app_id;
    } else if (_app.wait_all && _process_group && platf::process_group_running((std::uintptr_t) _process_group.native_handle())) {
      // The app is still running if any process in the group is still running
      return _app_id;
    } else if (_process.running()) {
      // The app is still running only if the initial process launched is still running
      return _app_id;
    } else if (_app.auto_detach && std::chrono::steady_clock::now() - _app_launch_time < 5s) {
      BOOST_LOG(info) << "App exited with code ["sv << _process.native_exit_code() << "] within 5 seconds of launch. Treating the app as a detached command."sv;
      BOOST_LOG(info) << "Adjust this behavior in the Applications tab or apps.json if this is not what you want."sv;
      placebo = true;

    #if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      if (_process.native_exit_code() != 0) {
        system_tray::update_tray_launch_error(proc::proc.get_last_run_app_name(), _process.native_exit_code());
      }
    #endif

      return _app_id;
    }

    // Perform cleanup actions now if needed
    if (_process) {
      terminate();
    }

    return 0;
  }

  void proc_t::resume() {
    BOOST_LOG(info) << "Session resuming for app [" << _app_name << "].";

    if (!_app.state_cmds.empty()) {
      auto exec_thread = std::thread([cmd_list = _app.state_cmds, app_working_dir = _app.working_dir, _env = _env]() mutable {

        _env["APOLLO_APP_STATUS"] = "RESUMING";

        std::error_code ec;
        auto _state_resume_it = std::begin(cmd_list);

        for (; _state_resume_it != std::end(cmd_list); ++_state_resume_it) {
          auto &cmd = *_state_resume_it;

          // Skip empty commands
          if (cmd.do_cmd.empty()) {
            continue;
          }

          boost::filesystem::path working_dir = app_working_dir.empty() ?
                                                  find_working_directory(cmd.do_cmd, _env) :
                                                  boost::filesystem::path(app_working_dir);
          BOOST_LOG(info) << "Executing Resume Cmd: ["sv << cmd.do_cmd << "] elevated: " << cmd.elevated;
          auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, nullptr, ec, nullptr);

          if (ec) {
            BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
            break;
          }

          child.wait();

          auto ret = child.exit_code();
          if (ret != 0 && ec != std::errc::permission_denied) {
            BOOST_LOG(error) << '[' << cmd.do_cmd << "] failed with code ["sv << ret << ']';
            break;
          }
        }
      });

      exec_thread.detach();
    }
  }

  void proc_t::pause() {
    if (!running()) {
      BOOST_LOG(info) << "Session already stopped, do not run pause commands.";
      return;
    }

    if (_app.terminate_on_pause) {
      BOOST_LOG(info) << "Terminating app [" << _app_name << "] when all clients are disconnected. Pause commands are skipped.";
      terminate();
      return;
    }

    BOOST_LOG(info) << "Session pausing for app [" << _app_name << "].";

    if (!_app.state_cmds.empty()) {
      auto exec_thread = std::thread([cmd_list = _app.state_cmds, app_working_dir = _app.working_dir, _env = _env]() mutable {
        _env["APOLLO_APP_STATUS"] = "PAUSING";

        std::error_code ec;
        auto _state_pause_it = std::begin(cmd_list);

        for (; _state_pause_it != std::end(cmd_list); ++_state_pause_it) {
          auto &cmd = *_state_pause_it;

          // Skip empty commands
          if (cmd.undo_cmd.empty()) {
            continue;
          }

          boost::filesystem::path working_dir = app_working_dir.empty() ?
                                                  find_working_directory(cmd.undo_cmd, _env) :
                                                  boost::filesystem::path(app_working_dir);
          BOOST_LOG(info) << "Executing Pause Cmd: ["sv << cmd.undo_cmd << "] elevated: " << cmd.elevated;
          auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, nullptr, ec, nullptr);

          if (ec) {
            BOOST_LOG(error) << "Couldn't run ["sv << cmd.undo_cmd << "]: System: "sv << ec.message();
            break;
          }

          child.wait();

          auto ret = child.exit_code();
          if (ret != 0 && ec != std::errc::permission_denied) {
            BOOST_LOG(error) << '[' << cmd.undo_cmd << "] failed with code ["sv << ret << ']';
            break;
          }
        }
      });

      exec_thread.detach();
    }

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::update_tray_pausing(proc::proc.get_last_run_app_name());
#endif
  }

  void proc_t::terminate(bool immediate, bool needs_refresh) {
    std::error_code ec;
    placebo = false;

    if (!immediate) {
      terminate_process_group(_process, _process_group, _app.exit_timeout);
    }

    _process = boost::process::v1::child();
    _process_group = boost::process::v1::group();

    _env["APOLLO_APP_STATUS"] = "TERMINATING";

    for (; _app_prep_it != _app_prep_begin; --_app_prep_it) {
      auto &cmd = *(_app_prep_it - 1);

      if (cmd.undo_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.undo_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Undo Cmd: ["sv << cmd.undo_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(warning) << "System: "sv << ec.message();
      }

      child.wait();
      auto ret = child.exit_code();

      if (ret != 0) {
        BOOST_LOG(warning) << "Return code ["sv << ret << ']';
      }
    }

    _pipe.reset();

    bool has_run = _app_id > 0;

    // Revert HDR state
    if (has_run && !mode_changed_display.empty()) {
#ifdef _WIN32
      auto displayNameW = platf::from_utf8(mode_changed_display);
      if (VDISPLAY::setDisplayHDRByName(displayNameW.c_str(), initial_hdr)) {
#else
      if (VDISPLAY::setDisplayHDRByName(mode_changed_display.c_str(), initial_hdr)) {
#endif
        BOOST_LOG(info) << "HDR reverted for display " << mode_changed_display;
      } else {
        BOOST_LOG(info) << "HDR revert failed for display " << mode_changed_display;
      }
    }

    bool used_virtual_display = vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK && _launch_session && _launch_session->virtual_display;
    const bool session_scoped_virtual_display =
      used_virtual_display && _launch_session->session_scoped_virtual_display;
    if (used_virtual_display && !session_scoped_virtual_display) {
#ifndef _WIN32
      if (config::video.isolated_virtual_display_option) {
        VDISPLAY::restoreExclusiveVirtualDisplay();
      }
#endif
      if (VDISPLAY::removeVirtualDisplay(_launch_session->display_guid)) {
        BOOST_LOG(info) << "Virtual Display removed successfully";
      } else if (this->virtual_display) {
        BOOST_LOG(warning) << "Virtual Display remove failed";
      } else {
        BOOST_LOG(warning) << "Virtual Display remove failed, but it seems it was not created correctly either.";
      }
    } else if (session_scoped_virtual_display) {
      BOOST_LOG(debug) << "Session-scoped virtual display cleanup is owned by the streaming session.";
    }

    // Only show the Stopped notification if we actually have an app to stop
    // Since terminate() is always run when a new app has started
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
      if (used_virtual_display) {
        display_device::reset_persistence();
      } else {
        display_device::revert_configuration();
      }

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_stopped(proc::proc.get_last_run_app_name());
#endif
    }

    // Load the configured output_name first
    // to prevent the value being write to empty when the initial terminate happens
    if (!has_run && initial_display.empty()) {
      initial_display = config::video.output_name;
    } else {
      // Restore output name to its original value
      config::video.output_name = initial_display;
    }

    _app_id = -1;
    _app_name.clear();
    _app = {};
    display_name.clear();
    initial_display.clear();
    mode_changed_display.clear();
    _launch_session.reset();
    virtual_display = false;
    allow_client_commands = false;

    if (_saved_input_config) {
      config::input = *_saved_input_config;
      _saved_input_config.reset();
    }

    if (needs_refresh) {
      refresh(config::stream.file_apps, false);
    }
  }

  const std::vector<ctx_t> &proc_t::get_apps() const {
    return _apps;
  }

  std::vector<ctx_t> &proc_t::get_apps() {
    return _apps;
  }

  // Gets application image from application list.
  // Returns image from assets directory if found there.
  // Returns default image if image configuration is not set.
  // Returns http content-type header compatible image type.
  std::string proc_t::get_app_image(int app_id) {
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });
    auto app_image_path = iter == _apps.end() ? std::string() : iter->image_path;

    return validate_app_image_path(app_image_path);
  }

  std::string proc_t::get_last_run_app_name() {
    return _app_name;
  }

  std::string proc_t::get_running_app_uuid() {
    return _app.uuid;
  }

  boost::process::environment proc_t::get_env() {
    return _env;
  }

  proc_t::~proc_t() {
    // It's not safe to call terminate() here because our proc_t is a static variable
    // that may be destroyed after the Boost loggers have been destroyed. Instead,
    // we return a deinit_t to main() to handle termination when we're exiting.
    // Once we reach this point here, termination must have already happened.
    assert(!placebo);
    assert(!_process.running());
  }

  std::string_view::iterator find_match(std::string_view::iterator begin, std::string_view::iterator end) {
    int stack = 0;

    --begin;
    do {
      ++begin;
      switch (*begin) {
        case '(':
          ++stack;
          break;
        case ')':
          --stack;
      }
    } while (begin != end && stack != 0);

    if (begin == end) {
      throw std::out_of_range("Missing closing bracket \')\'");
    }
    return begin;
  }

  std::string parse_env_val(boost::process::v1::native_environment &env, const std::string_view &val_raw) {
    auto pos = std::begin(val_raw);
    auto dollar = std::find(pos, std::end(val_raw), '$');

    std::stringstream ss;

    while (dollar != std::end(val_raw)) {
      auto next = dollar + 1;
      if (next != std::end(val_raw)) {
        switch (*next) {
          case '(':
            {
              ss.write(pos, (dollar - pos));
              auto var_begin = next + 1;
              auto var_end = find_match(next, std::end(val_raw));
              auto var_name = std::string {var_begin, var_end};

#ifdef _WIN32
              // Windows treats environment variable names in a case-insensitive manner,
              // so we look for a case-insensitive match here. This is critical for
              // correctly appending to PATH on Windows.
              auto itr = std::find_if(env.cbegin(), env.cend(), [&](const auto &e) {
                return boost::iequals(e.get_name(), var_name);
              });
              if (itr != env.cend()) {
                // Use an existing case-insensitive match
                var_name = itr->get_name();
              }
#endif

              ss << env[var_name].to_string();

              pos = var_end + 1;
              next = var_end;

              break;
            }
          case '$':
            ss.write(pos, (next - pos));
            pos = next + 1;
            ++next;
            break;
        }

        dollar = std::find(next, std::end(val_raw), '$');
      } else {
        dollar = next;
      }
    }

    ss.write(pos, (dollar - pos));

    return ss.str();
  }

  std::string validate_app_image_path(std::string app_image_path) {
    if (app_image_path.empty()) {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // get the image extension and convert it to lowercase
    auto image_extension = std::filesystem::path(app_image_path).extension().string();
    boost::to_lower(image_extension);

    // return the default box image if extension is not "png"
    if (image_extension != ".png") {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // check if image is in assets directory
    auto full_image_path = std::filesystem::path(SUNSHINE_ASSETS_DIR) / app_image_path;
    if (std::filesystem::exists(full_image_path)) {
      return full_image_path.string();
    } else if (app_image_path == "./assets/steam.png") {
      // handle old default steam image definition
      return SUNSHINE_ASSETS_DIR "/steam.png";
    }

    // check if specified image exists
    std::error_code code;
    if (!std::filesystem::exists(app_image_path, code)) {
      // return default box image if image does not exist
      BOOST_LOG(warning) << "Couldn't find app image at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // image is a png, and not in assets directory
    // return only "content-type" http header compatible image type
    return app_image_path;
  }

  std::optional<std::string> calculate_sha256(const std::string &filename) {
    crypto::md_ctx_t ctx {EVP_MD_CTX_create()};
    if (!ctx) {
      return std::nullopt;
    }

    if (!EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr)) {
      return std::nullopt;
    }

    // Read file and update calculated SHA
    char buf[1024 * 16];
    std::ifstream file(filename, std::ifstream::binary);
    while (file.good()) {
      file.read(buf, sizeof(buf));
      if (!EVP_DigestUpdate(ctx.get(), buf, file.gcount())) {
        return std::nullopt;
      }
    }
    file.close();

    unsigned char result[SHA256_DIGEST_LENGTH];
    if (!EVP_DigestFinal_ex(ctx.get(), result, nullptr)) {
      return std::nullopt;
    }

    // Transform byte-array to string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : result) {
      ss << std::setw(2) << (int) byte;
    }
    return ss.str();
  }

  uint32_t calculate_crc32(const std::string &input) {
    boost::crc_32_type result;
    result.process_bytes(input.data(), input.length());
    return result.checksum();
  }

  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index) {
    // Generate id by hashing name with image data if present
    std::vector<std::string> to_hash;
    to_hash.push_back(app_name);
    auto file_path = validate_app_image_path(app_image_path);
    if (file_path != DEFAULT_APP_IMAGE_PATH) {
      auto file_hash = calculate_sha256(file_path);
      if (file_hash) {
        to_hash.push_back(file_hash.value());
      } else {
        // Fallback to just hashing image path
        to_hash.push_back(file_path);
      }
    }

    // Create combined strings for hash
    std::stringstream ss;
    for_each(to_hash.begin(), to_hash.end(), [&ss](const std::string &s) {
      ss << s;
    });
    auto input_no_index = ss.str();
    ss << index;
    auto input_with_index = ss.str();

    // CRC32 then truncate to signed 32-bit range due to client limitations
    auto id_no_index = std::to_string(abs((int32_t) calculate_crc32(input_no_index)));
    auto id_with_index = std::to_string(abs((int32_t) calculate_crc32(input_with_index)));

    return std::make_tuple(id_no_index, id_with_index);
  }

  /**
   * @brief Migrate the applications stored in the file tree by merging in a new app.
   *
   * This function updates the application entries in *fileTree_p* using the data in *inputTree_p*.
   * If an app in the file tree does not have a UUID, one is generated and inserted.
   * If an app with the same UUID as the new app is found, it is replaced.
   * Additionally, empty keys (such as "prep-cmd" or "detached") and keys no longer needed ("launching", "index")
   * are removed from the input.
   *
   * Legacy versions of Sunshine/Apollo stored boolean and integer values as strings.
   * The following keys are converted:
   *   - Boolean keys: "exclude-global-prep-cmd", "elevated", "auto-detach", "wait-all",
   *                     "use-app-identity", "per-client-app-identity", "virtual-display"
   *   - Integer keys: "exit-timeout"
   *
   * A migration version is stored in the file tree (under "version") so that future changes can be applied.
   *
   * @param fileTree_p Pointer to the JSON object representing the file tree.
   * @param inputTree_p Pointer to the JSON object representing the new app.
   */
  void migrate_apps(nlohmann::json* fileTree_p, nlohmann::json* inputTree_p) {
    std::string new_app_uuid;

    if (inputTree_p) {
      // If the input contains a non-empty "uuid", use it; otherwise generate one.
      if (inputTree_p->contains("uuid") && !(*inputTree_p)["uuid"].get<std::string>().empty()) {
        new_app_uuid = (*inputTree_p)["uuid"].get<std::string>();
      } else {
        new_app_uuid = uuid_util::uuid_t::generate().string();
        (*inputTree_p)["uuid"] = new_app_uuid;
      }

      // Remove "prep-cmd" if empty.
      if (inputTree_p->contains("prep-cmd") && (*inputTree_p)["prep-cmd"].empty()) {
        inputTree_p->erase("prep-cmd");
      }

      // Remove "detached" if empty.
      if (inputTree_p->contains("detached") && (*inputTree_p)["detached"].empty()) {
        inputTree_p->erase("detached");
      }

      // Remove keys that are no longer needed.
      inputTree_p->erase("launching");
      inputTree_p->erase("index");
    }

    // Get the current apps array; if it doesn't exist, create one.
    nlohmann::json newApps = nlohmann::json::array();
    if (fileTree_p->contains("apps") && (*fileTree_p)["apps"].is_array()) {
      for (auto &app : (*fileTree_p)["apps"]) {
        // For apps without a UUID, generate one and remove "launching".
        if (!app.contains("uuid") || app["uuid"].get<std::string>().empty()) {
          app["uuid"] = uuid_util::uuid_t::generate().string();
          app.erase("launching");
          newApps.push_back(std::move(app));
        } else {
          // If an app with the same UUID as the new app is found, replace it.
          if (!new_app_uuid.empty() && app["uuid"].get<std::string>() == new_app_uuid) {
            newApps.push_back(*inputTree_p);
            new_app_uuid.clear();
          } else {
            newApps.push_back(std::move(app));
          }
        }
      }
    }
    // If the new app's UUID has not been merged yet, add it.
    if (!new_app_uuid.empty() && inputTree_p) {
      newApps.push_back(*inputTree_p);
    }
    (*fileTree_p)["apps"] = newApps;
  }

  void migration_v2(nlohmann::json& fileTree) {
    static const int this_version = 2;
    // Determine the current migration version (default to 1 if not present).
    int file_version = 1;
    if (fileTree.contains("version")) {
      try {
        file_version = fileTree["version"].get<int>();
      } catch (const std::exception& e) {
        BOOST_LOG(info) << "Cannot parse apps.json version, treating as v1: " << e.what();
      }
    }

    // If the version is less than this_version, perform legacy conversion.
    if (file_version < this_version) {
      BOOST_LOG(info) << "Migrating app list from v1 to v2...";
      migrate_apps(&fileTree, nullptr);

      // List of keys to convert to booleans.
      std::vector<std::string> boolean_keys = {
        "allow-client-commands",
        "exclude-global-prep-cmd",
        "elevated",
        "auto-detach",
        "wait-all",
        "use-app-identity",
        "per-client-app-identity",
        "virtual-display"
      };

      // List of keys to convert to integers.
      std::vector<std::string> integer_keys = {
        "exit-timeout",
        "scale-factor"
      };

      // Walk through each app and convert legacy string values.
      for (auto &app : fileTree["apps"]) {
        for (const auto &key : boolean_keys) {
          if (app.contains(key)) {
            auto& _key = app[key];
            if (_key.is_string()) {
              std::string s = _key.get<std::string>();
              std::transform(s.begin(), s.end(), s.begin(), ::tolower);  // Normalize to lowercase for comparison
              _key = (s == "true" || s == "on" || s == "yes");
            } else if (_key.is_array()) {
              // Check if the array contains at least one item and interpret the first element
              if (!_key.empty() && _key[0].is_string()) {
                std::string first = _key[0].get<std::string>();
                std::transform(first.begin(), first.end(), first.begin(), ::tolower);  // Normalize
                if (first == "on" || first == "true" || first == "yes") {
                  _key = true;
                } else if (first == "off" || first == "false" || first == "no") {
                  _key = false;
                } else {
                  _key = false;  // Default for unknown values
                }
              } else {
                _key = false;  // Treat empty arrays or non-string first elements as false
              }
            } else {
              // Fallback: Treat truthy/falsey cases
              if (_key.is_boolean()) {
                // Leave booleans as they are
              } else if (_key.is_number()) {
                _key = (_key.get<double>() != 0);  // Non-zero numbers are truthy
              } else if (_key.is_null()) {
                _key = false;  // Null is false
              } else {
                _key = !_key.empty();  // Non-empty objects/arrays are truthy, empty ones are falsey
              }
            }
          }
        }

        for (const auto &key : integer_keys) {
          if (app.contains(key) && app[key].is_string()) {
            std::string s = app[key].get<std::string>();
            app[key] = std::stoi(s);
          }
        }

        // For each entry in the "prep-cmd" array, convert "elevated" if necessary.
        if (app.contains("prep-cmd") && app["prep-cmd"].is_array()) {
          for (auto &prep : app["prep-cmd"]) {
            if (prep.contains("elevated") && prep["elevated"].is_string()) {
              std::string s = prep["elevated"].get<std::string>();
              prep["elevated"] = (s == "true");
            }
          }
        }
      }

      // Update migration version to this_version.
      fileTree["version"] = this_version;

      BOOST_LOG(info) << "Migrated app list from v1 to v2.";
    }
  }

  void migrate(nlohmann::json& fileTree, const std::string& fileName) {
    int last_version = 2;

    int file_version = 0;
    if (fileTree.contains("version")) {
      file_version = fileTree["version"].get<int>();
    }

    if (file_version < last_version) {
      migration_v2(fileTree);
      file_handler::write_file(fileName.c_str(), fileTree.dump(4));
    }
  }

  std::optional<proc::proc_t> parse(const std::string &file_name) {

    // Prepare environment variables.
    auto this_env = boost::this_process::environment();

    std::set<std::string> ids;
    std::vector<proc::ctx_t> apps;
    int i = 0;

    size_t fail_count = 0;
    do {
      // Read the JSON file into a tree.
      nlohmann::json tree;
      try {
        std::string content = file_handler::read_file(file_name.c_str());
        tree = nlohmann::json::parse(content);
      } catch (const std::exception& e) {
        BOOST_LOG(warning) << "Couldn't read apps.json properly! Apps will not be loaded."sv;
        break;
      }

      try {
        migrate(tree, file_name);

        if (tree.contains("env") && tree["env"].is_object()) {
          for (auto &item : tree["env"].items()) {
            this_env[item.key()] = parse_env_val(this_env, item.value().get<std::string>());
          }
        }

        // Ensure the "apps" array exists.
        if (!tree.contains("apps") || !tree["apps"].is_array()) {
          BOOST_LOG(warning) << "No apps were defined in apps.json!!!"sv;
          break;
        }

        // Iterate over each application in the "apps" array.
        for (auto &app_node : tree["apps"]) {
          proc::ctx_t ctx;
          ctx.idx = std::to_string(i);
          ctx.uuid = app_node.at("uuid");

          // Build the list of preparation commands.
          std::vector<proc::cmd_t> prep_cmds;
          bool exclude_global_prep = app_node.value("exclude-global-prep-cmd", false);
          if (!exclude_global_prep) {
            prep_cmds.reserve(config::sunshine.prep_cmds.size());
            for (auto &prep_cmd : config::sunshine.prep_cmds) {
              auto do_cmd = parse_env_val(this_env, prep_cmd.do_cmd);
              auto undo_cmd = parse_env_val(this_env, prep_cmd.undo_cmd);
              prep_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(prep_cmd.elevated)
              );
            }
          }
          if (app_node.contains("prep-cmd") && app_node["prep-cmd"].is_array()) {
            for (auto &prep_node : app_node["prep-cmd"]) {
              std::string do_cmd = parse_env_val(this_env, prep_node.value("do", ""));
              std::string undo_cmd = parse_env_val(this_env, prep_node.value("undo", ""));
              bool elevated = prep_node.value("elevated", false);
              prep_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(elevated)
              );
            }
          }

          // Build the list of pause/resume commands.
          std::vector<proc::cmd_t> state_cmds;
          bool exclude_global_state_cmds = app_node.value("exclude-global-state-cmd", false);
          if (!exclude_global_state_cmds) {
            state_cmds.reserve(config::sunshine.state_cmds.size());
            for (auto &state_cmd : config::sunshine.state_cmds) {
              auto do_cmd = parse_env_val(this_env, state_cmd.do_cmd);
              auto undo_cmd = parse_env_val(this_env, state_cmd.undo_cmd);
              state_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(state_cmd.elevated)
              );
            }
          }
          if (app_node.contains("state-cmd") && app_node["state-cmd"].is_array()) {
            for (auto &prep_node : app_node["state-cmd"]) {
              std::string do_cmd = parse_env_val(this_env, prep_node.value("do", ""));
              std::string undo_cmd = parse_env_val(this_env, prep_node.value("undo", ""));
              bool elevated = prep_node.value("elevated", false);
              state_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(elevated)
              );
            }
          }

          // Build the list of detached commands.
          std::vector<std::string> detached;
          if (app_node.contains("detached") && app_node["detached"].is_array()) {
            for (auto &detached_val : app_node["detached"]) {
              detached.emplace_back(parse_env_val(this_env, detached_val.get<std::string>()));
            }
          }

          // Process other fields.
          if (app_node.contains("output"))
            ctx.output = parse_env_val(this_env, app_node.value("output", ""));
          std::string name = parse_env_val(this_env, app_node.value("name", ""));
          if (app_node.contains("cmd"))
            ctx.cmd = parse_env_val(this_env, app_node.value("cmd", ""));
          if (app_node.contains("working-dir")) {
            ctx.working_dir = parse_env_val(this_env, app_node.value("working-dir", ""));
    #ifdef _WIN32
            // The working directory, unlike the command itself, should not be quoted.
            boost::erase_all(ctx.working_dir, "\"");
            ctx.working_dir += '\\';
    #endif
          }
          if (app_node.contains("image-path"))
            ctx.image_path = parse_env_val(this_env, app_node.value("image-path", ""));

          ctx.elevated = app_node.value("elevated", false);
          ctx.auto_detach = app_node.value("auto-detach", true);
          ctx.wait_all = app_node.value("wait-all", true);
          ctx.exit_timeout = std::chrono::seconds { app_node.value("exit-timeout", 5) };
          ctx.virtual_display = app_node.value("virtual-display", false);
          ctx.scale_factor = app_node.value("scale-factor", 100);
          ctx.use_app_identity = app_node.value("use-app-identity", false);
          ctx.per_client_app_identity = app_node.value("per-client-app-identity", false);
          ctx.allow_client_commands = app_node.value("allow-client-commands", true);
          ctx.terminate_on_pause = app_node.value("terminate-on-pause", false);
          ctx.session_type = app_node.value("session-type", "auto");
          if (ctx.session_type != "auto" &&
              ctx.session_type != "shared" &&
              ctx.session_type != "application" &&
              ctx.session_type != "desktop") {
            BOOST_LOG(warning) << "Unknown session-type '" << ctx.session_type
                               << "' for app " << name << "; using auto.";
            ctx.session_type = "auto";
          }
          ctx.gamepad = app_node.value("gamepad", "");

          // Calculate a unique application id.
          auto possible_ids = calculate_app_id(name, ctx.image_path, i++);
          if (ids.count(std::get<0>(possible_ids)) == 0) {
            ctx.id = std::get<0>(possible_ids);
          } else {
            ctx.id = std::get<1>(possible_ids);
          }
          ids.insert(ctx.id);

          ctx.name = std::move(name);
          ctx.prep_cmds = std::move(prep_cmds);
          ctx.state_cmds = std::move(state_cmds);
          ctx.detached = std::move(detached);

          apps.emplace_back(std::move(ctx));
        }

        fail_count = 0;
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Error happened during app loading: "sv << e.what();

        fail_count += 1;

        if (fail_count >= 3) {
          // No hope for recovering
          BOOST_LOG(warning) << "Couldn't parse/migrate apps.json properly! Apps will not be loaded."sv;
          break;
        }

        BOOST_LOG(warning) << "App format is still invalid! Trying to re-migrate the app list..."sv;

        // Always try migrating from scratch when error happened
        tree["version"] = 0;

        try {
          migrate(tree, file_name);
        } catch (std::exception &e) {
          BOOST_LOG(error) << "Error happened during migration: "sv << e.what();
          break;
        }

        this_env = boost::this_process::environment();
        ids.clear();
        apps.clear();
        i = 0;

        continue;
      }

      break;
    } while (fail_count < 3);

    if (fail_count > 0) {
      BOOST_LOG(warning) << "No applications configured, adding fallback Desktop entry.";
      proc::ctx_t ctx;
      ctx.idx = std::to_string(i);
      ctx.uuid = FALLBACK_DESKTOP_UUID; // Placeholder UUID
      ctx.name = "Desktop (fallback)";
      ctx.image_path = parse_env_val(this_env, "desktop-alt.png");
      ctx.virtual_display = false;
      ctx.scale_factor = 100;
      ctx.use_app_identity = false;
      ctx.per_client_app_identity = false;
      ctx.allow_client_commands = false;
      ctx.terminate_on_pause = false;
      ctx.session_type = "auto";

      ctx.elevated = false;
      ctx.auto_detach = true;
      ctx.wait_all = false; // Desktop doesn't have a specific command to wait for
      ctx.exit_timeout = 5s;

      // Calculate unique ID
      auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
      if (ids.count(std::get<0>(possible_ids)) == 0) {
        // Avoid using index to generate id if possible
        ctx.id = std::get<0>(possible_ids);
      } else {
        // Fallback to include index on collision
        ctx.id = std::get<1>(possible_ids);
      }
      ids.insert(ctx.id);

      apps.emplace_back(std::move(ctx));
    }

    // Virtual Display entry
  #ifdef _WIN32
    if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      proc::ctx_t ctx;
      ctx.idx = std::to_string(i);
      ctx.uuid = VIRTUAL_DISPLAY_UUID;
      ctx.name = "Virtual Display";
      ctx.image_path = parse_env_val(this_env, "virtual_desktop.png");
      ctx.virtual_display = true;
      ctx.scale_factor = 100;
      ctx.use_app_identity = false;
      ctx.per_client_app_identity = false;
      ctx.allow_client_commands = false;
      ctx.terminate_on_pause = false;
      ctx.session_type = "auto";

      ctx.elevated = false;
      ctx.auto_detach = true;
      ctx.wait_all = false;
      ctx.exit_timeout = 5s;

      auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
      if (ids.count(std::get<0>(possible_ids)) == 0) {
        // Avoid using index to generate id if possible
        ctx.id = std::get<0>(possible_ids);
      }
      else {
        // Fallback to include index on collision
        ctx.id = std::get<1>(possible_ids);
      }
      ids.insert(ctx.id);

      apps.emplace_back(std::move(ctx));
    }
  #endif

    if (config::input.enable_input_only_mode) {
      // Input Only entry
      {
        proc::ctx_t ctx;
        ctx.idx = std::to_string(i);
        ctx.uuid = REMOTE_INPUT_UUID;
        ctx.name = "Remote Input";
        ctx.image_path = parse_env_val(this_env, "input_only.png");
        ctx.virtual_display = false;
        ctx.scale_factor = 100;
        ctx.use_app_identity = false;
        ctx.per_client_app_identity = false;
        ctx.allow_client_commands = false;
        ctx.terminate_on_pause = true; // There's no need to keep an active input only session ongoing
        ctx.session_type = "shared";

        ctx.elevated = false;
        ctx.auto_detach = true;
        ctx.wait_all = true;
        ctx.exit_timeout = 5s;

        auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        }
        else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        ids.insert(ctx.id);

        input_only_app_id_str = ctx.id;
        input_only_app_id = util::from_view(ctx.id);

        apps.emplace_back(std::move(ctx));
      }

      // Terminate entry
      {
        proc::ctx_t ctx;
        ctx.idx = std::to_string(i);
        ctx.uuid = TERMINATE_APP_UUID;
        ctx.name = "Terminate";
        ctx.image_path = parse_env_val(this_env, "terminate.png");
        ctx.virtual_display = false;
        ctx.scale_factor = 100;
        ctx.use_app_identity = false;
        ctx.per_client_app_identity = false;
        ctx.allow_client_commands = false;
        ctx.terminate_on_pause = false;
        ctx.session_type = "shared";

        ctx.elevated = false;
        ctx.auto_detach = true;
        ctx.wait_all = true;
        ctx.exit_timeout = 5s;

        auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        }
        else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        // ids.insert(ctx.id);

        terminate_app_id_str = ctx.id;
        terminate_app_id = util::from_view(ctx.id);

        apps.emplace_back(std::move(ctx));
      }
    }

    return proc::proc_t {
      std::move(this_env),
      std::move(apps)
    };
  }

  void refresh(const std::string &file_name, bool needs_terminate) {
    if (needs_terminate) {
      proc.terminate_all_isolated();
      proc.terminate(false, false);
    }

  #ifdef _WIN32
    size_t fail_count = 0;
    while (fail_count < 5 && vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
      initVDisplayDriver();
      if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
        break;
      }

      fail_count += 1;
      std::this_thread::sleep_for(1s);
    }
  #endif

    auto proc_opt = proc::parse(file_name);

    if (proc_opt) {
      proc = std::move(*proc_opt);
    }
  }
}  // namespace proc
