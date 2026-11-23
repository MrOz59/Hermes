module;

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

module hermes.build_support;

import mcpp;

namespace hermes::build {
  namespace fs = std::filesystem;

  const char *configured_path(const char *key) {
    mcpp::rerun_if_env_changed(key);
    const char *value = std::getenv(key);
    return value ? value : "";
  }

  std::string wayland_scanner(const char *root) {
    if (auto scanner = configured_path("HERMES_MCPP_WAYLAND_SCANNER"); *scanner) {
      return scanner;
    }
    if (auto prefix = configured_path("HERMES_MCPP_WAYLAND_PREFIX"); *prefix) {
      return (fs::path(prefix) / "bin/wayland-scanner").string();
    }
    const auto local = fs::path(root) / ".toolchain/fedora/usr/bin/wayland-scanner";
    return fs::is_regular_file(local) ? local.string() : "wayland-scanner";
  }

  void add_wayland_protocol(const char *scanner, const char *input, const char *out) {
    const fs::path xml(input);
    const auto source = xml.string();
    const auto stem = xml.stem().string();
    for (const auto &[mode, suffix] : std::array{
           std::pair{"client-header", ".h"}, std::pair{"private-code", ".c"}}) {
      const auto name = "wayland:" + stem + ":" + mode;
      const auto output = (fs::path(out) / (stem + suffix)).string();
      mcpp::action action;
      action.id = name.c_str();
      action.role = mcpp::roles::source;
      action.arg(scanner).arg(mode).arg(source.c_str()).arg(output.c_str())
        .input(source.c_str()).output(output.c_str());
      if (fs::path(scanner).is_absolute()) {
        action.input(scanner);
      }
      action.submit();
    }
  }
}  // namespace hermes::build
