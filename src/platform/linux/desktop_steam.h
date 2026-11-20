/**
 * @file src/platform/linux/desktop_steam.h
 * @brief Find the user's Steam client, native or Flatpak.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace platf::desktop_steam {

  /// The Flatpak's application ID.
  inline constexpr std::string_view flatpak_id = "com.valvesoftware.Steam";

  struct client_t {
    pid_t pid;
    /// Runs in the Flatpak, so `flatpak run` starts it again.
    bool flatpak;
  };

  namespace detail {
    inline std::string first_line(const std::filesystem::path &file) {
      std::ifstream stream {file};
      std::string line;
      std::getline(stream, line);
      return line;
    }

    inline bool owned_by(const std::filesystem::path &entry, uid_t uid) {
      std::ifstream stream {entry / "status"};
      for (std::string line; std::getline(stream, line);) {
        if (line.starts_with("Uid:")) {
          // Real, effective, saved and filesystem IDs: the real one is first.
          try {
            return std::stoul(line.substr(4)) == uid;
          } catch (...) {
            return false;
          }
        }
      }
      return false;
    }

    /// The client is `ubuntu12_32/steam` (or `ubuntu12_64/steam`) under the
    /// Steam directory. The `steam` launcher script has the same name, so the
    /// name alone does not tell them apart.
    inline bool is_client(const std::filesystem::path &entry) {
      if (first_line(entry / "comm") != "steam") {
        return false;
      }
      std::error_code ec;
      const auto exe = std::filesystem::read_symlink(entry / "exe", ec);
      return !ec && exe.filename() == "steam" && exe.parent_path().filename().string().starts_with("ubuntu12_");
    }

    inline bool in_flatpak(const std::filesystem::path &entry) {
      // systemd puts a Flatpak in app-flatpak-<id>-<n>.scope; .flatpak-info
      // is in the root of every Flatpak sandbox, should the scope be missing.
      std::error_code ec;
      return first_line(entry / "cgroup").find("app-flatpak-" + std::string {flatpak_id}) != std::string::npos ||
             std::filesystem::exists(entry / "root" / ".flatpak-info", ec);
    }
  }  // namespace detail

  /**
   * @brief The user's running Steam client.
   *
   * `~/.steam/steam.pid` names the native client. A Flatpak's pid file is in
   * its own home, `~/.var/app/com.valvesoftware.Steam`, and holds the pid in
   * the sandbox's pid namespace, which means nothing outside it; so this looks
   * through /proc for the client instead.
   *
   * @param proc The proc file system.
   * @param uid The user whose Steam to find.
   * @param pid_file The native pid file, tried first; empty to skip it.
   * @param own_cgroup Hermes' own cgroup when it runs as a service; a Steam
   *                   under it is one Hermes started, not the desktop's.
   */
  inline std::optional<client_t> find(const std::filesystem::path &proc, uid_t uid, const std::filesystem::path &pid_file, std::string_view own_cgroup) {
    const auto candidate = [&](pid_t pid) -> std::optional<client_t> {
      const auto entry = proc / std::to_string(pid);
      if (pid <= 0 || detail::first_line(entry / "comm") != "steam" || !detail::owned_by(entry, uid)) {
        return std::nullopt;
      }
      if (own_cgroup.ends_with(".service") && detail::first_line(entry / "cgroup").starts_with(own_cgroup)) {
        return std::nullopt;
      }
      return client_t {pid, detail::in_flatpak(entry)};
    };

    if (!pid_file.empty()) {
      pid_t pid = 0;
      // The pid file outlives the client, so the process is checked too.
      std::ifstream {pid_file} >> pid;
      if (const auto client = candidate(pid)) {
        return client;
      }
    }

    std::optional<client_t> found;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator {proc, ec}) {
      const auto name = entry.path().filename().string();
      if (name.empty() || !std::ranges::all_of(name, [](unsigned char c) {
            return std::isdigit(c);
          })) {
        continue;
      }
      const pid_t pid = std::stoi(name);
      if (found && found->pid < pid) {
        continue;
      }
      if (!detail::is_client(entry.path())) {
        continue;
      }
      if (const auto client = candidate(pid)) {
        found = client;
      }
    }
    return found;
  }

}  // namespace platf::desktop_steam
