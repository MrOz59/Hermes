/**
 * @file src/platform/linux/misc.cpp
 * @brief Miscellaneous definitions for Linux.
 */

// Required for in6_pktinfo with glibc headers
#ifndef _GNU_SOURCE
  #define _GNU_SOURCE 1
#endif

// standard includes
#include <fstream>
#include <iostream>
#include <cstdio>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <thread>
#include <string_view>
#include <utility>

// platform includes
#include <arpa/inet.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <netinet/udp.h>
#include <pwd.h>

// lib includes
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/process/v1/group.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/env.hpp>
#include <boost/process/v1/environment.hpp>
#include <boost/process/v1/group.hpp>
#include <boost/process/v1/handles.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/search_path.hpp>
#include <boost/process/v1/start_dir.hpp>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xf86drm.h>

// local includes
#include "clipboard_process.h"
#include "graphics.h"
#include "misc.h"
#include "process_group.h"
#include "service_lifecycle.h"
#include "session_attach.h"
#include "src/config.h"
#include "src/entry_handler.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "vaapi.h"
#include "virtual_display.h"

#ifdef HERMES_BUILD_KWIN_TRANSPORT
  #include "kwin_display.h"
#endif

#include <linux/rtnetlink.h>

#ifdef HERMES_BUILD_SDBUS
  #include <poll.h>
  #include <sys/eventfd.h>
  #include <systemd/sd-bus.h>
#endif

#ifdef __GNUC__
  #define HERMES_GNUC_EXTENSION __extension__
#else
  #define HERMES_GNUC_EXTENSION
#endif

using namespace std::literals;
namespace fs = std::filesystem;
namespace bp = boost::process::v1;

window_system_e window_system;

namespace dyn {
  void *handle(const std::vector<const char *> &libs) {
    void *handle;

    for (auto lib : libs) {
      handle = dlopen(lib, RTLD_LAZY | RTLD_LOCAL);
      if (handle) {
        return handle;
      }
    }

    std::stringstream ss;
    ss << "Couldn't find any of the following libraries: ["sv << libs.front();
    std::for_each(std::begin(libs) + 1, std::end(libs), [&](auto lib) {
      ss << ", "sv << lib;
    });

    ss << ']';

    BOOST_LOG(error) << ss.str();

    return nullptr;
  }

  int load(void *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict) {
    int err = 0;
    for (auto &func : funcs) {
      TUPLE_2D_REF(fn, name, func);

      *fn = HERMES_GNUC_EXTENSION(apiproc) dlsym(handle, name);

      if (!*fn && strict) {
        BOOST_LOG(error) << "Couldn't find function: "sv << name;

        err = -1;
      }
    }

    return err;
  }
}  // namespace dyn

namespace platf {

  int open_drm_node(const char *path, int flags) {
    const int fd = ::open(path, flags | O_CLOEXEC);
    if (fd >= 0) {
      // EINVAL when this fd is not master, and render nodes refuse the
      // ioctl: either way there is nothing to give back.
      const int saved_errno = errno;
      (void) drmDropMaster(fd);
      errno = saved_errno;
    }
    return fd;
  }

#ifdef HERMES_BUILD_SDBUS
  int open_user_bus(sd_bus **result) {
    if (!result) {
      return -EINVAL;
    }
    *result = nullptr;

    const int discovered = sd_bus_open_user(result);
    if (discovered >= 0) {
      return discovered;
    }
    if (*result) {
      sd_bus_unref(*result);
      *result = nullptr;
    }
    if (discovered != -ENOMEDIUM) {
      return discovered;
    }

    sd_bus *bus = nullptr;
    int status = sd_bus_new(&bus);
    if (status < 0) {
      return status;
    }
    const auto fail = [&bus](int error) {
      sd_bus_unref(bus);
      return error;
    };

    const auto address = VDISPLAY::systemdUserBusAddress(static_cast<uint32_t>(::getuid()));
    status = sd_bus_set_address(bus, address.c_str());
    if (status < 0) {
      return fail(status);
    }
    status = sd_bus_set_bus_client(bus, 1);
    if (status < 0) {
      return fail(status);
    }
    status = sd_bus_start(bus);
    if (status < 0) {
      return fail(status);
    }

    *result = bus;
    return 0;
  }
#endif

  bool touch_binds_to_output() {
    // KWin picks an output for every touchscreen and tablet (its own guess
    // when nothing is configured, the streamed output once Hermes binds it),
    // and Mutter does the same through MetaInputMapper. wlroots compositors
    // leave an unmapped device spanning the whole layout, and X11 has no
    // per-output mapping at all, so there the desktop is the surface.
    const auto compositor = VDISPLAY::sessionCompositor();
    return compositor == VDISPLAY::compositor_e::kwin || compositor == VDISPLAY::compositor_e::mutter;
  }

  using ifaddr_t = util::safe_ptr<ifaddrs, freeifaddrs>;

  ifaddr_t get_ifaddrs() {
    ifaddrs *p {nullptr};

    getifaddrs(&p);

    return ifaddr_t {p};
  }

  namespace {
    // Hermes used to store its configuration in ~/.config/sunshine, the very directory
    // Apollo and Sunshine use. All three then shared one credentials file, one apps list
    // and one paired-client list, so installing Hermes next to either of them silently
    // took over their setup — and made a reinstall look like it had kept the old
    // password. Hermes now keeps its own directory.
    constexpr auto CONFIG_DIR_NAME = "hermes"sv;
    constexpr auto SHARED_CONFIG_DIR_NAME = "sunshine"sv;

    // Files that are renamed along with the directory. Keep in sync with the defaults in
    // src/config.cpp.
    constexpr std::pair<std::string_view, std::string_view> RENAMED_CONFIG_FILES[] {
      {"sunshine.conf"sv, "hermes.conf"sv},
      {"sunshine_state.json"sv, "hermes_state.json"sv},
    };

    /**
     * @brief Give any Sunshine-named file in the Hermes config directory its Hermes name.
     * @details Idempotent, so it also fixes up a directory that was copied over by hand.
     *          This runs before Boost logging exists, so it cannot log through it.
     * @param config_path The Hermes config directory.
     */
    void rename_shared_config_files(const fs::path &config_path) {
      for (const auto &[shared_name, name] : RENAMED_CONFIG_FILES) {
        std::error_code ec;
        const fs::path shared_file = config_path / shared_name;
        const fs::path file = config_path / name;
        if (fs::exists(shared_file, ec) && !fs::exists(file, ec)) {
          fs::rename(shared_file, file, ec);
          if (ec) {
            std::cerr << "Failed to rename "sv << shared_file << " to "sv << file << ": "sv << ec.message() << std::endl;
          }
        }
      }
    }

    /**
     * @brief Seed a first-run Hermes config directory from the shared Sunshine one.
     * @details The source is copied, never moved: Apollo or Sunshine may still be running
     *          from it. Existing Hermes installs therefore carry their apps, paired
     *          clients and credentials across without disturbing their neighbours.
     * @param config_path The Hermes config directory.
     * @param shared_path The Sunshine config directory to seed from.
     */
    void seed_from_shared_config(const fs::path &config_path, const fs::path &shared_path) {
      std::error_code ec;
      if (!fs::is_directory(shared_path, ec)) {
        return;
      }
      // An empty directory still counts as a first run. The AppImage's AppRun pre-creates
      // the config directory before launching, and users creating it by hand is common
      // enough that treating it as "already migrated" would silently skip the seeding.
      if (fs::is_directory(config_path, ec) && !fs::is_empty(config_path, ec)) {
        return;
      }

      std::cout << "Seeding "sv << config_path << " from "sv << shared_path << std::endl;
      fs::create_directories(config_path, ec);
      if (!ec) {
        fs::copy(shared_path, config_path, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
      }
      if (ec) {
        std::cerr << "Could not seed the Hermes config directory: "sv << ec.message() << std::endl;
        std::cerr << "Hermes will start with a fresh configuration in "sv << config_path << std::endl;
        return;
      }

      // Logs are the one thing not worth inheriting: a log that starts mid-way through
      // another host's session is actively misleading when diagnosing Hermes.
      for (const auto &name : {"sunshine.log"sv, "sunshine.log.backup"sv}) {
        fs::remove(config_path / name, ec);
      }
      ec.clear();

      std::cout << shared_path << " was left untouched; Apollo and Sunshine keep using it."sv << std::endl;
    }
  }  // namespace

  /**
   * @brief Performs migration if necessary, then returns the appdata directory.
   * @details This is used for the log directory, so it cannot invoke Boost logging!
   * @return The path of the appdata directory that should be used.
   */
  fs::path appdata() {
    static std::once_flag migration_flag;
    static fs::path config_path;

    // Ensure migration is only attempted once
    std::call_once(migration_flag, []() {
      bool found = false;
      bool migrate_config = true;
      const char *dir;
      const char *homedir;
      const char *migrate_envvar;

      // Get the home directory
      if ((homedir = getenv("HOME")) == nullptr || strlen(homedir) == 0) {
        // If HOME is empty or not set, use the current user's home directory
        homedir = getpwuid(geteuid())->pw_dir;
      }

      // May be set if running under a systemd service with the ConfigurationDirectory= option set.
      if ((dir = getenv("CONFIGURATION_DIRECTORY")) != nullptr && strlen(dir) > 0) {
        found = true;
        config_path = fs::path(dir) / CONFIG_DIR_NAME;
      }
      // Otherwise, follow the XDG base directory specification:
      // https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
      if (!found && (dir = getenv("XDG_CONFIG_HOME")) != nullptr && strlen(dir) > 0) {
        found = true;
        config_path = fs::path(dir) / CONFIG_DIR_NAME;
      }
      // As a last resort, use the home directory
      if (!found) {
        migrate_config = false;
        config_path = fs::path(homedir) / ".config" / CONFIG_DIR_NAME;
      }

      // migrate from the old config location if necessary
      migrate_envvar = getenv("SUNSHINE_MIGRATE_CONFIG");
      if (migrate_config && found && migrate_envvar && strcmp(migrate_envvar, "1") == 0) {
        std::error_code ec;
        fs::path old_config_path = fs::path(homedir) / ".config" / CONFIG_DIR_NAME;
        if (old_config_path != config_path && fs::exists(old_config_path, ec)) {
          if (!fs::exists(config_path, ec)) {
            std::cout << "Migrating config from "sv << old_config_path << " to "sv << config_path << std::endl;
            if (!ec) {
              // Create the new directory tree if it doesn't already exist
              fs::create_directories(config_path, ec);
            }
            if (!ec) {
              // Copy the old directory into the new location
              // NB: We use a copy instead of a move so that cross-volume migrations work
              fs::copy(old_config_path, config_path, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
            }
            if (!ec) {
              // If the copy was successful, delete the original directory
              fs::remove_all(old_config_path, ec);
              if (ec) {
                std::cerr << "Failed to clean up old config directory: " << ec.message() << std::endl;

                // This is not fatal. Next time we start, we'll warn the user to delete the old one.
                ec.clear();
              }
            }
            if (ec) {
              std::cerr << "Migration failed: " << ec.message() << std::endl;
              config_path = old_config_path;
            }
          } else {
            // We cannot use Boost logging because it hasn't been initialized yet!
            std::cerr << "Config exists in both "sv << old_config_path << " and "sv << config_path << ". Using "sv << config_path << " for config" << std::endl;
            std::cerr << "It is recommended to remove "sv << old_config_path << std::endl;
          }
        }
      }

      // Carry an install that predates the rename over from the shared Sunshine directory.
      seed_from_shared_config(config_path, config_path.parent_path() / SHARED_CONFIG_DIR_NAME);
      rename_shared_config_files(config_path);
    });

    return config_path;
  }

  std::string from_sockaddr(const sockaddr *const ip_addr) {
    char data[INET6_ADDRSTRLEN] = {};

    auto family = ip_addr->sa_family;
    if (family == AF_INET6) {
      inet_ntop(AF_INET6, &((sockaddr_in6 *) ip_addr)->sin6_addr, data, INET6_ADDRSTRLEN);
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((sockaddr_in *) ip_addr)->sin_addr, data, INET_ADDRSTRLEN);
    }

    return std::string {data};
  }

  std::pair<std::uint16_t, std::string> from_sockaddr_ex(const sockaddr *const ip_addr) {
    char data[INET6_ADDRSTRLEN] = {};

    auto family = ip_addr->sa_family;
    std::uint16_t port = 0;
    if (family == AF_INET6) {
      inet_ntop(AF_INET6, &((sockaddr_in6 *) ip_addr)->sin6_addr, data, INET6_ADDRSTRLEN);
      port = ((sockaddr_in6 *) ip_addr)->sin6_port;
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((sockaddr_in *) ip_addr)->sin_addr, data, INET_ADDRSTRLEN);
      port = ((sockaddr_in *) ip_addr)->sin_port;
    }

    return {port, std::string {data}};
  }

  std::string get_mac_address(const std::string_view &address) {
    auto ifaddrs = get_ifaddrs();
    for (auto pos = ifaddrs.get(); pos != nullptr; pos = pos->ifa_next) {
      if (pos->ifa_addr && address == from_sockaddr(pos->ifa_addr)) {
        std::ifstream mac_file("/sys/class/net/"s + pos->ifa_name + "/address");
        if (mac_file.good()) {
          std::string mac_address;
          std::getline(mac_file, mac_address);
          return mac_address;
        }
      }
    }

    BOOST_LOG(warning) << "Unable to find MAC address for "sv << address;
    return "00:00:00:00:00:00"s;
  }

std::string get_local_ip_for_gateway() {
  int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (fd < 0) {
    BOOST_LOG(warning) << "Socket creation failed: " << strerror(errno);
    return "";
  }

  char buffer[8192];
  struct nlmsghdr *nlMsg = (struct nlmsghdr *)buffer;
  struct rtmsg *rtMsg = (struct rtmsg *)NLMSG_DATA(nlMsg);
  struct rtattr *rtAttr;
  int len = 0;

  memset(nlMsg, 0, sizeof(struct nlmsghdr));
  nlMsg->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
  nlMsg->nlmsg_type = RTM_GETROUTE;
  nlMsg->nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
  nlMsg->nlmsg_seq = 1;
  nlMsg->nlmsg_pid = getpid();

  if (send(fd, nlMsg, nlMsg->nlmsg_len, 0) < 0) {
    BOOST_LOG(warning) << "Send message failed: " << strerror(errno);
    close(fd);
    return "";
  }

  std::string local_ip;
  bool found = false;

  while ((len = recv(fd, nlMsg, sizeof(buffer), 0)) > 0) {
    for (; NLMSG_OK(nlMsg, len); nlMsg = NLMSG_NEXT(nlMsg, len)) {
      if (nlMsg->nlmsg_type == NLMSG_DONE) {
        found = true;
        break;
      }

      rtMsg = (struct rtmsg *)NLMSG_DATA(nlMsg);
      if (rtMsg->rtm_family != AF_INET || rtMsg->rtm_table != RT_TABLE_MAIN)
        continue;

      rtAttr = (struct rtattr *)RTM_RTA(rtMsg);
      int rtLen = RTM_PAYLOAD(nlMsg);

      in_addr gateway;
      in_addr local;
      memset(&gateway, 0, sizeof(gateway));
      memset(&local, 0, sizeof(local));

      for (; RTA_OK(rtAttr, rtLen); rtAttr = RTA_NEXT(rtAttr, rtLen)) {
        switch(rtAttr->rta_type) {
          case RTA_GATEWAY:
            gateway.s_addr = *reinterpret_cast<uint32_t *>(RTA_DATA(rtAttr));
            break;
          case RTA_PREFSRC:
            local.s_addr = *reinterpret_cast<uint32_t *>(RTA_DATA(rtAttr));
            break;
          default:
            break;
        }
      }

      if (gateway.s_addr != 0 && local.s_addr != 0) {
        local_ip = inet_ntoa(local);
        found = true;
        break;
      }
    }

    if (found) break;
  }

  close(fd);

  if (local_ip.empty()) {
    BOOST_LOG(warning) << "No associated IP address found for the default gateway";
  }

  return local_ip;
}

  bp::child run_command(bool elevated, bool interactive, const std::string &cmd, boost::filesystem::path &working_dir, const bp::environment &env, FILE *file, std::error_code &ec, bp::group *group) {
    // clang-format off
    if (!group) {
      if (!file) {
        return bp::child(cmd, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > bp::null, bp::std_err > bp::null, bp::limit_handles, ec);
      }
      else {
        return bp::child(cmd, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > file, bp::std_err > file, bp::limit_handles, ec);
      }
    }
    else {
      if (!file) {
        return bp::child(cmd, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > bp::null, bp::std_err > bp::null, bp::limit_handles, ec, *group);
      }
      else {
        return bp::child(cmd, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > file, bp::std_err > file, bp::limit_handles, ec, *group);
      }
    }
    // clang-format on
  }

  /**
   * @brief Open a url in the default web browser.
   * @param url The url to open.
   */
  void open_url(const std::string &url) {
    // set working dir to user home directory
    auto working_dir = boost::filesystem::path(std::getenv("HOME"));
    std::string cmd = R"(xdg-open ")" + url + R"(")";

    boost::process::v1::environment _env = boost::this_process::environment();
    std::error_code ec;
    auto child = run_command(false, false, cmd, working_dir, _env, nullptr, ec, nullptr);
    if (ec) {
      BOOST_LOG(warning) << "Couldn't open url ["sv << url << "]: System: "sv << ec.message();
    } else {
      BOOST_LOG(info) << "Opened url ["sv << url << "]"sv;
      child.detach();
    }
  }

  void adjust_thread_priority(thread_priority_e priority) {
    // Unimplemented
  }

  namespace {
#ifdef HERMES_BUILD_SDBUS
    constexpr auto screen_saver_service = "org.freedesktop.ScreenSaver"sv;
    constexpr auto screen_saver_path = "/org/freedesktop/ScreenSaver"sv;
    constexpr auto screen_saver_interface = "org.freedesktop.ScreenSaver"sv;

    // All sd-bus operations belong to a worker thread. The stream hooks only
    // publish the desired state and wake the worker, so a slow or wedged
    // screensaver cannot block session transitions, which are serialized by
    // stream::session_activity_counter_t while these hooks run.
    struct screen_lock_bus_t {
      using clock = std::chrono::steady_clock;

      sd_bus *bus {nullptr};
      sd_bus_slot *owner_watch {nullptr};
      std::uint32_t cookie {0};
      // Unique bus name of the service instance which issued the cookie.
      // NameOwnerChanged signals from *older* instances must not invalidate
      // an inhibition already re-established on a replacement instance.
      std::string cookie_owner;
      bool inhibited {false};
      bool failure_logged {false};
      // Earliest time to connect or call Inhibit again. clock::time_point::max()
      // means "wait until org.freedesktop.ScreenSaver gets an owner".
      clock::time_point retry_at {};
      // Next back-off step; zero restarts the back-off.
      clock::duration retry_delay {};
    };

    std::string bus_error_message(const sd_bus_error &error, int status) {
      if (error.message) {
        return error.message;
      }
      return std::strerror(-status);
    }

    bool screen_saver_has_no_owner(const sd_bus_error &error) {
      return sd_bus_error_has_name(&error, SD_BUS_ERROR_SERVICE_UNKNOWN) ||
             sd_bus_error_has_name(&error, SD_BUS_ERROR_NAME_HAS_NO_OWNER);
    }

    class screen_lock_inhibitor_t {
    public:
      screen_lock_inhibitor_t():
          wake_fd_ {::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)},
          wake_errno_ {wake_fd_ < 0 ? errno : 0} {
      }

      screen_lock_inhibitor_t(const screen_lock_inhibitor_t &) = delete;
      screen_lock_inhibitor_t &operator=(const screen_lock_inhibitor_t &) = delete;

      ~screen_lock_inhibitor_t() {
        // The eventfd is level-triggered, so this wakeup cannot be lost even if
        // the worker is between checking the stop token and blocking in poll().
        // Join explicitly so wake_fd_ outlives the worker.
        worker_.request_stop();
        wake();
        if (worker_.joinable()) {
          worker_.join();
        }
        if (wake_fd_ >= 0) {
          ::close(wake_fd_);
        }
      }

      void set_streaming(bool streaming) {
        {
          std::lock_guard lock {mutex_};
          streaming_ = streaming;
          if (streaming && !worker_.joinable()) {
            if (wake_fd_ < 0) {
              if (!wake_failure_logged_) {
                BOOST_LOG(warning) << "Could not inhibit screen locking: eventfd() failed: "sv
                                   << std::strerror(wake_errno_);
                wake_failure_logged_ = true;
              }
              return;
            }
            worker_ = std::jthread {[this](std::stop_token stop) {
              run(stop);
            }};
          }
        }
        wake();
      }

    private:
      using clock = screen_lock_bus_t::clock;

      static constexpr auto initial_retry_delay = std::chrono::seconds {2};
      static constexpr auto max_retry_delay = std::chrono::seconds {30};
      static constexpr auto never = clock::time_point::max();

      std::mutex mutex_;
      bool streaming_ {false};
      bool wake_failure_logged_ {false};
      const int wake_fd_;
      const int wake_errno_;
      std::jthread worker_;

      void wake() {
        if (wake_fd_ < 0) {
          return;
        }
        const std::uint64_t one = 1;
        while (::write(wake_fd_, &one, sizeof(one)) < 0 && errno == EINTR) {
        }
      }

      void consume_wakeups() {
        std::uint64_t count = 0;
        while (::read(wake_fd_, &count, sizeof(count)) < 0 && errno == EINTR) {
        }
      }

      bool wants_inhibition() {
        std::lock_guard lock {mutex_};
        return streaming_;
      }

      /**
       * @brief Sleep until set_streaming()/shutdown, D-Bus traffic or @p deadline.
       * @return false if the bus connection can no longer be polled.
       */
      bool wait_for_event(const screen_lock_bus_t &state, clock::time_point deadline) {
        std::array<pollfd, 2> fds {};
        nfds_t count = 0;
        fds[count++] = pollfd {wake_fd_, POLLIN, 0};

        if (state.bus) {
          const int fd = sd_bus_get_fd(state.bus);
          const int events = sd_bus_get_events(state.bus);
          std::uint64_t bus_deadline_usec = UINT64_MAX;
          const int has_bus_deadline = sd_bus_get_timeout(state.bus, &bus_deadline_usec);
          if (fd < 0 || events < 0 || has_bus_deadline < 0) {
            return false;
          }
          fds[count++] = pollfd {fd, static_cast<short>(events), 0};

          // sd-bus reports an absolute CLOCK_MONOTONIC deadline. It is zero
          // when messages are already queued, so poll() returns immediately.
          if (has_bus_deadline > 0 && bus_deadline_usec != UINT64_MAX) {
            timespec now {};
            ::clock_gettime(CLOCK_MONOTONIC, &now);
            const auto now_usec = static_cast<std::uint64_t>(now.tv_sec) * 1'000'000u +
                                  static_cast<std::uint64_t>(now.tv_nsec) / 1'000u;
            const auto remaining = std::chrono::microseconds {
              bus_deadline_usec > now_usec ? bus_deadline_usec - now_usec : 0
            };
            deadline = std::min(deadline, clock::now() + remaining);
          }
        }

        int timeout_ms = -1;
        if (deadline != never) {
          const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now());
          timeout_ms = static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(remaining.count(), 0, INT_MAX));
        }

        if (::poll(fds.data(), count, timeout_ms) < 0 && errno != EINTR) {
          // Not expected for valid descriptors; avoid spinning if it happens.
          std::this_thread::sleep_for(std::chrono::milliseconds {100});
          return !state.bus;
        }
        return true;
      }

      static void warn_once(screen_lock_bus_t &state, const std::string &message) {
        if (!state.failure_logged) {
          BOOST_LOG(warning) << message;
          state.failure_logged = true;
        }
      }

      static void retry_now(screen_lock_bus_t &state) {
        state.retry_at = clock::now();
        state.retry_delay = {};
      }

      static void schedule_retry(screen_lock_bus_t &state) {
        const clock::duration delay = state.retry_delay > clock::duration::zero() ? state.retry_delay : clock::duration {initial_retry_delay};
        state.retry_at = clock::now() + delay;
        state.retry_delay = std::min<clock::duration>(delay * 2, max_retry_delay);
      }

      static int on_owner_changed(sd_bus_message *message, void *userdata, sd_bus_error *) {
        auto *state = static_cast<screen_lock_bus_t *>(userdata);
        const char *name = nullptr;
        const char *old_owner = nullptr;
        const char *new_owner = nullptr;
        if (sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner) < 0) {
          return 0;
        }
        if (std::strcmp(name, screen_saver_service.data()) != 0 ||
            std::strcmp(old_owner, new_owner) == 0) {
          return 0;
        }

        // A late NameOwnerChanged for the PREVIOUS screensaver may be queued
        // after an Inhibit reply from the NEW screensaver. Only invalidate
        // cookies issued by the owner which actually disappeared.
        if (state->inhibited && state->cookie_owner == old_owner) {
          state->inhibited = false;
          state->cookie = 0;
          state->cookie_owner.clear();
          if (new_owner[0] != '\0') {
            retry_now(*state);
          } else {
            // Give a restarting screensaver a moment; its NameOwnerChanged
            // brings the retry forward as soon as it is back.
            state->retry_delay = {};
            schedule_retry(*state);
          }
        } else if (!state->inhibited && new_owner[0] != '\0') {
          // A service just appeared after a failed attempt: retry promptly.
          retry_now(*state);
        }
        return 0;
      }

      static bool dispatch(screen_lock_bus_t &state) {
        int processed = 0;
        while ((processed = sd_bus_process(state.bus, nullptr)) > 0) {
        }
        return processed >= 0;
      }

      static void close_bus(screen_lock_bus_t &state, bool release_inhibition) {
        if (!state.bus) {
          return;
        }
        if (release_inhibition && state.inhibited) {
          // If the issuing screensaver is already gone, its queued
          // NameOwnerChanged clears the cookie and there is nothing to release.
          dispatch(state);
        }
        if (release_inhibition && state.inhibited) {
          sd_bus_error error = SD_BUS_ERROR_NULL;
          // Target the instance which issued this cookie, not the well-known
          // name that may now belong to a different screensaver process.
          const int status = sd_bus_call_method(
            state.bus,
            state.cookie_owner.c_str(),
            screen_saver_path.data(),
            screen_saver_interface.data(),
            "UnInhibit",
            &error,
            nullptr,
            "u",
            state.cookie
          );
          if (status >= 0) {
            BOOST_LOG(info) << "Released screen-lock inhibition."sv;
          } else if (screen_saver_has_no_owner(error)) {
            BOOST_LOG(debug) << "The screensaver holding our inhibition is gone; nothing to release."sv;
          } else {
            BOOST_LOG(warning) << "Could not release screen-lock inhibition: "sv
                               << bus_error_message(error, status);
          }
          sd_bus_error_free(&error);
        }
        // Closing the caller's bus connection also releases the inhibition
        // if UnInhibit failed.
        sd_bus_slot_unref(state.owner_watch);
        state.owner_watch = nullptr;
        sd_bus_unref(state.bus);
        state.bus = nullptr;
        state.inhibited = false;
        state.cookie = 0;
        state.cookie_owner.clear();
        state.retry_at = {};
      }

      static bool connect_bus(screen_lock_bus_t &state) {
        sd_bus *bus = nullptr;
        const int opened = open_user_bus(&bus);
        if (opened < 0 || !bus) {
          warn_once(state, "Could not connect to the D-Bus session bus to inhibit screen locking.");
          return false;
        }

        state.bus = bus;
        sd_bus_set_method_call_timeout(bus, 2'000'000);

        // Watch the owner, not merely the connection. Desktop components can
        // restart without disconnecting Hermes from the user bus.
        constexpr auto match =
          "type='signal',sender='org.freedesktop.DBus',"
          "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
          "arg0='org.freedesktop.ScreenSaver'";
        const int matched = sd_bus_add_match(bus, &state.owner_watch, match, on_owner_changed, &state);
        if (matched < 0) {
          warn_once(state, "Could not watch org.freedesktop.ScreenSaver ownership changes.");
          close_bus(state, false);
          return false;
        }

        state.retry_at = clock::now();
        return true;
      }

      static void acquire_inhibition(screen_lock_bus_t &state) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = nullptr;
        const int status = sd_bus_call_method(
          state.bus,
          screen_saver_service.data(),
          screen_saver_path.data(),
          screen_saver_interface.data(),
          "Inhibit",
          &error,
          &reply,
          "ss",
          "Hermes",
          "Streaming a client"
        );

        std::uint32_t cookie = 0;
        const int read_status = status < 0 ? status : (reply ? sd_bus_message_read(reply, "u", &cookie) : -EIO);
        const char *reply_owner = read_status >= 0 ? sd_bus_message_get_sender(reply) : nullptr;
        if (read_status < 0) {
          if (status < 0) {
            warn_once(state, "Could not inhibit screen locking through org.freedesktop.ScreenSaver: " + bus_error_message(error, status));
          } else {
            warn_once(state, "The screensaver returned an unexpected Inhibit reply.");
          }
          if (status < 0 && screen_saver_has_no_owner(error)) {
            // Nothing to retry until a screensaver appears; NameOwnerChanged
            // reports that without polling the bus for the whole stream.
            state.retry_at = never;
          } else {
            schedule_retry(state);
          }
        } else if (!reply_owner || !*reply_owner) {
          warn_once(state, "The screensaver did not identify the owner of its inhibition cookie.");
          schedule_retry(state);
        } else {
          state.cookie = cookie;
          state.cookie_owner = reply_owner;
          state.inhibited = true;
          state.failure_logged = false;
          state.retry_delay = {};
          BOOST_LOG(info) << "Inhibited screen locking while a client is connected."sv;
        }

        sd_bus_error_free(&error);
        sd_bus_message_unref(reply);
      }

      void run(std::stop_token stop) {
        screen_lock_bus_t state;
        for (;;) {
          // Consume wakeups BEFORE reading the state they announce: a stop or
          // set_streaming() racing with this iteration leaves the eventfd
          // readable, so the next poll() returns instead of sleeping on it.
          consume_wakeups();
          if (stop.stop_requested()) {
            break;
          }

          if (!wants_inhibition()) {
            close_bus(state, true);
            state.retry_delay = {};
            wait_for_event(state, never);
            continue;
          }

          if (!state.bus) {
            if (clock::now() < state.retry_at) {
              wait_for_event(state, state.retry_at);
              continue;
            }
            if (!connect_bus(state)) {
              schedule_retry(state);
              continue;
            }
          }

          // Dispatch owner changes queued on the socket or by an earlier
          // synchronous call before deciding whether to (re)acquire.
          if (!dispatch(state)) {
            warn_once(state, "Lost the D-Bus session bus while inhibiting screen locking.");
            close_bus(state, false);
            schedule_retry(state);
            continue;
          }

          if (!state.inhibited && clock::now() >= state.retry_at) {
            acquire_inhibition(state);
            // Dispatch whatever arrived during the call before sleeping.
            continue;
          }

          if (!wait_for_event(state, state.inhibited ? never : state.retry_at)) {
            warn_once(state, "Lost the D-Bus session bus while inhibiting screen locking.");
            close_bus(state, false);
            schedule_retry(state);
          }
        }
        close_bus(state, true);
      }
    };

    screen_lock_inhibitor_t &screen_lock() {
      static screen_lock_inhibitor_t inhibitor;
      return inhibitor;
    }

    void inhibit_screen_lock() {
      screen_lock().set_streaming(true);
    }

    void uninhibit_screen_lock() {
      screen_lock().set_streaming(false);
    }
#else
    void inhibit_screen_lock() {
      static std::once_flag warned;
      std::call_once(warned, [] {
        BOOST_LOG(warning) << "inhibit_screen_lock is enabled, but this build has no libsystemd "
                              "support for org.freedesktop.ScreenSaver."sv;
      });
    }

    void uninhibit_screen_lock() {}
#endif
  }  // namespace

  void streaming_will_start() {
    if (config::sunshine.inhibit_screen_lock) {
      inhibit_screen_lock();
    }
  }

  void streaming_will_stop() {
    // Uninhibit even when the setting was just turned off, so the bus
    // connection cannot outlive the configuration that opened it.
    uninhibit_screen_lock();
  }

  void restart_on_exit() {
    char executable[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", executable, PATH_MAX - 1);
    if (len == -1) {
      BOOST_LOG(fatal) << "readlink() failed: "sv << errno;
      return;
    }
    executable[len] = '\0';

    // ASIO doesn't use O_CLOEXEC, so we have to close all fds ourselves
    int openmax = (int) sysconf(_SC_OPEN_MAX);
    for (int fd = STDERR_FILENO + 1; fd < openmax; fd++) {
      close(fd);
    }

    // Re-exec ourselves with the same arguments, and with the environment
    // Hermes was started with rather than the one it has changed since: what
    // a restart of the unit would give it. If a graphical session started
    // after Hermes did, this is also where Hermes joins it.
    const auto environment = session_attach::restart_environment();
    if (!environment.empty()) {
      std::vector<char *> envp;
      envp.reserve(environment.size() + 1);
      for (const auto &assignment : environment) {
        envp.push_back(const_cast<char *>(assignment.c_str()));
      }
      envp.push_back(nullptr);
      execve(executable, lifetime::get_argv(), envp.data());
    } else {
      execv(executable, lifetime::get_argv());
    }
    BOOST_LOG(fatal) << "execv() failed: "sv << errno;
  }

  void restart() {
    // Gracefully clean up and restart ourselves instead of exiting
    atexit(restart_on_exit);
    lifetime::exit_sunshine(0, true);
  }

  int set_env(const std::string &name, const std::string &value) {
    return setenv(name.c_str(), value.c_str(), 1);
  }

  int unset_env(const std::string &name) {
    return unsetenv(name.c_str());
  }

  bool request_process_group_exit(std::uintptr_t native_handle) {
    if (process_group::signal(native_handle, SIGTERM)) {
      BOOST_LOG(debug) << "Successfully sent SIGTERM to process group: "sv << native_handle;
      return true;
    } else {
      BOOST_LOG(warning) << "Unable to send SIGTERM to process group ["sv << native_handle << "]: "sv << errno;
      return false;
    }
  }

  bool process_group_running(std::uintptr_t native_handle) {
    return process_group::running(native_handle);
  }

  bool request_user_service_stop() {
    std::ifstream cgroups {"/proc/self/cgroup"};
    std::string entry;
    bool managed = false;
    while (std::getline(cgroups, entry)) {
      if (is_hermes_user_service_cgroup(entry)) {
        managed = true;
        break;
      }
    }
    if (!managed) {
      return false;
    }

    // --no-block only queues the stop job; waiting for the unit to stop here
    // would deadlock against our own exit. A deliberate stop also suppresses
    // Restart=on-failure if a remaining workload hits systemd's stop timeout.
    std::error_code ec;
    boost::filesystem::path working_dir {"/"};
    auto child = run_command(false, false, "systemctl --user --no-block stop hermes.service",
                             working_dir, boost::this_process::environment(), nullptr, ec, nullptr);
    if (!ec && child.valid()) {
      if (child.wait_for(2s, ec) && !ec && child.exit_code() == 0) {
        BOOST_LOG(info) << "Requested intentional stop of hermes.service from system tray";
        return true;
      }
      child.terminate(ec);
      child.detach();
    }
    BOOST_LOG(warning) << "Unable to request user-service stop; continuing normal shutdown";
    return false;
  }

  struct sockaddr_in to_sockaddr(boost::asio::ip::address_v4 address, uint16_t port) {
    struct sockaddr_in saddr_v4 = {};

    saddr_v4.sin_family = AF_INET;
    saddr_v4.sin_port = htons(port);

    auto addr_bytes = address.to_bytes();
    memcpy(&saddr_v4.sin_addr, addr_bytes.data(), sizeof(saddr_v4.sin_addr));

    return saddr_v4;
  }

  struct sockaddr_in6 to_sockaddr(boost::asio::ip::address_v6 address, uint16_t port) {
    struct sockaddr_in6 saddr_v6 = {};

    saddr_v6.sin6_family = AF_INET6;
    saddr_v6.sin6_port = htons(port);
    saddr_v6.sin6_scope_id = address.scope_id();

    auto addr_bytes = address.to_bytes();
    memcpy(&saddr_v6.sin6_addr, addr_bytes.data(), sizeof(saddr_v6.sin6_addr));

    return saddr_v6;
  }

  bool send_batch(batched_send_info_t &send_info) {
    auto sockfd = (int) send_info.native_socket;
    struct msghdr msg = {};

    // Convert the target address into a sockaddr
    struct sockaddr_in taddr_v4 = {};
    struct sockaddr_in6 taddr_v6 = {};
    if (send_info.target_address.is_v6()) {
      taddr_v6 = to_sockaddr(send_info.target_address.to_v6(), send_info.target_port);

      msg.msg_name = (struct sockaddr *) &taddr_v6;
      msg.msg_namelen = sizeof(taddr_v6);
    } else {
      taddr_v4 = to_sockaddr(send_info.target_address.to_v4(), send_info.target_port);

      msg.msg_name = (struct sockaddr *) &taddr_v4;
      msg.msg_namelen = sizeof(taddr_v4);
    }

    union {
      char buf[CMSG_SPACE(sizeof(uint16_t)) + std::max(CMSG_SPACE(sizeof(struct in_pktinfo)), CMSG_SPACE(sizeof(struct in6_pktinfo)))];
      struct cmsghdr alignment;
    } cmbuf = {};  // Must be zeroed for CMSG_NXTHDR()

    socklen_t cmbuflen = 0;

    msg.msg_control = cmbuf.buf;
    msg.msg_controllen = sizeof(cmbuf.buf);

    // The PKTINFO option will always be first, then we will conditionally
    // append the UDP_SEGMENT option next if applicable.
    auto pktinfo_cm = CMSG_FIRSTHDR(&msg);
    if (send_info.source_address.is_v6()) {
      struct in6_pktinfo pktInfo;

      struct sockaddr_in6 saddr_v6 = to_sockaddr(send_info.source_address.to_v6(), 0);
      pktInfo.ipi6_addr = saddr_v6.sin6_addr;
      pktInfo.ipi6_ifindex = 0;

      cmbuflen += CMSG_SPACE(sizeof(pktInfo));

      pktinfo_cm->cmsg_level = IPPROTO_IPV6;
      pktinfo_cm->cmsg_type = IPV6_PKTINFO;
      pktinfo_cm->cmsg_len = CMSG_LEN(sizeof(pktInfo));
      memcpy(CMSG_DATA(pktinfo_cm), &pktInfo, sizeof(pktInfo));
    } else {
      struct in_pktinfo pktInfo;

      struct sockaddr_in saddr_v4 = to_sockaddr(send_info.source_address.to_v4(), 0);
      pktInfo.ipi_spec_dst = saddr_v4.sin_addr;
      pktInfo.ipi_ifindex = 0;

      cmbuflen += CMSG_SPACE(sizeof(pktInfo));

      pktinfo_cm->cmsg_level = IPPROTO_IP;
      pktinfo_cm->cmsg_type = IP_PKTINFO;
      pktinfo_cm->cmsg_len = CMSG_LEN(sizeof(pktInfo));
      memcpy(CMSG_DATA(pktinfo_cm), &pktInfo, sizeof(pktInfo));
    }

    auto const max_iovs_per_msg = send_info.payload_buffers.size() + (send_info.headers ? 1 : 0);

#ifdef UDP_SEGMENT
    {
      // UDP GSO on Linux currently only supports sending 64K or 64 segments at a time
      size_t seg_index = 0;
      const size_t seg_max = 65536 / 1500;
      struct iovec iovs[(send_info.headers ? std::min(seg_max, send_info.block_count) : 1) * max_iovs_per_msg];
      auto msg_size = send_info.header_size + send_info.payload_size;
      while (seg_index < send_info.block_count) {
        int iovlen = 0;
        auto segs_in_batch = std::min(send_info.block_count - seg_index, seg_max);
        if (send_info.headers) {
          // Interleave iovs for headers and payloads
          for (auto i = 0; i < segs_in_batch; i++) {
            iovs[iovlen].iov_base = (void *) &send_info.headers[(send_info.block_offset + seg_index + i) * send_info.header_size];
            iovs[iovlen].iov_len = send_info.header_size;
            iovlen++;
            auto payload_desc = send_info.buffer_for_payload_offset((send_info.block_offset + seg_index + i) * send_info.payload_size);
            iovs[iovlen].iov_base = (void *) payload_desc.buffer;
            iovs[iovlen].iov_len = send_info.payload_size;
            iovlen++;
          }
        } else {
          // Translate buffer descriptors into iovs
          auto payload_offset = (send_info.block_offset + seg_index) * send_info.payload_size;
          auto payload_length = payload_offset + (segs_in_batch * send_info.payload_size);
          while (payload_offset < payload_length) {
            auto payload_desc = send_info.buffer_for_payload_offset(payload_offset);
            iovs[iovlen].iov_base = (void *) payload_desc.buffer;
            iovs[iovlen].iov_len = std::min(payload_desc.size, payload_length - payload_offset);
            payload_offset += iovs[iovlen].iov_len;
            iovlen++;
          }
        }

        msg.msg_iov = iovs;
        msg.msg_iovlen = iovlen;

        // We should not use GSO if the data is <= one full block size
        if (segs_in_batch > 1) {
          msg.msg_controllen = cmbuflen + CMSG_SPACE(sizeof(uint16_t));

          // Enable GSO to perform segmentation of our buffer for us
          auto cm = CMSG_NXTHDR(&msg, pktinfo_cm);
          cm->cmsg_level = SOL_UDP;
          cm->cmsg_type = UDP_SEGMENT;
          cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
          *((uint16_t *) CMSG_DATA(cm)) = msg_size;
        } else {
          msg.msg_controllen = cmbuflen;
        }

        // This will fail if GSO is not available, so we will fall back to non-GSO if
        // it's the first sendmsg() call. On subsequent calls, we will treat errors as
        // actual failures and return to the caller.
        auto bytes_sent = sendmsg(sockfd, &msg, 0);
        if (bytes_sent < 0) {
          // If there's no send buffer space, wait for some to be available
          if (errno == EAGAIN) {
            struct pollfd pfd;

            pfd.fd = sockfd;
            pfd.events = POLLOUT;

            if (poll(&pfd, 1, -1) != 1) {
              BOOST_LOG(warning) << "poll() failed: "sv << errno;
              break;
            }

            // Try to send again
            continue;
          }

          BOOST_LOG(verbose) << "sendmsg() failed: "sv << errno;
          break;
        }

        seg_index += bytes_sent / msg_size;
      }

      // If we sent something, return the status and don't fall back to the non-GSO path.
      if (seg_index != 0) {
        return seg_index >= send_info.block_count;
      }
    }
#endif

    {
      // If GSO is not supported, use sendmmsg() instead.
      struct mmsghdr msgs[send_info.block_count];
      struct iovec iovs[send_info.block_count * (send_info.headers ? 2 : 1)];
      int iov_idx = 0;
      for (size_t i = 0; i < send_info.block_count; i++) {
        msgs[i].msg_len = 0;
        msgs[i].msg_hdr.msg_iov = &iovs[iov_idx];
        msgs[i].msg_hdr.msg_iovlen = send_info.headers ? 2 : 1;

        if (send_info.headers) {
          iovs[iov_idx].iov_base = (void *) &send_info.headers[(send_info.block_offset + i) * send_info.header_size];
          iovs[iov_idx].iov_len = send_info.header_size;
          iov_idx++;
        }
        auto payload_desc = send_info.buffer_for_payload_offset((send_info.block_offset + i) * send_info.payload_size);
        iovs[iov_idx].iov_base = (void *) payload_desc.buffer;
        iovs[iov_idx].iov_len = send_info.payload_size;
        iov_idx++;

        msgs[i].msg_hdr.msg_name = msg.msg_name;
        msgs[i].msg_hdr.msg_namelen = msg.msg_namelen;
        msgs[i].msg_hdr.msg_control = cmbuf.buf;
        msgs[i].msg_hdr.msg_controllen = cmbuflen;
        msgs[i].msg_hdr.msg_flags = 0;
      }

      // Call sendmmsg() until all messages are sent
      size_t blocks_sent = 0;
      while (blocks_sent < send_info.block_count) {
        int msgs_sent = sendmmsg(sockfd, &msgs[blocks_sent], send_info.block_count - blocks_sent, 0);
        if (msgs_sent < 0) {
          // If there's no send buffer space, wait for some to be available
          if (errno == EAGAIN) {
            struct pollfd pfd;

            pfd.fd = sockfd;
            pfd.events = POLLOUT;

            if (poll(&pfd, 1, -1) != 1) {
              BOOST_LOG(warning) << "poll() failed: "sv << errno;
              break;
            }

            // Try to send again
            continue;
          }

          BOOST_LOG(warning) << "sendmmsg() failed: "sv << errno;
          return false;
        }

        blocks_sent += msgs_sent;
      }

      return true;
    }
  }

  bool send(send_info_t &send_info) {
    auto sockfd = (int) send_info.native_socket;
    struct msghdr msg = {};

    // Convert the target address into a sockaddr
    struct sockaddr_in taddr_v4 = {};
    struct sockaddr_in6 taddr_v6 = {};
    if (send_info.target_address.is_v6()) {
      taddr_v6 = to_sockaddr(send_info.target_address.to_v6(), send_info.target_port);

      msg.msg_name = (struct sockaddr *) &taddr_v6;
      msg.msg_namelen = sizeof(taddr_v6);
    } else {
      taddr_v4 = to_sockaddr(send_info.target_address.to_v4(), send_info.target_port);

      msg.msg_name = (struct sockaddr *) &taddr_v4;
      msg.msg_namelen = sizeof(taddr_v4);
    }

    union {
      char buf[std::max(CMSG_SPACE(sizeof(struct in_pktinfo)), CMSG_SPACE(sizeof(struct in6_pktinfo)))];
      struct cmsghdr alignment;
    } cmbuf;

    socklen_t cmbuflen = 0;

    msg.msg_control = cmbuf.buf;
    msg.msg_controllen = sizeof(cmbuf.buf);

    auto pktinfo_cm = CMSG_FIRSTHDR(&msg);
    if (send_info.source_address.is_v6()) {
      struct in6_pktinfo pktInfo;

      struct sockaddr_in6 saddr_v6 = to_sockaddr(send_info.source_address.to_v6(), 0);
      pktInfo.ipi6_addr = saddr_v6.sin6_addr;
      pktInfo.ipi6_ifindex = 0;

      cmbuflen += CMSG_SPACE(sizeof(pktInfo));

      pktinfo_cm->cmsg_level = IPPROTO_IPV6;
      pktinfo_cm->cmsg_type = IPV6_PKTINFO;
      pktinfo_cm->cmsg_len = CMSG_LEN(sizeof(pktInfo));
      memcpy(CMSG_DATA(pktinfo_cm), &pktInfo, sizeof(pktInfo));
    } else {
      struct in_pktinfo pktInfo;

      struct sockaddr_in saddr_v4 = to_sockaddr(send_info.source_address.to_v4(), 0);
      pktInfo.ipi_spec_dst = saddr_v4.sin_addr;
      pktInfo.ipi_ifindex = 0;

      cmbuflen += CMSG_SPACE(sizeof(pktInfo));

      pktinfo_cm->cmsg_level = IPPROTO_IP;
      pktinfo_cm->cmsg_type = IP_PKTINFO;
      pktinfo_cm->cmsg_len = CMSG_LEN(sizeof(pktInfo));
      memcpy(CMSG_DATA(pktinfo_cm), &pktInfo, sizeof(pktInfo));
    }

    struct iovec iovs[2];
    int iovlen = 0;
    if (send_info.header) {
      iovs[iovlen].iov_base = (void *) send_info.header;
      iovs[iovlen].iov_len = send_info.header_size;
      iovlen++;
    }
    iovs[iovlen].iov_base = (void *) send_info.payload;
    iovs[iovlen].iov_len = send_info.payload_size;
    iovlen++;

    msg.msg_iov = iovs;
    msg.msg_iovlen = iovlen;

    msg.msg_controllen = cmbuflen;

    auto bytes_sent = sendmsg(sockfd, &msg, 0);

    // If there's no send buffer space, wait for some to be available
    while (bytes_sent < 0 && errno == EAGAIN) {
      struct pollfd pfd;

      pfd.fd = sockfd;
      pfd.events = POLLOUT;

      if (poll(&pfd, 1, -1) != 1) {
        BOOST_LOG(warning) << "poll() failed: "sv << errno;
        break;
      }

      // Try to send again
      bytes_sent = sendmsg(sockfd, &msg, 0);
    }

    if (bytes_sent < 0) {
      BOOST_LOG(warning) << "sendmsg() failed: "sv << errno;
      return false;
    }

    return true;
  }

  // We can't track QoS state separately for each destination on this OS,
  // so we keep a ref count to only disable QoS options when all clients
  // are disconnected.
  static std::atomic<int> qos_ref_count = 0;

  class qos_t: public deinit_t {
  public:
    qos_t(int sockfd, std::vector<std::tuple<int, int, int>> options):
        sockfd(sockfd),
        options(options) {
      qos_ref_count++;
    }

    virtual ~qos_t() {
      if (--qos_ref_count == 0) {
        for (const auto &tuple : options) {
          auto reset_val = std::get<2>(tuple);
          if (setsockopt(sockfd, std::get<0>(tuple), std::get<1>(tuple), &reset_val, sizeof(reset_val)) < 0) {
            BOOST_LOG(warning) << "Failed to reset option: "sv << errno;
          }
        }
      }
    }

  private:
    int sockfd;
    std::vector<std::tuple<int, int, int>> options;
  };

  /**
   * @brief Enables QoS on the given socket for traffic to the specified destination.
   * @param native_socket The native socket handle.
   * @param address The destination address for traffic sent on this socket.
   * @param port The destination port for traffic sent on this socket.
   * @param data_type The type of traffic sent on this socket.
   * @param dscp_tagging Specifies whether to enable DSCP tagging on outgoing traffic.
   */
  std::unique_ptr<deinit_t> enable_socket_qos(uintptr_t native_socket, boost::asio::ip::address &address, uint16_t port, qos_data_type_e data_type, bool dscp_tagging) {
    int sockfd = (int) native_socket;
    std::vector<std::tuple<int, int, int>> reset_options;

    if (dscp_tagging) {
      int level;
      int option;

      // With dual-stack sockets, Linux uses IPV6_TCLASS for IPv6 traffic
      // and IP_TOS for IPv4 traffic.
      if (address.is_v6() && !address.to_v6().is_v4_mapped()) {
        level = SOL_IPV6;
        option = IPV6_TCLASS;
      } else {
        level = SOL_IP;
        option = IP_TOS;
      }

      // The specific DSCP values here are chosen to be consistent with Windows,
      // except that we use CS6 instead of CS7 for audio traffic.
      int dscp = 0;
      switch (data_type) {
        case qos_data_type_e::video:
          dscp = 40;
          break;
        case qos_data_type_e::audio:
          dscp = 48;
          break;
        default:
          BOOST_LOG(error) << "Unknown traffic type: "sv << (int) data_type;
          break;
      }

      if (dscp) {
        // Shift to put the DSCP value in the correct position in the TOS field
        dscp <<= 2;

        if (setsockopt(sockfd, level, option, &dscp, sizeof(dscp)) == 0) {
          // Reset TOS to -1 when QoS is disabled
          reset_options.emplace_back(std::make_tuple(level, option, -1));
        } else {
          BOOST_LOG(error) << "Failed to set TOS/TCLASS: "sv << errno;
        }
      }
    }

    // We can use SO_PRIORITY to set outgoing traffic priority without DSCP tagging.
    //
    // NB: We set this after IP_TOS/IPV6_TCLASS since setting TOS value seems to
    // reset SO_PRIORITY back to 0.
    //
    // 6 is the highest priority that can be used without SYS_CAP_ADMIN.
    int priority = data_type == qos_data_type_e::audio ? 6 : 5;
    if (setsockopt(sockfd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) == 0) {
      // Reset SO_PRIORITY to 0 when QoS is disabled
      reset_options.emplace_back(std::make_tuple(SOL_SOCKET, SO_PRIORITY, 0));
    } else {
      BOOST_LOG(error) << "Failed to set SO_PRIORITY: "sv << errno;
    }

    return std::make_unique<qos_t>(sockfd, reset_options);
  }

  std::string get_host_name() {
    try {
      return boost::asio::ip::host_name();
    } catch (boost::system::system_error &err) {
      BOOST_LOG(error) << "Failed to get hostname: "sv << err.what();
      return "Hermes"s;
    }
  }

  namespace source {
    enum source_e : std::size_t {
#ifdef HERMES_BUILD_CUDA
      NVFBC,  ///< NvFBC
#endif
#ifdef HERMES_BUILD_WAYLAND
      WAYLAND,  ///< Wayland
#endif
#ifdef HERMES_BUILD_DRM
      KMS,  ///< KMS
#endif
#ifdef HERMES_BUILD_X11
      X11,  ///< X11
#endif
      MAX_FLAGS  ///< The maximum number of flags
    };
  }  // namespace source

  static std::bitset<source::MAX_FLAGS> sources;

#ifdef HERMES_BUILD_CUDA
  std::vector<std::string> nvfbc_display_names();
  std::shared_ptr<display_t> nvfbc_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  bool verify_nvfbc() {
    return !nvfbc_display_names().empty();
  }
#endif

#ifdef HERMES_BUILD_WAYLAND
  std::vector<std::string> wl_display_names();
  std::shared_ptr<display_t> wl_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  bool verify_wl() {
    return window_system == window_system_e::WAYLAND && !wl_display_names().empty();
  }
#endif

#ifdef HERMES_BUILD_DRM
  std::vector<std::string> kms_display_names(mem_type_e hwdevice_type);
  std::shared_ptr<display_t> kms_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  bool verify_kms() {
    if (!kms_display_names(mem_type_e::unknown).empty()) {
      return true;
    }

    // Hermes-KMS virtual displays are captured through the driver's render
    // node (no DRM master, no CAP_SYS_ADMIN), so a present driver makes the
    // KMS backend usable even when no physical framebuffer handle is
    // readable. Without this, running unprivileged with capture=kms would
    // abort platform init before the virtual-display bootstrap can run.
    if (config::video.virtual_display_backend == "hermes_kms" &&
        VDISPLAY::isHermesKmsDriverPresent()) {
      BOOST_LOG(info) << "KMS capture enabled through Hermes-KMS render-node path (no CAP_SYS_ADMIN)"sv;
      return true;
    }

    return false;
  }
#endif

#ifdef HERMES_BUILD_X11
  std::vector<std::string> x11_display_names();
  std::shared_ptr<display_t> x11_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  bool verify_x11() {
    return window_system == window_system_e::X11 && !x11_display_names().empty();
  }
#endif

  std::vector<std::string> display_names(mem_type_e hwdevice_type) {
#ifdef HERMES_BUILD_DRM
    // An EVDI display is a real DRM output, but its name is intentionally not
    // part of the generic monitor-index list. Keep the stable virtual name so
    // a capture reinitialization cannot switch the stream back to a physical
    // output (or a lower-latency-looking but wrong Wayland/NvFBC source).
    auto virtual_displays = VDISPLAY::matchDisplay("VIRTUAL-");
    if (!virtual_displays.empty()) {
      return virtual_displays;
    }
#endif

#ifdef HERMES_BUILD_CUDA
    // display using NvFBC only supports mem_type_e::cuda
    if (sources[source::NVFBC] && hwdevice_type == mem_type_e::cuda) {
      return nvfbc_display_names();
    }
#endif
#ifdef HERMES_BUILD_WAYLAND
    if (sources[source::WAYLAND]) {
      return wl_display_names();
    }
#endif
#ifdef HERMES_BUILD_DRM
    if (sources[source::KMS]) {
      return kms_display_names(hwdevice_type);
    }
#endif
#ifdef HERMES_BUILD_X11
    if (sources[source::X11]) {
      return x11_display_names();
    }
#endif
    return {};
  }

  /**
   * @brief Returns if GPUs/drivers have changed since the last call to this function.
   * @return `true` if a change has occurred or if it is unknown whether a change occurred.
   */
  bool needs_encoder_reenumeration() {
    // We don't track GPU state, so we will always reenumerate. Fortunately, it is fast on Linux.
    return true;
  }

  std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!config.kwin_wayland_socket.empty() || !config.kwin_pipewire_socket.empty()) {
      if (config.kwin_wayland_socket.empty() || config.kwin_pipewire_socket.empty()) {
        BOOST_LOG(error) << "Incomplete private KWin endpoint; refusing host capture";
        return nullptr;
      }
#ifdef HERMES_BUILD_KWIN_TRANSPORT
      return kwin_display(hwdevice_type, config);
#else
      BOOST_LOG(error) << "Private KWin capture support was not built; refusing host capture";
      return nullptr;
#endif
    }

#ifdef HERMES_BUILD_WAYLAND
    // A Hyprland headless output has no DRM device behind it, so the KMS
    // backend has nothing to open. Route it explicitly rather than letting it
    // fall through to the generic Wayland branch: with capture=kms configured,
    // falling through would reach kms_display() and fail on a display that
    // captures perfectly well over wlr-screencopy.
    if (!VDISPLAY::getHyprlandOutputName(display_name).empty()) {
      BOOST_LOG(info) << "Screencasting Hyprland headless output with Wayland's protocol"sv;
      return wl_display(hwdevice_type, display_name, config);
    }
#endif
#ifdef HERMES_BUILD_DRM
    if (VDISPLAY::isHermesKmsDisplay(display_name)) {
      BOOST_LOG(info) << "Screencasting Hermes-KMS virtual display with KMS"sv;
      return kms_display(hwdevice_type, display_name, config);
    }
    if (VDISPLAY::isEvdiDisplay(display_name)) {
      BOOST_LOG(info) << "Screencasting EVDI virtual display with KMS"sv;
      return kms_display(hwdevice_type, display_name, config);
    }
#endif

#ifdef HERMES_BUILD_CUDA
    if (sources[source::NVFBC] && hwdevice_type == mem_type_e::cuda) {
      BOOST_LOG(info) << "Screencasting with NvFBC"sv;
      return nvfbc_display(hwdevice_type, display_name, config);
    }
#endif
#ifdef HERMES_BUILD_WAYLAND
    if (sources[source::WAYLAND]) {
      BOOST_LOG(info) << "Screencasting with Wayland's protocol"sv;
      return wl_display(hwdevice_type, display_name, config);
    }
#endif
#ifdef HERMES_BUILD_DRM
    if (sources[source::KMS]) {
      BOOST_LOG(info) << "Screencasting with KMS"sv;
      return kms_display(hwdevice_type, display_name, config);
    }
#endif
#ifdef HERMES_BUILD_X11
    if (sources[source::X11]) {
      BOOST_LOG(info) << "Screencasting with X11"sv;
      return x11_display(hwdevice_type, display_name, config);
    }
#endif

    return nullptr;
  }

  std::unique_ptr<deinit_t> init() {
    // Some existing Hermes-KMS configurations name the compositor here.
    // KWin is not a capture backend: leaving this value unresolved disables
    // every source and makes startup report all hardware encoders unavailable.
    if (config::video.capture == "kwin" && config::video.virtual_display_backend == "hermes_kms") {
      BOOST_LOG(warning) << "capture=kwin names a compositor, not a capture backend; using kms for Hermes-KMS. "
                           "Select KMS under Force a Specific Capture Method to save this setting.";
      config::video.capture = "kms";
    }

    // enable low latency mode for AMD
    // https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/30039
    set_env("AMD_DEBUG", "lowlatencyenc");

    // These are allowed to fail.
    gbm::init();

    window_system = window_system_e::NONE;
#ifdef HERMES_BUILD_WAYLAND
    if (std::getenv("WAYLAND_DISPLAY")) {
      window_system = window_system_e::WAYLAND;
    }
#endif
#if defined(HERMES_BUILD_X11) || defined(HERMES_BUILD_CUDA)
    if (std::getenv("DISPLAY") && window_system != window_system_e::WAYLAND) {
      if (std::getenv("WAYLAND_DISPLAY")) {
        BOOST_LOG(warning) << "Wayland detected, yet Hermes will use X11 for screencasting, screencasting will only work on XWayland applications"sv;
      }

      window_system = window_system_e::X11;
    }
#endif

#ifdef HERMES_BUILD_CUDA
    if ((config::video.capture.empty() && sources.none()) || config::video.capture == "nvfbc") {
      if (verify_nvfbc()) {
        sources[source::NVFBC] = true;
      }
    }
#endif
#ifdef HERMES_BUILD_WAYLAND
    if ((config::video.capture.empty() && sources.none()) || config::video.capture == "wlr") {
      if (verify_wl()) {
        sources[source::WAYLAND] = true;
      }
    }
#endif
#ifdef HERMES_BUILD_DRM
    if ((config::video.capture.empty() && sources.none()) || config::video.capture == "kms") {
      if (verify_kms()) {
        sources[source::KMS] = true;
      }
    }
#endif
#ifdef HERMES_BUILD_X11
    // We enumerate this capture backend regardless of other suitable sources,
    // since it may be needed as a NvFBC fallback for software encoding on X11.
    if (config::video.capture.empty() || config::video.capture == "x11") {
      if (verify_x11()) {
        sources[source::X11] = true;
      }
    }
#endif

    if (sources.none()) {
      BOOST_LOG(error) << "Unable to initialize capture method [" << config::video.capture
                       << "]. Linux capture values are auto (empty), kms, wlr, x11 or nvfbc; "
                          "the selected backend must also be available. Encoder probing requires a working capture source.";
      return nullptr;
    }

    if (!gladLoaderLoadEGL(EGL_NO_DISPLAY) || !eglGetPlatformDisplay) {
      BOOST_LOG(warning) << "Couldn't load EGL library"sv;
    }

    return std::make_unique<deinit_t>();
  }

  class linux_high_precision_timer: public high_precision_timer {
  public:
    void sleep_for(const std::chrono::nanoseconds &duration) override {
      std::this_thread::sleep_for(duration);
    }

    operator bool() override {
      return true;
    }
  };

  std::unique_ptr<high_precision_timer> create_high_precision_timer() {
    return std::make_unique<linux_high_precision_timer>();
  }

  std::string
  get_clipboard() {
    if (!clipboard_available()) {
      return "";
    }

    const bool wayland = window_system == window_system_e::WAYLAND;
    const auto executable = bp::search_path(wayland ? "wl-paste" : "xclip").string();
    const char *wayland_arguments[] = {"wl-paste", "--no-newline", "--type", "text/plain", nullptr};
    const char *x11_arguments[] = {"xclip", "-selection", "clipboard", "-out", nullptr};
    clipboard::pipe_t helper(executable.c_str(), wayland ? wayland_arguments : x11_arguments, false);
    FILE *pipe = helper.get();
    if (pipe == nullptr) {
      BOOST_LOG(warning) << "Unable to read the clipboard";
      return "";
    }

    constexpr size_t max_clipboard_bytes = 64 * 1024;
    std::string content;
    char buffer[4096];
    while (const size_t bytes_read = fread(buffer, 1, sizeof(buffer), pipe)) {
      if (content.size() + bytes_read > max_clipboard_bytes) {
        BOOST_LOG(warning) << "Clipboard exceeds the 64 KiB Hestia limit";
        return "";
      }
      content.append(buffer, bytes_read);
    }

    const bool read_failed = ferror(pipe) != 0;
    if (!helper.finish() || read_failed) {
      BOOST_LOG(warning) << "Unable to read the clipboard";
      return "";
    }

    return content;
  }

  bool
  set_clipboard(const std::string& content) {
    if (!clipboard_available() || content.size() > 64 * 1024 || content.find('\0') != std::string::npos) {
      return false;
    }

    const bool wayland = window_system == window_system_e::WAYLAND;
    const auto executable = bp::search_path(wayland ? "wl-copy" : "xclip").string();
    const char *wayland_arguments[] = {"wl-copy", "--type", "text/plain", nullptr};
    const char *x11_arguments[] = {"xclip", "-selection", "clipboard", "-in", nullptr};
    clipboard::pipe_t helper(executable.c_str(), wayland ? wayland_arguments : x11_arguments, true);
    const bool success = helper.write(content);
    if (!success) {
      BOOST_LOG(warning) << "Unable to write the " << (wayland ? "Wayland" : "X11") << " clipboard";
    }
    return success;
  }

  bool
  clipboard_available() {
    if (window_system == window_system_e::WAYLAND) {
      return std::getenv("WAYLAND_DISPLAY") != nullptr &&
             !bp::search_path("wl-copy").empty() && !bp::search_path("wl-paste").empty();
    }
    if (window_system == window_system_e::X11) {
      return std::getenv("DISPLAY") != nullptr && !bp::search_path("xclip").empty();
    }
    return false;
  }

  session_environment_t detect_session_environment() {
    session_environment_t env;

    const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (desktop) {
      env.desktop = desktop;
    }

    // Gamescope's nested Wayland socket is exported by gamescope-session and by
    // any running Gamescope compositor. When present, a Gamescope compositor is
    // available to capture/launch into.
    const char *gs_wl = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
    const bool desktop_is_gamescope =
      !env.desktop.empty() && boost::algorithm::to_lower_copy(env.desktop).find("gamescope") != std::string::npos;
    env.gamescope_running = (gs_wl != nullptr && gs_wl[0] != '\0') || desktop_is_gamescope;

    // A standalone Gamescope session (SteamOS Game Mode) is the compositor
    // itself: it advertises itself via XDG_CURRENT_DESKTOP=gamescope, and there
    // is no separate desktop compositor underneath. Distinguish this from a
    // desktop that merely has Gamescope installed/nested.
    if (desktop_is_gamescope) {
      env.kind = session_environment_e::gamescope_session;
      env.can_host_virtual_display = false;
      return env;
    }

    switch (window_system) {
      case window_system_e::WAYLAND:
        env.kind = session_environment_e::desktop_wayland;
        // A desktop Wayland compositor can host a virtual display when it
        // exposes an output-management protocol (kscreen/wlr).
        env.can_host_virtual_display = true;
        break;
      case window_system_e::X11:
        env.kind = session_environment_e::desktop_x11;
        env.can_host_virtual_display = true;
        break;
      case window_system_e::NONE:
      default:
        env.kind = session_environment_e::headless;
        env.can_host_virtual_display = false;
        break;
    }

    return env;
  }

  // Detect, read-only, whether a display-manager autologin drop-in exists. This
  // only inspects well-known config locations; it never enables autologin.
  static bool detect_autologin() {
    std::error_code ec;

    // getty autologin override (used for tty/Gamescope-session appliance setups).
    for (const char *path : {"/etc/systemd/system/getty@tty1.service.d/autologin.conf",
                             "/etc/systemd/system/getty@tty1.service.d/override.conf"}) {
      if (fs::exists(path, ec)) {
        std::ifstream in(path);
        std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (contents.find("--autologin") != std::string::npos || contents.find("autologin") != std::string::npos) {
          return true;
        }
      }
    }

    // Display-manager autologin: SDDM and LightDM keep it in their config trees.
    for (const char *path : {"/etc/sddm.conf", "/etc/lightdm/lightdm.conf"}) {
      if (fs::exists(path, ec)) {
        std::ifstream in(path);
        std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (contents.find("Autologin") != std::string::npos || contents.find("autologin-user") != std::string::npos) {
          return true;
        }
      }
    }
    for (const char *dir : {"/etc/sddm.conf.d", "/etc/lightdm/lightdm.conf.d"}) {
      if (!fs::exists(dir, ec)) {
        continue;
      }
      for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::ifstream in(it->path());
        std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (contents.find("Autologin") != std::string::npos || contents.find("autologin-user") != std::string::npos) {
          return true;
        }
      }
    }

    return false;
  }

  appliance_readiness_t appliance_readiness() {
    appliance_readiness_t out;
    out.enabled = config::sunshine.appliance_mode;

    const auto env = detect_session_environment();
    out.session_environment = env.describe();
    out.gamescope_available = env.gamescope_running || env.kind == session_environment_e::gamescope_session;

    // A virtual display makes the host headless-capable: it can present without
    // a physical monitor attached. Reuse the existing driver readiness checks.
    const bool evdi_ready = VDISPLAY::getEvdiDiagnostic() == VDISPLAY::EVDI_DIAGNOSTIC::READY;
    const bool kms_ready = VDISPLAY::getHermesKmsDiagnostic() == VDISPLAY::HERMES_KMS_DIAGNOSTIC::READY;
    out.virtual_display_available = evdi_ready || kms_ready;
    out.headless_capable = out.virtual_display_available;

    out.autologin_configured = detect_autologin();

    // Aggregate diagnostic. The flag being off is reported first because the
    // whole feature is dormant; when enabled, report the first missing piece.
    if (!out.enabled) {
      out.diagnostic = "disabled";
    } else if (!out.virtual_display_available) {
      out.diagnostic = "no_virtual_display";
    } else if (!out.gamescope_available) {
      out.diagnostic = "no_gamescope";
    } else if (!out.autologin_configured) {
      out.diagnostic = "no_autologin";
    } else {
      out.diagnostic = "ready";
    }

    return out;
  }
}  // namespace platf
