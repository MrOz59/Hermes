// SPDX-License-Identifier: GPL-3.0-only
// Session supervisor for a private, same-user stock Plasma desktop.
// The broker owns the unit/mount namespace. KWin writes only private settings.
#include "../src/platform/linux/detached_mode.h"
#include "../src/platform/linux/wayland_peer.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  volatile std::sig_atomic_t stopping = 0;

  void stop_signal(int) {
    stopping = 1;
  }

  std::string env(const char *key) {
    const auto *value = std::getenv(key);
    return value ? value : "";
  }

  void set(const char *key, const std::string &value) {
    if (::setenv(key, value.c_str(), 1)) {
      throw std::runtime_error("Could not set session environment");
    }
  }

  // The unit's PATH comes from the user's own manager, so this finds the same
  // programs the logged-in desktop would. Missing ones fail here, by name.
  std::string which(const std::string &name) {
    std::string path = env("PATH");
    if (path.empty()) {
      path = "/usr/local/bin:/usr/bin:/bin";
    }
    std::size_t start = 0;
    while (start <= path.size()) {
      const auto end = path.find(':', start);
      const auto dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (!dir.empty() && dir.front() == '/') {
        const auto candidate = fs::path {dir} / name;
        if (::access(candidate.c_str(), X_OK) == 0) {
          return candidate.string();
        }
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    throw std::runtime_error(name + " is not installed or not in PATH");
  }

  // Values the bootstrap overrides for KWin alone. "1:<value>" was set, "0" was
  // unset; the Plasma session gets back exactly what the user's manager gave.
  void remember(const char *key, const char *saved_as) {
    const auto *value = std::getenv(key);
    set(saved_as, value ? std::string {"1:"} + value : std::string {"0"});
  }

  void restore(const char *key, const char *saved_as) {
    const auto saved = env(saved_as);
    if (saved.starts_with("1:")) {
      set(key, saved.substr(2));
    } else if (::unsetenv(key)) {
      throw std::runtime_error("Could not restore session environment");
    }
    ::unsetenv(saved_as);
  }

  pid_t spawn(std::vector<std::string> args, int output = -1, bool wayland_only = false) {
    std::vector<char *> words;
    for (auto &arg : args) {
      words.push_back(arg.data());
    }
    words.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) {
      throw std::runtime_error("Could not fork session child");
    }
    if (pid == 0) {
      ::setpgid(0, 0);
      if (wayland_only) {
        // Only the mode tool needs this override. Keep KWin's Xwayland
        // DISPLAY intact in the supervisor and the applications it starts.
        ::unsetenv("DISPLAY");
        ::unsetenv("WAYLAND_SOCKET");
        ::unsetenv("KSCREEN_BACKEND");
        if (::setenv("QT_QPA_PLATFORM", "wayland", 1)) {
          ::_exit(126);
        }
      }
      if (output >= 0) {
        // Only the reader is nonblocking; child programs expect blocking stdout.
        const int flags = ::fcntl(output, F_GETFL);
        if (flags < 0 || ::fcntl(output, F_SETFL, flags & ~O_NONBLOCK) < 0 ||
            ::dup2(output, STDOUT_FILENO) < 0) {
          ::_exit(126);
        }
      }
      ::execv(words[0], words.data());
      ::_exit(127);
    }
    ::setpgid(pid, pid);
    return pid;
  }

  void terminate(pid_t pid) {
    if (pid <= 0) {
      return;
    }
    ::kill(-pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const pid_t waited = ::waitpid(pid, &status, WNOHANG);
      if (waited == pid || (waited < 0 && errno == ECHILD)) {
        return;
      }
      std::this_thread::sleep_for(20ms);
    }
    ::kill(-pid, SIGKILL);
    // Do not wait indefinitely for an uninterruptible child; the owning unit
    // also has KillMode=control-group and a bounded TimeoutStopSec.
    for (int n = 0; n < 100; ++n) {
      const pid_t waited = ::waitpid(pid, &status, WNOHANG);
      if (waited == pid || (waited < 0 && errno == ECHILD)) {
        return;
      }
      std::this_thread::sleep_for(10ms);
    }
  }

  std::string run(const std::vector<std::string> &args, std::chrono::milliseconds timeout, bool wayland_only = false) {
    int descriptors[2];
    if (::pipe2(descriptors, O_CLOEXEC | O_NONBLOCK)) {
      throw std::runtime_error("Could not create child pipe");
    }
    pid_t pid;
    try {
      pid = spawn(args, descriptors[1], wayland_only);
    } catch (...) {
      ::close(descriptors[0]);
      ::close(descriptors[1]);
      throw;
    }
    ::close(descriptors[1]);
    std::string result;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool complete = false;
    while (!stopping && std::chrono::steady_clock::now() < deadline) {
      char bytes[4096];
      for (;;) {
        const auto count = ::read(descriptors[0], bytes, sizeof(bytes));
        if (count <= 0) {
          break;
        }
        if (result.size() + count > 65536) {
          ::close(descriptors[0]);
          terminate(pid);
          throw std::runtime_error("Session child output exceeded limit");
        }
        result.append(bytes, count);
      }
      const pid_t waited = ::waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        complete = true;
        break;
      }
      if (waited < 0 && errno != EINTR) {
        break;
      }
      std::this_thread::sleep_for(10ms);
    }
    // Drain bytes written immediately before the child exited.
    char bytes[4096];
    for (;;) {
      const auto count = ::read(descriptors[0], bytes, sizeof(bytes));
      if (count <= 0) {
        break;
      }
      if (result.size() + count <= 65536) {
        result.append(bytes, count);
      }
    }
    ::close(descriptors[0]);
    if (!complete) {
      terminate(pid);
      throw std::runtime_error("Session child timed out or was interrupted: " + args.front());
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      throw std::runtime_error("Session child failed: " + args.front());
    }
    return result;
  }

  bool owns(const std::string &name) {
    const auto reply = run({which("gdbus"), "call", "--session", "--dest", "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus", "--method", "org.freedesktop.DBus.NameHasOwner", name}, 2s);
    if (reply == "(true,)\n") {
      return true;
    }
    if (reply == "(false,)\n") {
      return false;
    }
    throw std::runtime_error("Unexpected private D-Bus ownership reply");
  }

  fs::path runtime_dir() {
    const fs::path path = env("XDG_RUNTIME_DIR");

    struct stat info {};

    if (!path.is_absolute() || path == fs::path {"/run/user"} / std::to_string(::getuid()) ||
        ::lstat(path.c_str(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != ::getuid() ||
        (info.st_mode & 077) != 0 || ::getuid() == 0) {
      throw std::runtime_error("Refusing to run outside a private same-user Hermes runtime directory");
    }
    return path;
  }

  void write_private(const fs::path &path, const std::string &value) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
      throw std::runtime_error("Could not write session file: " + path.string());
    }
    std::size_t offset = 0;
    while (offset < value.size()) {
      const auto size = ::write(fd, value.data() + offset, value.size() - offset);
      if (size < 0 && errno == EINTR) {
        continue;
      }
      if (size <= 0) {
        ::close(fd);
        throw std::runtime_error("Could not write session state");
      }
      offset += size;
    }
    ::close(fd);
  }

  // One line saying where startup is, for Hermes to quote when it gives up.
  // stdout and stderr cannot carry it: KWin owns the session process's.
  void report(const fs::path &root, const std::string &text) {
    try {
      write_private(root / "status.tmp", text + "\n");
      fs::rename(root / "status.tmp", root / "status");
    } catch (...) {}
  }

  void fail(const fs::path &root, const std::string &reason) {
    std::cerr << reason << std::endl;
    try {
      write_private(root / "failure", reason + "\n");
    } catch (...) {}
  }

  std::string desktop_exec(const std::string &path) {
    std::string result = "\"";
    for (char c : path) {
      if (c == '\n' || c == '\r' || c == '%') {
        throw std::runtime_error("Unsupported executable path in desktop metadata");
      }
      if (c == '\\' || c == '"' || c == '$' || c == '`') {
        result += '\\';
      }
      result += c;
    }
    return result + "\"";
  }

  int dimension(const char *value) {
    const std::string text = value;
    if (text.empty() || text.size() > 5 || !std::all_of(text.begin(), text.end(), [](char c) {
          return c >= '0' && c <= '9';
        })) {
      throw std::runtime_error("Invalid detached output dimension");
    }
    const int n = std::stoi(text);
    if (n < 64 || n > 16384) {
      throw std::runtime_error("Detached output dimension outside supported range");
    }
    return n;
  }

  int refresh_rate(const std::string &text) {
    if (text.empty() || text.size() > 7 || !std::all_of(text.begin(), text.end(), [](char c) {
          return c >= '0' && c <= '9';
        })) {
      throw std::runtime_error("Invalid private output refresh rate");
    }
    const int value = std::stoi(text);
    platf::kwin::detached::validate({1920, 1080, value});
    return value;
  }

  void configure_output(const fs::path &root, int width, int height, int refresh) {
    // Set only by the bootstrap that gives KWin private writable settings.
    // Refuse an old generation rather than modifying its shared output store.
    if (!fs::is_regular_file(root / "kwin-config/kwinoutputconfig.json")) {
      throw std::runtime_error("This desktop predates private output configuration; start a fresh Detached desktop");
    }
    const auto doctor = which("kscreen-doctor");
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    const auto actual = platf::kwin::detached::configure({width, height, refresh}, [&](const std::string &argument) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (stopping || remaining <= 0ms) {
        throw std::runtime_error("Private output mode application timed out or was cancelled");
      }
      return run({doctor, argument}, remaining, true);
    });
    std::cout << "Detached output verified: " << actual.width << 'x' << actual.height
              << '@' << actual.refresh_mhz << " mHz" << std::endl;
  }

  void configure_existing(const fs::path &root, int width, int height, int refresh) {
    set("XDG_RUNTIME_DIR", root.string());
    runtime_dir();
    if (root.parent_path() != fs::path {"/run/user"} / std::to_string(::getuid()) / "hermes/sessions") {
      throw std::runtime_error("Not a private Hermes session directory");
    }
    std::ifstream ready(root / "ready");
    std::string line, socket, bus;
    while (std::getline(ready, line)) {
      if (line.starts_with("WAYLAND_DISPLAY=")) {
        socket = line.substr(16);
      }
      if (line.starts_with("DBUS_SESSION_BUS_ADDRESS=")) {
        bus = line.substr(25);
      }
    }
    if (socket.empty() || socket.find('/') != std::string::npos || !fs::is_socket(root / socket) || bus.empty()) {
      throw std::runtime_error("Private desktop is not ready for a mode change");
    }
    ::unsetenv("DISPLAY");
    ::unsetenv("WAYLAND_SOCKET");
    ::unsetenv("KSCREEN_BACKEND");
    set("QT_QPA_PLATFORM", "wayland");
    set("WAYLAND_DISPLAY", socket);
    set("DBUS_SESSION_BUS_ADDRESS", bus);
    if (owns("org.freedesktop.systemd1")) {
      throw std::runtime_error("Refusing to configure an output on the shared user bus");
    }
    configure_output(root, width, height, refresh);
  }

  // Commands Hermes asks this desktop to run: launch/<name>, written whole by
  // rename, holding the working directory and then the arguments, each ended
  // by a NUL. They start here so they get the desktop's own environment,
  // mount namespace and unit, exactly as one started from its menu. A request
  // that cannot start leaves launch/<name>.error with the reason.
  void start_requested(const fs::path &root, std::vector<pid_t> &launched) {
    std::error_code ec;
    std::vector<fs::path> requests;
    for (const auto &entry : fs::directory_iterator {root / "launch", ec}) {
      const auto name = entry.path().filename().string();
      if (entry.is_regular_file(ec) && name.find('.') == std::string::npos) {
        requests.push_back(entry.path());
      }
    }
    std::sort(requests.begin(), requests.end());
    for (const auto &request : requests) {
      std::ifstream stream {request, std::ios::binary};
      std::vector<std::string> fields;
      for (std::string field; std::getline(stream, field, '\0');) {
        fields.push_back(field);
      }
      try {
        if (fields.size() < 2 || fields[1].empty()) {
          throw std::runtime_error("Empty command");
        }
        const auto directory = fields.front();
        std::vector<std::string> args {fields.begin() + 1, fields.end()};
        if (args.front().find('/') == std::string::npos) {
          args.front() = which(args.front());
        }
        if (::access(args.front().c_str(), X_OK)) {
          throw std::runtime_error(args.front() + " is not executable");
        }
        std::vector<char *> words;
        for (auto &arg : args) {
          words.push_back(arg.data());
        }
        words.push_back(nullptr);
        const pid_t pid = ::fork();
        if (pid < 0) {
          throw std::runtime_error("Could not fork");
        }
        if (pid == 0) {
          ::setsid();
          if (!directory.empty() && ::chdir(directory.c_str())) {
            ::_exit(126);
          }
          ::execv(words[0], words.data());
          ::_exit(127);
        }
        launched.push_back(pid);
        report(root, "started " + args.front() + " (pid " + std::to_string(pid) + ")");
      } catch (const std::exception &error) {
        try {
          write_private(fs::path {request} += ".error", std::string {error.what()} + "\n");
        } catch (...) {}
      }
      // Last, so Hermes finds any error once the request is gone.
      fs::remove(request, ec);
    }
    // Reap only these: a wait for any child would take the status of a
    // program run() is still waiting for.
    std::erase_if(launched, [](pid_t pid) {
      int status = 0;
      return ::waitpid(pid, &status, WNOHANG) != 0;
    });
  }

  void supervise(const fs::path &root) {
    pid_t plasma = -1;
    std::vector<pid_t> launched;
    try {
      report(root, "waiting for the KWin wrapper");
      const auto deadline = std::chrono::steady_clock::now() + 8s;
      while (!owns("org.kde.KWinWrapper")) {
        if (stopping || std::chrono::steady_clock::now() >= deadline) {
          throw std::runtime_error("Private KWin wrapper did not become ready");
        }
        std::this_thread::sleep_for(100ms);
      }
      if (owns("org.freedesktop.systemd1")) {
        throw std::runtime_error("Refusing a desktop bus with a shared user manager");
      }
      // The wrapper names its own socket, the first free one in this private
      // runtime directory, and tells the session through WAYLAND_DISPLAY.
      const auto socket = env("WAYLAND_DISPLAY");
      std::error_code socket_ec;
      if (socket.empty() || socket.find('/') != std::string::npos || socket.find('\n') != std::string::npos ||
          !fs::is_socket(root / socket, socket_ec)) {
        throw std::runtime_error("KWin's Wayland socket [" + socket + "] is not in the private runtime directory");
      }
      const auto private_bus = env("DBUS_SESSION_BUS_ADDRESS");
      if (private_bus.empty() || private_bus.find('\n') != std::string::npos) {
        throw std::runtime_error("Invalid private session bus address");
      }
      // The authorization overlay is KWin's alone. Plasma and everything it
      // starts read the user's real data and cache directories.
      restore("XDG_DATA_DIRS", "HERMES_DETACHED_DATA_DIRS");
      restore("XDG_CACHE_HOME", "HERMES_DETACHED_CACHE_HOME");
      restore("XDG_CONFIG_HOME", "HERMES_DETACHED_CONFIG_HOME");
      restore("XDG_CONFIG_DIRS", "HERMES_DETACHED_CONFIG_DIRS");
      const int width = dimension(env("HERMES_DETACHED_WIDTH").c_str());
      const int height = dimension(env("HERMES_DETACHED_HEIGHT").c_str());
      const int refresh = refresh_rate(env("HERMES_DETACHED_REFRESH_MHZ"));
      report(root, "applying the requested private output mode");
      configure_output(root, width, height, refresh);
      ::unsetenv("HERMES_DETACHED_WIDTH");
      ::unsetenv("HERMES_DETACHED_HEIGHT");
      ::unsetenv("HERMES_DETACHED_REFRESH_MHZ");
      report(root, "starting Plasma on " + socket);
      plasma = spawn({which("startplasma-wayland")});
      const auto ready_deadline = std::chrono::steady_clock::now() + 35s;
      bool ready = false;
      std::string waiting;
      while (!stopping) {
        int status = 0;
        const pid_t waited = ::waitpid(plasma, &status, WNOHANG);
        if (waited == plasma) {
          plasma = -1;
          throw std::runtime_error("Private Plasma exited");
        }
        if (waited < 0 && errno != EINTR) {
          throw std::runtime_error("Could not observe private Plasma process");
        }
        if (!ready) {
          const bool shell = owns("org.kde.plasmashell");
          const bool session = owns("org.kde.ksmserver");
          const bool splash = owns("org.kde.KSplash");
          if (shell && session && !splash) {
            // Plasma startup must not silently replace the negotiated mode.
            configure_output(root, width, height, refresh);
            write_private(root / "ready.tmp", "WAYLAND_DISPLAY=" + socket + "\nDBUS_SESSION_BUS_ADDRESS=" + private_bus + "\n");
            fs::rename(root / "ready.tmp", root / "ready");
            report(root, "ready on " + socket);
            ready = true;
          } else {
            const auto now_waiting = std::string {"waiting for Plasma: plasmashell="} + (shell ? "up" : "down") +
                                     " ksmserver=" + (session ? "up" : "down") +
                                     " splash=" + (splash ? "still shown" : "gone");
            if (now_waiting != waiting) {
              report(root, waiting = now_waiting);
            }
            if (std::chrono::steady_clock::now() >= ready_deadline) {
              throw std::runtime_error("Private Plasma startup timed out (" + waiting + ")");
            }
          }
        }
        if (ready) {
          start_requested(root, launched);
        }
        std::this_thread::sleep_for(100ms);
      }
    } catch (const std::exception &failure) {
      fail(root, failure.what());
    }
    std::error_code ignored;
    fs::remove(root / "ready", ignored);
    terminate(plasma);
    // KWin's wrapper restarts on nonzero child exits. Failures are reported in
    // the generation's private status file; normal exit always ends this unit.
  }
}  // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--connect-wayland") {
    return platf::kwin::serve_wayland_peer(argv[2]);
  }
  if (argc == 6 && std::string_view(argv[1]) == "--configure-output") {
    try {
      configure_existing(fs::path {argv[2]}, dimension(argv[3]), dimension(argv[4]), refresh_rate(argv[5]));
      return 0;
    } catch (const std::exception &error) {
      std::cerr << "Detached mode change failed: " << error.what() << std::endl;
      return 1;
    }
  }
  const bool child = env("HERMES_DETACHED_STAGE") == "session";
  try {
    // No umask of our own: KWin and every program in the desktop inherit it,
    // and files the user makes there must get the same permissions as at the
    // machine. This tool's own files are private by their open() mode, inside
    // a runtime directory that is 0700 already.
    ::signal(SIGTERM, stop_signal);
    ::signal(SIGINT, stop_signal);
    const auto root = runtime_dir();
    if (child) {
      supervise(root);
      return 0;
    }
    if ((argc != 5 && argc != 6) || std::string(argv[1]) != "--bootstrap") {
      throw std::runtime_error("Usage: hermes-detached-session --bootstrap WIDTH HEIGHT HERMES_EXECUTABLE [REFRESH_MHZ]");
    }
    const auto width = std::to_string(dimension(argv[2]));
    const auto height = std::to_string(dimension(argv[3]));
    const auto refresh = std::to_string(argc == 6 ? refresh_rate(argv[5]) : 60000);
    which("kscreen-doctor");
    set("HERMES_DETACHED_WIDTH", width);
    set("HERMES_DETACHED_HEIGHT", height);
    set("HERMES_DETACHED_REFRESH_MHZ", refresh);
    const auto executable = fs::canonical(argv[4]);
    if (!executable.is_absolute() || ::access(executable.c_str(), X_OK)) {
      throw std::runtime_error("Hermes executable is unavailable");
    }
    if (owns("org.freedesktop.systemd1")) {
      throw std::runtime_error("Bootstrap requires a private session bus");
    }
    fs::create_directories(root / "data/applications");
    fs::create_directories(root / "cache");
    write_private(root / "data/applications/org.hermes.DetachedTransport.desktop", "[Desktop Entry]\nType=Application\nName=Hermes Detached Transport\nExec=" + desktop_exec(executable.string()) + "\nX-KDE-Wayland-Interfaces=org_kde_kwin_fake_input,zkde_screencast_unstable_v1\n");
    // A packaged Hermes retains capabilities needed by ordinary KMS capture.
    // KWin cannot read that process's /proc/<pid>/exe. Its private connection
    // is therefore opened by this capability-free helper and passed back.
    const auto connector = fs::canonical("/proc/self/exe");
    write_private(root / "data/applications/org.hermes.DetachedPeer.desktop", "[Desktop Entry]\nType=Application\nName=Hermes Detached Connection\nExec=" + desktop_exec(connector.string()) + "\nX-KDE-Wayland-Interfaces=org_kde_kwin_fake_input,zkde_screencast_unstable_v1\n");
    // A second PowerDevil would manage the physical monitors and the machine's
    // sleep from a desktop nobody is sitting at. The host's stays in charge.
    fs::create_directories(root / "xdg/autostart");
    write_private(root / "xdg/autostart/powerdevil.desktop", "[Desktop Entry]\nHidden=true\n");
    // This desktop has no login session, so polkit maps its requests to the
    // user's display session, where this desktop's own agent registers when
    // no other agent has. Every prompt dismissed there then counts as a failed
    // password in pam_faillock, and a few lock the user's account. Without the
    // agent, polkit refuses such a request at once and nothing is counted; one
    // it allows without asking is unaffected.
    write_private(root / "xdg/autostart/polkit-kde-authentication-agent-1.desktop", "[Desktop Entry]\nHidden=true\n");
    // This desktop is another login of the same user. Restoring the saved
    // session would reopen the host's apps in it, where a single-instance app
    // then keeps every later launch on the host, and saving its own at logout
    // would replace the session the host restores next. A system value marked
    // immutable outranks the user's ksmserverrc, which stays untouched.
    write_private(root / "xdg/ksmserverrc", "[General]\nloginMode[$i]=emptySession\n");
    // Leave Bluetooth management to the host. KDED 6 still reads kded5rc.
    // BlueDevil does not load on demand; prevent its duplicate OBEX retries.
    write_private(root / "xdg/kded5rc", "[Module-bluedevil]\nautoload[$i]=false\n");
    auto config_dirs = env("XDG_CONFIG_DIRS");
    if (config_dirs.empty()) {
      config_dirs = "/etc/xdg";
    }
    set("XDG_CONFIG_DIRS", (root / "xdg").string() + ":" + config_dirs);
    remember("XDG_DATA_DIRS", "HERMES_DETACHED_DATA_DIRS");
    remember("XDG_CACHE_HOME", "HERMES_DETACHED_CACHE_HOME");
    set("XDG_CACHE_HOME", (root / "cache").string());
    auto data_dirs = env("XDG_DATA_DIRS");
    if (data_dirs.empty()) {
      data_dirs = "/usr/local/share:/usr/share";
    }
    set("XDG_DATA_DIRS", (root / "data").string() + ":" + data_dirs);
    report(root, "building KWin's service cache");
    run({which("kbuildsycoca6"), "--noincremental"}, 10s);
    // KWin persists output changes below XDG_CONFIG_HOME. Keep those writes
    // private while retaining the user's other settings as read-only defaults.
    // The session child restores both variables before starting Plasma/apps.
    remember("XDG_CONFIG_HOME", "HERMES_DETACHED_CONFIG_HOME");
    remember("XDG_CONFIG_DIRS", "HERMES_DETACHED_CONFIG_DIRS");
    auto config_home = env("XDG_CONFIG_HOME");
    if (config_home.empty()) {
      config_home = (fs::path {env("HOME")} / ".config").string();
    }
    if (!fs::path {config_home}.is_absolute() || config_home.find(':') != std::string::npos) {
      throw std::runtime_error("Unsupported host configuration path");
    }
    fs::create_directories(root / "kwin-config");
    write_private(root / "kwin-config/kwinoutputconfig.json", "[]\n");
    set("XDG_CONFIG_HOME", (root / "kwin-config").string());
    set("XDG_CONFIG_DIRS", (root / "xdg").string() + ":" + config_home + ":" + config_dirs);
    set("HERMES_DETACHED_STAGE", "session");
    const auto self = fs::canonical("/proc/self/exe").string();
    std::vector<std::string> args {which("kwin_wayland_wrapper"), "--virtual", "--width", width, "--height", height, "--output-count", "1", "--xwayland", "--no-lockscreen", "--exit-with-session", self};
    report(root, "starting KWin");
    std::vector<char *> words;
    for (auto &arg : args) {
      words.push_back(arg.data());
    }
    words.push_back(nullptr);
    ::execv(words[0], words.data());
    throw std::runtime_error("Could not execute stock KWin wrapper");
  } catch (const std::exception &failure) {
    // Hermes watches for this file, so a failure here ends the launch at once
    // instead of when it times out. Nothing is written outside a directory
    // that passed the ownership check.
    try {
      fail(runtime_dir(), failure.what());
    } catch (...) {
      std::cerr << failure.what() << std::endl;
    }
    return child ? 0 : 1;
  }
}
