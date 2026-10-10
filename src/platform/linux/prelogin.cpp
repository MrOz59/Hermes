/**
 * @file src/platform/linux/prelogin.cpp
 * @brief Definitions for setting up and checking reachability before login.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <vector>

// lib includes
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

// local includes
#include "prelogin.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/utility.h"
#include "virtual_display.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace platf::prelogin {

  namespace {

    constexpr auto generated_marker = "# hermes-prelogin: generated"sv;

    /** Targets that only exist once somebody has logged in. */
    constexpr std::array session_targets {
      "graphical-session.target"sv,
      "graphical-session-pre.target"sv,
      "xdg-desktop-autostart.target"sv,
    };

    /** [Unit] settings that tie a unit to another one. */
    constexpr std::array dependency_keys {
      "After"sv,
      "Before"sv,
      "Wants"sv,
      "Requires"sv,
      "Requisite"sv,
      "BindsTo"sv,
      "PartOf"sv,
      "Upholds"sv,
    };

    constexpr auto docs_pointer = "The steps are in docs/getting_started.md, under \"Before anyone logs in\"."sv;

    std::string_view trim(std::string_view text) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
      }
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
      }
      return text;
    }

    std::string lower(std::string_view text) {
      std::string out {text};
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return out;
    }

    std::vector<std::string_view> lines_of(std::string_view text) {
      std::vector<std::string_view> lines;
      while (!text.empty()) {
        const auto end = text.find('\n');
        lines.push_back(text.substr(0, end));
        if (end == std::string_view::npos) {
          break;
        }
        text.remove_prefix(end + 1);
      }
      return lines;
    }

    std::optional<std::string> read_file(const fs::path &path) {
      std::ifstream in {path, std::ios::binary};
      if (!in) {
        return std::nullopt;
      }
      return std::string {std::istreambuf_iterator<char> {in}, std::istreambuf_iterator<char> {}};
    }

    std::string env_or(const char *name, const std::string &fallback) {
      const char *value = std::getenv(name);
      return value && *value ? std::string {value} : fallback;
    }

    std::string home() {
      if (const char *value = std::getenv("HOME"); value && *value) {
        return value;
      }
      if (const auto *account = ::getpwuid(::getuid()); account && account->pw_dir) {
        return account->pw_dir;
      }
      return {};
    }

    fs::path user_unit_dir() {
      return fs::path {env_or("XDG_CONFIG_HOME", home() + "/.config")} / "systemd" / "user";
    }

    /** Remembers that `enable` turned linger on, so `disable` only undoes its own. */
    fs::path linger_marker() {
      return fs::path {env_or("XDG_STATE_HOME", home() + "/.local/state")} / "hermes" / "prelogin-linger";
    }

    struct packaged_unit_t {
      std::string name;
      fs::path path;
    };

    /**
     * The unit the package installed. CMake generates it as sunshine.service
     * and most packagings rename it, so both names are looked for; the user's
     * own configuration directory is not, since that is where the generated
     * copy goes.
     */
    std::optional<packaged_unit_t> find_packaged_unit() {
      std::error_code ec;
      for (const char *name : {"hermes.service", "sunshine.service"}) {
        for (const char *dir : {"/etc/systemd/user", "/usr/local/lib/systemd/user", "/usr/lib/systemd/user"}) {
          const auto path = fs::path {dir} / name;
          if (fs::is_regular_file(path, ec)) {
            return packaged_unit_t {name, path};
          }
        }
      }
      return std::nullopt;
    }

    /**
     * Whether this account opens @p node by ownership or group alone.
     *
     * The seat grant logind adds for whoever is in front is an ACL on top of
     * these bits, and before login it goes to the greeter. The groups are the
     * account's, not this process's: what counts is the next boot.
     */
    std::optional<bool> opens_without_seat_grant(const fs::path &node) {
      struct stat info {};
      if (::stat(node.c_str(), &info) != 0) {
        return std::nullopt;
      }
      const uid_t uid = ::getuid();
      if (info.st_uid == uid) {
        return (info.st_mode & S_IRUSR) && (info.st_mode & S_IWUSR);
      }

      const auto *account = ::getpwuid(uid);
      std::vector<gid_t> groups;
      if (account) {
        int count = 64;
        groups.resize(static_cast<size_t>(count));
        if (::getgrouplist(account->pw_name, account->pw_gid, groups.data(), &count) < 0) {
          groups.resize(static_cast<size_t>(count));
          ::getgrouplist(account->pw_name, account->pw_gid, groups.data(), &count);
        }
        groups.resize(static_cast<size_t>(std::max(count, 0)));
      }
      if (std::find(groups.begin(), groups.end(), info.st_gid) != groups.end()) {
        return (info.st_mode & S_IRGRP) && (info.st_mode & S_IWGRP);
      }
      return (info.st_mode & S_IROTH) && (info.st_mode & S_IWOTH);
    }

    std::optional<bool> hermes_kms_opens_without_seat_grant() {
      std::error_code ec;
      std::optional<bool> result;
      for (const auto &entry : fs::directory_iterator {"/sys/class/drm", ec}) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("renderD")) {
          continue;
        }
        const auto driver = fs::read_symlink(entry.path() / "device" / "driver", ec);
        if (ec || driver.filename() != "hermes-kms") {
          ec.clear();
          continue;
        }
        const auto opens = opens_without_seat_grant(fs::path {"/dev/dri"} / name);
        if (opens.value_or(false)) {
          return true;
        }
        if (opens) {
          result = false;
        }
      }
      return result;
    }

    /** Every *.conf in @p dir, in the order SDDM reads them. */
    void append_conf_dir(const fs::path &dir, std::vector<std::string> &files) {
      std::error_code ec;
      std::vector<fs::path> paths;
      for (const auto &entry : fs::directory_iterator {dir, ec}) {
        if (entry.path().extension() == ".conf") {
          paths.push_back(entry.path());
        }
      }
      std::sort(paths.begin(), paths.end());
      for (const auto &path : paths) {
        if (auto text = read_file(path)) {
          files.push_back(std::move(*text));
        }
      }
    }

    const char *marker_for(state_e state) {
      switch (state) {
        case state_e::ready:
          return "ok     ";
        case state_e::missing:
          return "missing";
        case state_e::unknown:
          return "unknown";
        case state_e::note:
          return "note   ";
      }
      return "       ";
    }

    void print(const report_t &report) {
      std::cout << "Reaching this machine before anyone logs in: "
                << (report.ready ? "ready" : report.enabled ? "set to start at boot, but not ready" : "not set up")
                << "\n\n";
      for (const auto &check : report.checks) {
        std::cout << "  " << marker_for(check.state) << "  " << check.title << ": " << check.detail << '\n';
        if (!check.fix.empty()) {
          std::cout << "           -> " << check.fix << '\n';
        }
      }
      std::cout << std::endl;
    }

    /** Run a command, showing what it said only when it fails. */
    bool run(const std::string &command) {
      bool timed_out = false;
      if (VDISPLAY::boundedCommandOutput(command + " >/dev/null 2>&1", 30s, &timed_out)) {
        return true;
      }
      std::cerr << (timed_out ? "Timed out: " : "Failed: ") << command << std::endl;
      return false;
    }

    int usage(const char *name) {
      std::cerr << "Usage: " << name << " --prelogin status|enable|disable\n\n"
                << "  status   say what stands between this machine and being reachable before login\n"
                << "  enable   start Hermes with your user manager, and that manager at boot\n"
                << "  disable  go back to starting Hermes with the graphical session\n"
                << std::endl;
      return 2;
    }

    int enable() {
      const auto packaged = find_packaged_unit();
      if (!packaged) {
        std::cerr << "No packaged hermes.service or sunshine.service was found to start at boot." << std::endl;
        return 1;
      }
      const auto source = read_file(packaged->path);
      if (!source) {
        std::cerr << "Cannot read " << packaged->path.string() << '.' << std::endl;
        return 1;
      }

      const auto target = user_unit_dir() / packaged->name;
      if (const auto existing = read_file(target); existing && !is_generated_unit(*existing)) {
        std::cerr << target.string() << " is a unit of your own, and this would replace it. Move it away first."
                  << std::endl;
        return 1;
      }

      std::error_code ec;
      fs::create_directories(target.parent_path(), ec);
      {
        std::ofstream out {target, std::ios::binary | std::ios::trunc};
        out << from_boot_unit(*source, packaged->path.string());
        if (!out) {
          std::cerr << "Cannot write " << target.string() << '.' << std::endl;
          return 1;
        }
      }
      if (!run("systemctl --user daemon-reload") || !run("systemctl --user enable " + packaged->name)) {
        return 1;
      }

      if (!collect_facts().linger.value_or(false)) {
        if (!run("loginctl enable-linger")) {
          return 1;
        }
        const auto marker = linger_marker();
        fs::create_directories(marker.parent_path(), ec);
        std::ofstream {marker} << "enabled by hermes --prelogin\n";
      }

      const auto result = report();
      print(result);
      std::cout << "This takes effect the next time Hermes starts: at the next boot, or with\n"
                << "`systemctl --user restart " << packaged->name << "`, which ends a stream in progress." << std::endl;
      return result.ready ? 0 : 1;
    }

    int disable() {
      std::error_code ec;
      const auto packaged = find_packaged_unit();
      if (packaged) {
        const auto target = user_unit_dir() / packaged->name;
        if (const auto existing = read_file(target)) {
          if (!is_generated_unit(*existing)) {
            std::cerr << target.string() << " is a unit of your own; it was left as it is." << std::endl;
          } else {
            // While the copy still exists, so that its default.target link is
            // the one removed.
            run("systemctl --user disable " + packaged->name);
            fs::remove(target, ec);
            run("systemctl --user daemon-reload");
            run("systemctl --user enable " + packaged->name);
          }
        }
      }

      const auto marker = linger_marker();
      if (fs::exists(marker, ec)) {
        run("loginctl disable-linger");
        fs::remove(marker, ec);
      }

      print(report());
      return 0;
    }

  }  // namespace

  std::string state_name(state_e state) {
    switch (state) {
      case state_e::ready:
        return "ready";
      case state_e::missing:
        return "missing";
      case state_e::unknown:
        return "unknown";
      case state_e::note:
        return "note";
    }
    return "unknown";
  }

  bool is_generated_unit(std::string_view unit) {
    for (const auto line : lines_of(unit)) {
      if (trim(line) == generated_marker) {
        return true;
      }
    }
    return false;
  }

  bool unit_waits_for_session(std::string_view unit) {
    for (const auto raw : lines_of(unit)) {
      const auto line = trim(raw);
      if (line.empty() || line.front() == '#' || line.front() == ';') {
        continue;
      }
      const auto equals = line.find('=');
      if (equals == std::string_view::npos) {
        continue;
      }
      std::istringstream words {std::string {line.substr(equals + 1)}};
      for (std::string word; words >> word;) {
        if (std::find(session_targets.begin(), session_targets.end(), word) != session_targets.end()) {
          return true;
        }
      }
    }
    return false;
  }

  std::string from_boot_unit(std::string_view packaged_unit, std::string_view source_path) {
    std::ostringstream out;
    out << generated_marker << '\n'
        << "# Written by `hermes --prelogin enable` from " << source_path << ": the same\n"
        << "# service, started with the user manager instead of the graphical session, so\n"
        << "# that it is running before anyone logs in. `hermes --prelogin disable`\n"
        << "# removes this file; changes made here are lost the next time it is written.\n";

    std::string section;
    bool restart_seen = false;
    bool install_written = false;

    // What a section owes the unit once all of its own lines are through.
    const auto close_section = [&]() {
      if (section == "[Service]" && !restart_seen) {
        out << "Restart=on-failure\nRestartSec=5s\n";
      }
      if (section == "[Install]") {
        out << "WantedBy=default.target\n";
        install_written = true;
      }
    };

    for (const auto raw : lines_of(packaged_unit)) {
      const auto line = trim(raw);
      // The packaged comments explain a unit that waits for the session.
      if (line.empty() || line.front() == '#' || line.front() == ';') {
        continue;
      }
      if (line.front() == '[' && line.back() == ']') {
        close_section();
        section = std::string {line};
        out << '\n' << section << '\n';
        continue;
      }

      const auto equals = line.find('=');
      const auto key = trim(line.substr(0, equals));
      const auto value = equals == std::string_view::npos ? std::string_view {} : trim(line.substr(equals + 1));

      if (section == "[Unit]" && std::find(dependency_keys.begin(), dependency_keys.end(), key) != dependency_keys.end()) {
        std::string kept;
        std::istringstream words {std::string {value}};
        for (std::string word; words >> word;) {
          if (std::find(session_targets.begin(), session_targets.end(), word) == session_targets.end()) {
            kept += kept.empty() ? word : ' ' + word;
          }
        }
        if (!kept.empty()) {
          out << key << '=' << kept << '\n';
        }
        continue;
      }
      if (section == "[Install]" && (key == "WantedBy" || key == "RequiredBy" || key == "UpheldBy")) {
        continue;
      }
      if (section == "[Service]" && key == "Restart") {
        restart_seen = true;
      }
      out << line << '\n';
    }
    close_section();
    if (!install_written) {
      out << "\n[Install]\nWantedBy=default.target\n";
    }
    return out.str();
  }

  sddm_settings_t sddm_settings(const std::vector<std::string> &files) {
    sddm_settings_t settings;
    for (const auto &file : files) {
      std::string section;
      for (const auto raw : lines_of(file)) {
        const auto line = trim(raw);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
          continue;
        }
        if (line.front() == '[' && line.back() == ']') {
          section = lower(line);
          continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
          continue;
        }
        const auto key = lower(trim(line.substr(0, equals)));
        const auto value = trim(line.substr(equals + 1));
        if (section == "[general]" && key == "displayserver") {
          settings.display_server = lower(value);
        } else if (section == "[autologin]" && key == "user") {
          settings.autologin_user = std::string {value};
        }
      }
    }
    return settings;
  }

  report_t assess(const facts_t &facts) {
    report_t report;
    report.enabled = facts.unit_from_boot && facts.unit_enabled;

    const auto add = [&report](std::string id, state_e state, std::string title, std::string detail, std::string fix = {}) {
      report.checks.push_back({std::move(id), state, std::move(title), std::move(detail), std::move(fix)});
      return state == state_e::ready;
    };
    const auto user = facts.user.empty() ? "this user"s : facts.user;
    bool ready = true;

    if (facts.unit_name.empty()) {
      ready &= add("unit", state_e::missing, "Start at boot", "No packaged hermes.service or sunshine.service was found, so there is no unit to start at boot.", "Install Hermes from a package, or write a user unit that starts it from default.target.");
    } else if (report.enabled) {
      ready &= add("unit", state_e::ready, "Start at boot", facts.unit_name + (facts.unit_generated ? " starts with the user manager." : " is a unit of your own that starts with the user manager."));
    } else {
      ready &= add("unit", state_e::missing, "Start at boot", facts.unit_name + " starts with the graphical session, and there is none before login.", "Run `hermes --prelogin enable`.");
    }

    if (!facts.linger) {
      ready &= add("linger", state_e::unknown, "User manager at boot", "Whether the user manager for " + user + " runs from boot could not be read.");
    } else if (*facts.linger) {
      ready &= add("linger", state_e::ready, "User manager at boot", "The user manager for " + user + " runs from boot.");
    } else {
      ready &= add("linger", state_e::missing, "User manager at boot", "The user manager for " + user + " only runs while they are logged in.", "Run `hermes --prelogin enable`, or `loginctl enable-linger " + user + "`.");
    }

    if (facts.display_manager.empty()) {
      ready &= add("greeter", state_e::unknown, "Login screen", "No display manager is enabled, so there is no login screen to stream.");
    } else if (facts.display_manager == "sddm") {
      if (!facts.sddm) {
        ready &= add("greeter", state_e::unknown, "Login screen", "SDDM's configuration could not be read.");
      } else if (facts.sddm->display_server == "wayland") {
        ready &= add("greeter", state_e::ready, "Login screen", "SDDM uses its Wayland greeter, which lights a new output by itself.");
      } else {
        ready &= add("greeter", state_e::missing, "Login screen", "SDDM uses its X11 greeter, which never lights a virtual display.", "As root, set DisplayServer=wayland for SDDM, in /etc/sddm.conf too if it is set there. " + std::string {docs_pointer});
      }
      if (facts.sddm && !facts.sddm->autologin_user.empty()) {
        add("autologin", state_e::note, "Autologin", "SDDM logs " + facts.sddm->autologin_user + " in by itself, so the login screen is never shown and none of this is used while that stays on.");
      }
    } else if (facts.display_manager == "gdm") {
      ready &= add("greeter", state_e::unknown, "Login screen", "GDM's greeter is a Wayland session and should light a new output, but it has not been run with Hermes.");
    } else {
      ready &= add("greeter", state_e::unknown, "Login screen", facts.display_manager + " has not been run with Hermes; its greeter has to light a new output by itself.");
    }

    if (!facts.input_without_session) {
      ready &= add("input", state_e::unknown, "Input before login", "/dev/uinput was not found.");
    } else if (*facts.input_without_session) {
      ready &= add("input", state_e::ready, "Input before login", "Hermes can create its keyboard and mouse with no session of its own.");
    } else {
      ready &= add("input", state_e::missing, "Input before login", "/dev/uinput is granted to whoever is on the seat, which before login is the greeter.", "Run `sudo usermod -aG input " + user + "` and reboot.");
    }

    if (facts.virtual_display_backend != "hermes_kms") {
      ready &= add("virtual_display", state_e::missing, "Virtual display", "Only the Hermes-KMS virtual display has been run before login; this host is set to '" + facts.virtual_display_backend + "'.", "Set virtual_display_backend to hermes_kms.");
    } else if (!facts.hermes_kms_without_session) {
      ready &= add("virtual_display", state_e::unknown, "Virtual display", "No Hermes-KMS device was found.", "Install and load the Hermes-KMS driver.");
    } else if (*facts.hermes_kms_without_session) {
      ready &= add("virtual_display", state_e::ready, "Virtual display", "The Hermes-KMS device opens with no session.");
    } else {
      ready &= add("virtual_display", state_e::missing, "Virtual display", "The Hermes-KMS device belongs to root until someone is on the seat.", "Run `sudo hermes-kms-setup configure --user auto`.");
    }

    report.ready = ready;
    return report;
  }

  facts_t collect_facts() {
    facts_t facts;
    std::error_code ec;

    if (const auto *account = ::getpwuid(::getuid()); account && account->pw_name) {
      facts.user = account->pw_name;
    }

    if (const auto packaged = find_packaged_unit()) {
      facts.unit_name = packaged->name;
      const auto dir = user_unit_dir();
      if (const auto text = read_file(dir / packaged->name)) {
        facts.unit_generated = is_generated_unit(*text);
        facts.unit_from_boot = !unit_waits_for_session(*text);
      }
      facts.unit_enabled = fs::is_symlink(dir / "default.target.wants" / packaged->name, ec);
    }

    if (!facts.user.empty() && fs::is_directory("/var/lib/systemd/linger", ec)) {
      facts.linger = fs::exists(fs::path {"/var/lib/systemd/linger"} / facts.user, ec);
    }

    const auto display_manager = fs::read_symlink("/etc/systemd/system/display-manager.service", ec);
    if (!ec) {
      facts.display_manager = display_manager.stem().string();
    }
    if (facts.display_manager == "sddm") {
      std::vector<std::string> files;
      append_conf_dir("/usr/lib/sddm/sddm.conf.d", files);
      append_conf_dir("/etc/sddm.conf.d", files);
      if (auto text = read_file("/etc/sddm.conf")) {
        files.push_back(std::move(*text));
      }
      if (!files.empty()) {
        facts.sddm = sddm_settings(files);
      }
    }

    facts.input_without_session = opens_without_seat_grant("/dev/uinput");
    facts.virtual_display_backend = config::video.virtual_display_backend;
    facts.hermes_kms_without_session = hermes_kms_opens_without_seat_grant();
    return facts;
  }

  report_t report() {
    return assess(collect_facts());
  }

  void log_report() {
    const auto result = report();
    if (!result.enabled) {
      return;
    }
    if (result.ready) {
      BOOST_LOG(info) << "[Prelogin] This host is set up to be reached before anyone logs in."sv;
      return;
    }
    BOOST_LOG(warning) << "[Prelogin] Hermes starts at boot, but this host cannot be reached before login yet:"sv;
    for (const auto &check : result.checks) {
      if (check.state == state_e::ready || check.state == state_e::note) {
        continue;
      }
      BOOST_LOG(warning) << "[Prelogin]   "sv << check.title << ": "sv << check.detail
                         << (check.fix.empty() ? ""s : " Fix: " + check.fix);
    }
  }

  int command(const char *name, int argc, char *argv[]) {
    if (argc != 1) {
      return usage(name);
    }
    if (::getuid() == 0) {
      std::cerr << "Run this as the user Hermes streams for, not as root." << std::endl;
      return 2;
    }

    const std::string_view action {argv[0]};
    if (action == "status"sv) {
      const auto result = report();
      print(result);
      return result.ready ? 0 : 1;
    }
    if (action == "enable"sv) {
      return enable();
    }
    if (action == "disable"sv) {
      return disable();
    }
    return usage(name);
  }

}  // namespace platf::prelogin
