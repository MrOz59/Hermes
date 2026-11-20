#pragma once

namespace hermes::build {
  namespace fs = std::filesystem;

  inline fs::path configured_path(const char *key) {
    mcpp::rerun_if_env_changed(key);
    const char *value = std::getenv(key);
    return value && *value ? fs::path(value) : fs::path {};
  }

  inline std::string wayland_scanner(const fs::path &root) {
    if (auto scanner = configured_path("HERMES_MCPP_WAYLAND_SCANNER"); !scanner.empty()) {
      return scanner.string();
    }
    if (auto prefix = configured_path("HERMES_MCPP_WAYLAND_PREFIX"); !prefix.empty()) {
      return (prefix / "bin/wayland-scanner").string();
    }
    const auto local = root / ".toolchain/fedora/usr/bin/wayland-scanner";
    return fs::is_regular_file(local) ? local.string() : "wayland-scanner";
  }

  inline void add_wayland_protocol(const std::string &scanner, const fs::path &xml, const fs::path &out) {
    const auto source = xml.string();
    const auto stem = xml.stem().string();
    for (const auto &[mode, suffix] : std::array{
           std::pair{"client-header", ".h"}, std::pair{"private-code", ".c"}}) {
      const auto name = "wayland:" + stem + ":" + mode;
      const auto output = (out / (stem + suffix)).string();
      mcpp::action action;
      action.id = name.c_str();
      action.role = mcpp::roles::source;
      action.arg(scanner.c_str()).arg(mode).arg(source.c_str()).arg(output.c_str())
        .input(source.c_str()).output(output.c_str());
      if (fs::path(scanner).is_absolute()) {
        action.input(scanner.c_str());
      }
      action.submit();
    }
  }
}  // namespace hermes::build
