#pragma once

namespace hermes::build {
  inline void add_web_assets(const std::filesystem::path &root, const std::filesystem::path &out,
                             const std::filesystem::path &runtime_assets) {
    const auto script = root / "mcpp/host/stage-assets.sh";
    const auto web = root / "src_assets/common/assets/web";
    const auto assets = out / "assets";
    const auto stamp = out / "hermes-web-assets.stamp";
    const auto depfile = out / "hermes-web-assets.d";
    std::ostringstream define;
    define << "HERMES_ASSETS_DIR=" << std::quoted(runtime_assets.string());
    mcpp::define(define.str().c_str());

    mcpp::action action;
    action.id = "hermes-web-assets";
    action.role = mcpp::roles::prepare;
    action.depfile = depfile.c_str();
    action.arg("bash").arg(script.c_str()).arg(root.c_str()).arg(assets.c_str())
      .arg(stamp.c_str()).arg(depfile.c_str())
      .input(script.c_str()).input((script.parent_path() / "depfile.sh").c_str())
      .input((web / "package.json").c_str())
      .input((web / "vite.config.ts").c_str())
      .input((web / "tsconfig.json").c_str())
      .input((web / "bun.lock").c_str())
      .output(stamp.c_str()).output_dir(assets.c_str());
    action.submit();
  }
}  // namespace hermes::build
