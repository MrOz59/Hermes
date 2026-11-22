#pragma once

namespace hermes::build {
  inline std::filesystem::path add_pyrowave(const std::filesystem::path &root, bool package_assets) {
    namespace fs = std::filesystem;

    auto prefix = configured_path("HERMES_MCPP_PYROWAVE_PREFIX");
    if (prefix.empty()) {
      prefix = root / "target/pyrowave-sdk/install";
    }
    prefix = fs::weakly_canonical(root / prefix);
    const auto include_dir = prefix / "include/pyrowave";
    const auto library = prefix / "lib/libpyrowave-shared.so";
    if (!fs::is_regular_file(include_dir / "pyrowave.h") || !fs::is_regular_file(library)) {
      throw std::runtime_error("A PyroWave shared SDK >= 0.5 is required at " + prefix.string() +
                               ". Run scripts/build-pyrowave-sdk.sh or set HERMES_MCPP_PYROWAVE_PREFIX.");
    }
    auto runpath = configured_path("HERMES_MCPP_PYROWAVE_RUNPATH");
    if (runpath.empty()) {
      runpath = package_assets ? fs::path("/usr/lib64/hermes") : library.parent_path();
    }

    mcpp::include_dir(include_dir.c_str());
    mcpp::link_flag(library.c_str());
    mcpp::link_flag(("-Wl,-rpath," + runpath.string()).c_str());
    mcpp::link_lib("vulkan");
    return library;
  }
}  // namespace hermes::build
