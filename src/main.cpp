/**
 * @file src/main.cpp
 * @brief Definitions for the main entry point for Sunshine.
 */
// standard includes
#include <algorithm>
#include <codecvt>
#include <csignal>
#include <fstream>
#include <iostream>

// local includes
#include "confighttp.h"
#include "display_device.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "main.h"
#include "nvhttp.h"
#include "process.h"
#include "system_tray.h"
#include "upnp.h"
#include "uuid.h"
#include "video.h"

  #include "platform/linux/virtual_display.h"
  #include "platform/linux/prelogin.h"
  #include "platform/linux/session_attach.h"

  #include <boost/log/core.hpp>

#define PROBE_DISPLAY_UUID "38F72B96-B00C-4F21-8B6C-E1BFF1602B0E"

extern "C" {
#include "rswrapper.h"
}

using namespace std::literals;

std::map<int, std::function<void()>> signal_handlers;

void on_signal_forwarder(int sig) {
  signal_handlers.at(sig)();
}

template<class FN>
void on_signal(int sig, FN &&fn) {
  signal_handlers.emplace(sig, std::forward<FN>(fn));

  std::signal(sig, on_signal_forwarder);
}

std::map<std::string_view, std::function<int(const char *name, int argc, char **argv)>> cmd_to_func {
  {"creds"sv, [](const char *name, int argc, char **argv) {
     return args::creds(name, argc, argv);
   }},
  {"help"sv, [](const char *name, int argc, char **argv) {
     return args::help(name);
   }},
  {"version"sv, [](const char *name, int argc, char **argv) {
     return args::version();
   }},
};


#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
constexpr bool tray_is_enabled = true;
#else
constexpr bool tray_is_enabled = false;
#endif

void mainThreadLoop(const std::shared_ptr<safe::event_t<bool>> &shutdown_event) {
  bool run_loop = false;

  // Conditions that would require the main thread event loop
  run_loop = tray_is_enabled;

  if (!run_loop) {
    BOOST_LOG(info) << "No main thread features enabled, skipping event loop"sv;
    return;
  }

  // Main thread event loop
  BOOST_LOG(info) << "Starting main loop"sv;
  while (true) {
    if (shutdown_event->peek()) {
      BOOST_LOG(info) << "Shutdown event detected, breaking main loop"sv;
      if (tray_is_enabled && config::sunshine.system_tray) {
        system_tray::end_tray();
      }
      break;
    }

    if (tray_is_enabled) {
      system_tray::process_tray_events();
    }

    // Sleep to avoid busy waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

int main(int argc, char *argv[]) {
  lifetime::argv = argv;

  task_pool_util::TaskPool::task_id_t force_shutdown = nullptr;


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // Use UTF-8 conversion for the default C++ locale (used by boost::log)
  std::locale utf8_locale(std::locale(), new std::codecvt_utf8<wchar_t>);
  std::locale::global(utf8_locale);
  boost::filesystem::path::imbue(utf8_locale);
#pragma GCC diagnostic pop

  mail::man = std::make_shared<safe::mail_raw_t>();

  // `--prelogin` answers on the terminal and nowhere else. Reading the
  // configuration logs every setting, and starting the log rotates the log
  // file - of a Hermes that is very likely running while this is asked.
  const bool prelogin_command = std::any_of(argv + 1, argv + argc, [](const char *argument) {
    return argument == "--prelogin"sv;
  });
  if (prelogin_command) {
    boost::log::core::get()->set_logging_enabled(false);
  }

  // parse config file
  if (config::parse(argc, argv)) {
    return 0;
  }

  if (prelogin_command && config::sunshine.cmd.name == "prelogin"sv) {
    return platf::prelogin::command(argv[0], config::sunshine.cmd.argc, config::sunshine.cmd.argv);
  }

  auto log_deinit_guard = logging::init(config::sunshine.min_log_level, config::sunshine.log_file);
  if (!log_deinit_guard) {
    BOOST_LOG(error) << "Logging failed to initialize"sv;
  }

  // logging can begin at this point
  // if anything is logged prior to this point, it will appear in stdout, but not in the log viewer in the UI
  // the version should be printed to the log before anything else
  BOOST_LOG(info) << PROJECT_NAME << " version: " << PROJECT_VERSION << " commit: " << PROJECT_VERSION_COMMIT;

  // Log publisher metadata
  log_publisher_data();

  // Log modified_config_settings
  for (auto &[name, val] : config::modified_config_settings) {
    BOOST_LOG(info) << "config: '"sv << name << "' = "sv << val;
  }
  config::modified_config_settings.clear();

  if (!config::sunshine.cmd.name.empty()) {
    auto fn = cmd_to_func.find(config::sunshine.cmd.name);
    if (fn == std::end(cmd_to_func)) {
      BOOST_LOG(fatal) << "Unknown command: "sv << config::sunshine.cmd.name;

      BOOST_LOG(info) << "Possible commands:"sv;
      for (auto &[key, _] : cmd_to_func) {
        BOOST_LOG(info) << '\t' << key;
      }

      return 7;
    }

    return fn->second(argv[0], config::sunshine.cmd.argc, config::sunshine.cmd.argv);
  }

  // Adding guard here first as it also performs recovery after crash,
  // otherwise people could theoretically end up without display output.
  // It also should be destroyed before forced shutdown to expedite the cleanup.
  auto display_device_deinit_guard = display_device::init(platf::appdata() / "display_device.state", config::video);
  if (!display_device_deinit_guard) {
    BOOST_LOG(error) << "Display device session failed to initialize"sv;
  }


  task_pool.start(1);

  // Create signal handler after logging has been initialized
  auto shutdown_event = mail::man->event<bool>(mail::shutdown);
  on_signal(SIGINT, [&force_shutdown, &display_device_deinit_guard, shutdown_event]() {
    BOOST_LOG(info) << "Interrupt handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Hermes is still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };

    proc::proc.terminate();

    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    shutdown_event->raise(true);
    display_device_deinit_guard = nullptr;
  });

  on_signal(SIGTERM, [&force_shutdown, &display_device_deinit_guard, shutdown_event]() {
    BOOST_LOG(info) << "Terminate handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Hermes is still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    shutdown_event->raise(true);
    display_device_deinit_guard = nullptr;
  });


  proc::refresh(config::stream.file_apps);

  // If any of the following fail, we log an error and continue event though sunshine will not function correctly.
  // This allows access to the UI to fix configuration problems or view the logs.

  platf::session_attach::drop_stale_display_environment();

  auto platf_deinit_guard = platf::init();
  if (!platf_deinit_guard) {
    BOOST_LOG(error) << "Platform failed to initialize"sv;
  }

  auto proc_deinit_guard = proc::init();
  if (!proc_deinit_guard) {
    BOOST_LOG(error) << "Proc failed to initialize"sv;
  }

  // Initialize virtual display driver early so clients know it's supported
  proc::initVDisplayDriver();

  // Say what this session can and cannot do before anything depends on it. A
  // virtual-display feature that is unavailable here is almost always a
  // configuration a user can fix, and the alternative to saying so at startup
  // is that they find out from a black stream.
  VDISPLAY::logSessionAssessment();
  platf::prelogin::log_report();

  reed_solomon_init();
  auto input_deinit_guard = input::init();

  if (input::probe_gamepads()) {
    BOOST_LOG(warning) << "No gamepad input is available"sv;
  }

  // A host with no monitor has nothing to probe until a virtual display stands
  // in, so go straight to one. Probing first would fail and log the fatal "no
  // display" error - which the Web UI shows as a startup failure - before the
  // fallback below had even run.
  const bool display_present = video::display_present();
  // Started before anyone logged in, with no monitor, the only thing on screen
  // is the login greeter. SDDM's greeter quits for good when its last output
  // goes away, so the probe display's removal would leave nothing for the
  // first client to log in on. Leave the greeter alone: the first session
  // probes on the display it brings up.
  const bool defer_probe = !display_present && !VDISPLAY::windowSystemAttached();
  if (defer_probe) {
    BOOST_LOG(info) << "No monitor is lit and no graphical session is attached; "
                       "probing encoders when the first session brings up its virtual display."sv;
  }
  if (!defer_probe && (!display_present || video::probe_encoders())) {
    // No screen could be opened - typically because every monitor is asleep.
    // A temporary virtual output would be a monitor plugged in and unplugged,
    // which wakes every display the compositor drives, at every start of
    // Hermes. An encoder does not depend on a screen, so validate it on a
    // display with nothing behind it instead.
    BOOST_LOG(info) << "No screen to probe encoders on; probing them without one."sv;
    if (video::probe_encoders_without_display()) {
      BOOST_LOG(error) << "Video failed to find working encoder: allow probing but failed"sv;
    }
  }

  if (http::init()) {
    BOOST_LOG(fatal) << "HTTP interface failed to initialize"sv;


    return -1;
  }

  std::unique_ptr<platf::deinit_t> mDNS;
  auto sync_mDNS = std::async(std::launch::async, [&mDNS]() {
    if (config::sunshine.enable_discovery) {
      mDNS = platf::publish::start();
    }
  });

  std::unique_ptr<platf::deinit_t> upnp_unmap;
  auto sync_upnp = std::async(std::launch::async, [&upnp_unmap]() {
    upnp_unmap = upnp::start();
  });

  // FIXME: Temporary workaround: Simple-Web_server needs to be updated or replaced
  if (shutdown_event->peek()) {
    return lifetime::desired_exit_code;
  }

  std::thread httpThread {nvhttp::start};
  std::thread configThread {confighttp::start};
  std::thread rtspThread {rtsp_stream::start};

  // Started at boot, before anyone logged in, Hermes joins the graphical
  // session once there is one and no client is in the way.
  auto session_attach_guard = platf::session_attach::start();


  if (tray_is_enabled && config::sunshine.system_tray) {
    BOOST_LOG(info) << "Starting system tray"sv;
    system_tray::init_tray();
  }

  mainThreadLoop(shutdown_event);

  // Wait for shutdown, this is not necessary when we're using the main event loop
  shutdown_event->view();

  httpThread.join();
  configThread.join();
  rtspThread.join();

  task_pool.stop();
  task_pool.join();


  return lifetime::desired_exit_code;
}
