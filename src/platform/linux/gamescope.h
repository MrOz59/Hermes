#pragma once

#include <string_view>

namespace platf::gamescope {
  // SDL's Linux backend does not advertise an HDR output. Never use the debug
  // force-support/output switches: they can expose HDR while emitting SDR.
  inline bool supports_hdr_backend(std::string_view backend) {
    return backend.empty() || backend == "auto" || backend == "wayland" || backend == "drm";
  }

  inline const char *hdr_arguments(bool enabled) {
    return enabled ? " --hdr-enabled" : "";
  }

  template<class Environment>
  void set_hdr_environment(Environment &env, bool enabled) {
    for (const auto *name : {"SUNSHINE_CLIENT_HDR", "APOLLO_CLIENT_HDR", "HERMES_CLIENT_HDR"}) {
      env[name] = enabled ? "true" : "false";
    }
    // Assign both states: a previous SDR launch or inherited environment must
    // not leave HDR disabled for a subsequent HDR session.
    env["DXVK_HDR"] = enabled ? "1" : "0";
    env["PROTON_ENABLE_HDR"] = enabled ? "1" : "0";
  }
}
