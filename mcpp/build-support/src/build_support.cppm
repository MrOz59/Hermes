module;

#include <string>

export module hermes.build_support;

// Keep filesystem types inside the implementation; older GCC/libstdc++
// cannot reliably merge their declarations across the module boundary.
export namespace hermes::build {
  const char *configured_path(const char *key);
  std::string wayland_scanner(const char *root);
  void add_wayland_protocol(const char *scanner, const char *input, const char *out);
}
