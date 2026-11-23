/**
 * @file src/system_tray.cpp
 * @brief Definitions for the system tray icon and notification system.
 */
// macros
#if defined HERMES_TRAY && HERMES_TRAY >= 1

    // Use absolute paths for tray icons to ensure they work with all icon themes
    #define TRAY_ICON "/usr/share/icons/hicolor/scalable/status/" HERMES_TRAY_PREFIX "-tray.svg"
    #define TRAY_ICON_PLAYING "/usr/share/icons/hicolor/scalable/status/" HERMES_TRAY_PREFIX "-playing.svg"
    #define TRAY_ICON_PAUSING "/usr/share/icons/hicolor/scalable/status/" HERMES_TRAY_PREFIX "-pausing.svg"
    #define TRAY_ICON_LOCKED "/usr/share/icons/hicolor/scalable/status/" HERMES_TRAY_PREFIX "-locked.svg"

  #define TRAY_MSG_NO_APP_RUNNING "Reload Apps"

  #ifndef BOOST_PROCESS_VERSION
    #define BOOST_PROCESS_VERSION 1
  #endif

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <csignal>
  #include <format>
  #include <string>
  #include <thread>

  // lib includes
  #include <boost/filesystem.hpp>
  #include <boost/process/v1/environment.hpp>
  #include <tray/src/tray.h>

  // local includes
  #include "config.h"
  #include "confighttp.h"
  #include "logging.h"
  #include "platform/common.h"
  #include "process.h"
  #include "network.h"
  #include "src/entry_handler.h"
  #include "platform/linux/misc.h"

using namespace std::literals;

// system_tray namespace
namespace system_tray {
  static std::atomic tray_initialized = false;

  // Tray thread state
  static std::thread tray_thread;
  static std::atomic tray_thread_running = false;
  static std::atomic tray_thread_should_exit = false;

  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Opening UI from system tray"sv;
    launch_ui();
  }

  void
  tray_force_stop_cb(struct tray_menu *item) {
    BOOST_LOG(info) << "Force stop from system tray"sv;
    proc::proc.terminate();
  }

  void tray_restart_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Restarting from system tray"sv;

    proc::proc.terminate();
    platf::restart();
  }

  void tray_quit_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Quitting from system tray"sv;

    proc::proc.terminate();
    platf::request_user_service_stop();

    lifetime::exit_sunshine(0, true);
  }

  // Tray menu
  static struct tray tray = {
    .icon = TRAY_ICON,
    .tooltip = PROJECT_NAME,
    .menu =
      (struct tray_menu[]) {
        // todo - use boost/locale to translate menu strings
        { .text = "Open " PROJECT_NAME, .cb = tray_open_ui_cb },
        { .text = "-" },
        // { .text = "-" },
        // { .text = "Donate",
        //   .submenu =
        //     (struct tray_menu[]) {
        //       { .text = "GitHub Sponsors", .cb = tray_donate_github_cb },
        //       { .text = "MEE6", .cb = tray_donate_mee6_cb },
        //       { .text = "Patreon", .cb = tray_donate_patreon_cb },
        //       { .text = "PayPal", .cb = tray_donate_paypal_cb },
        //       { .text = nullptr } } },
        // { .text = "-" },
        { .text = TRAY_MSG_NO_APP_RUNNING, .cb = tray_force_stop_cb },
        {.text = "Restart", .cb = tray_restart_cb},
        {.text = "Quit", .cb = tray_quit_cb},
        {.text = nullptr}
      },
    .iconPathCount = 4,
    .allIconPaths = {TRAY_ICON, TRAY_ICON_LOCKED, TRAY_ICON_PLAYING, TRAY_ICON_PAUSING},
  };

  int init_tray() {

    if (tray_init(&tray) < 0) {
      BOOST_LOG(warning) << "Failed to create system tray"sv;
      return 1;
    }

    BOOST_LOG(info) << "System tray created"sv;
    tray_initialized = true;
    return 0;
  }

  int process_tray_events() {
    if (!tray_initialized) {
      return 1;
    }

    // Process one iteration of the tray loop with non-blocking mode (0)
    if (const int result = tray_loop(0); result != 0) {
      BOOST_LOG(warning) << "System tray loop failed"sv;
      return result;
    }

    return 0;
  }

  int end_tray() {
    if (tray_initialized) {
      tray_initialized = false;
      tray_exit();
    }
    return 0;
  }

  void update_tray_playing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON_PLAYING;

    tray_update(&tray);
    tray.icon = TRAY_ICON_PLAYING;
    tray.notification_title = "App launched";
    char msg[256];
    static char force_close_msg[256];
    snprintf(msg, std::size(msg), "%s launched.", app_name.c_str());
    snprintf(force_close_msg, std::size(force_close_msg), "Force close [%s]", app_name.c_str());
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON_PLAYING;
    tray.tooltip = PROJECT_NAME;
    tray.menu[2].text = force_close_msg;
    tray_update(&tray);
  }

  void update_tray_pausing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON_PAUSING;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), "Streaming paused for %s", app_name.c_str());
    tray.icon = TRAY_ICON_PAUSING;
    tray.notification_title = "Stream Paused";
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON_PAUSING;
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void update_tray_stopped(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), "Streaming stopped for %s", app_name.c_str());
    tray.icon = TRAY_ICON;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = "Application Stopped";
    tray.notification_text = msg;
    tray.tooltip = PROJECT_NAME;
    tray.menu[2].text = TRAY_MSG_NO_APP_RUNNING;
    tray_update(&tray);
  }

  void
  update_tray_launch_error(std::string app_name, int exit_code) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), "Application %s exited too fast with code %d. Click here to terminate the stream.", app_name.c_str(), exit_code);
    tray.icon = TRAY_ICON;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = "Launch Error";
    tray.notification_text = msg;
    tray.notification_cb = []() {
      BOOST_LOG(info) << "Force stop from notification"sv;
      proc::proc.terminate();
    };
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void update_tray_session_error(std::string reason) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray_update(&tray);

    char msg[512];
    snprintf(msg, std::size(msg), "%s", reason.c_str());
    tray.icon = TRAY_ICON;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = "Stream could not start";
    tray.notification_text = msg;
    tray.notification_cb = []() {
      launch_ui();
    };
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void update_tray_require_pin() {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    tray.icon = TRAY_ICON;
    tray.notification_title = "Incoming Pairing Request";
    tray.notification_text = "Click here to complete the pairing process";
    tray.notification_icon = TRAY_ICON_LOCKED;
    tray.tooltip = PROJECT_NAME;
    tray.notification_cb = []() {
      launch_ui("/pin#PIN");
    };
    tray_update(&tray);
  }

  void
  update_tray_paired(std::string device_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), "Device %s paired Succesfully. Please make sure you have access to the device.", device_name.c_str());
    tray.notification_title = "Device Paired Succesfully";
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON;
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void
  update_tray_client_connected(std::string client_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), "%s has connected to the session.", client_name.c_str());
    tray.notification_title = "Client Connected";
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON;
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  // Threading functions available on all platforms
  static void tray_thread_worker() {
    BOOST_LOG(info) << "System tray thread started"sv;

    // Initialize the tray in this thread
    if (init_tray() != 0) {
      BOOST_LOG(error) << "Failed to initialize tray in thread"sv;
      tray_thread_running = false;
      return;
    }

    tray_thread_running = true;

    // Main tray event loop
    while (!tray_thread_should_exit) {
      if (process_tray_events() != 0) {
        BOOST_LOG(warning) << "Tray event processing failed in thread"sv;
        break;
      }

      // Sleep to avoid busy waiting
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Clean up the tray
    end_tray();
    tray_thread_running = false;
    BOOST_LOG(info) << "System tray thread ended"sv;
  }

  int init_tray_threaded() {
    if (tray_thread_running) {
      BOOST_LOG(warning) << "Tray thread is already running"sv;
      return 1;
    }

    static const std::string title_str = "Open " PROJECT_NAME " (" + config::nvhttp.sunshine_name + ":" + std::to_string(net::map_port(confighttp::PORT_HTTPS)) + ")";
    tray.menu[0].text = title_str.c_str();

    if (config::sunshine.hide_tray_controls) {
      tray.menu[1].text = nullptr;
    }

    tray_thread_should_exit = false;

    try {
      tray_thread = std::thread(tray_thread_worker);

      // Wait for the thread to start and initialize
      const auto start_time = std::chrono::steady_clock::now();
      while (!tray_thread_running && !tray_thread_should_exit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Timeout after 10 seconds
        if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(10)) {
          BOOST_LOG(error) << "Tray thread initialization timeout"sv;
          tray_thread_should_exit = true;
          if (tray_thread.joinable()) {
            tray_thread.join();
          }
          return 1;
        }
      }

      if (!tray_thread_running) {
        BOOST_LOG(error) << "Tray thread failed to start"sv;
        if (tray_thread.joinable()) {
          tray_thread.join();
        }
        return 1;
      }

      BOOST_LOG(info) << "System tray thread initialized successfully"sv;
      return 0;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to create tray thread: " << e.what();
      return 1;
    }
  }

  int end_tray_threaded() {
    if (!tray_thread_running) {
      return 0;
    }

    BOOST_LOG(info) << "Stopping system tray thread"sv;
    tray_thread_should_exit = true;

    if (tray_thread.joinable()) {
      tray_thread.join();
    }

    BOOST_LOG(info) << "System tray thread stopped"sv;
    return 0;
  }

}  // namespace system_tray

  #ifdef BOOST_PROCESS_VERSION
    #undef BOOST_PROCESS_VERSION 1
  #endif

#endif
