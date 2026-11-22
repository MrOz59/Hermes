#pragma once

namespace hermes::build {
  inline void add_web_assets(const std::filesystem::path &root, const std::filesystem::path &out,
                             const std::filesystem::path &runtime_assets) {
    namespace fs = std::filesystem;
    const auto script = root / "mcpp/host/stage-assets.py";
    const auto assets = out / "assets";
    std::ostringstream define;
    define << "HERMES_ASSETS_DIR=" << std::quoted(runtime_assets.string());
    mcpp::define(define.str().c_str());

    mcpp::action action;
    action.id = "hermes-web-assets";
    action.role = mcpp::roles::prepare;
    action.arg("python3").arg(script.c_str()).arg(root.c_str()).arg(assets.c_str())
      .input(script.c_str()).input((root / "package.json").c_str())
      .input((root / "vite.config.js").c_str())
      .output((out / "hermes-web-assets.stamp").c_str()).output_dir(assets.c_str());

    const auto lock = root / "node_modules/.package-lock.json";
    if (fs::is_regular_file(lock)) {
      action.input(lock.c_str());
    }
    mcpp::rerun_if_changed(lock.c_str());
    for (const auto &tree : {"common/assets", "linux/assets", "linux/misc/session-compositors"}) {
      const auto source = root / "src_assets" / tree;
      mcpp::rerun_if_changed_glob((source / "**/*").c_str());
      for (const auto &entry : fs::recursive_directory_iterator(source)) {
        if (entry.is_regular_file()) {
          action.input(entry.path().c_str());
        }
      }
    }
    action.submit();
  }
}  // namespace hermes::build
