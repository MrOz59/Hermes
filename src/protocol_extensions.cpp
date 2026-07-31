/**
 * @file src/protocol_extensions.cpp
 * @brief Negotiation of Hermes protocol extensions.
 */

#include "protocol_extensions.h"

#include <algorithm>
#include <array>

namespace protocol::ext {
  namespace {

    /**
     * The registry is intentionally small. An extension belongs here only once
     * this host can actually honour it: advertising one it cannot serve would
     * make a client enable a path that silently does nothing, which is worse
     * than not offering it at all.
     */
    constexpr std::array<extension_t, 1> registry {
      extension_t {
        .name = "congestion_report",
        .version = 1,
        .experimental = true,
        .summary =
          "Host publishes per-session path measurements (loss, round trip, "
          "queue delay, protection in force) through the Hestia diagnostics "
          "runtime view.",
      },
    };

  }  // namespace

  std::span<const extension_t> supported() noexcept {
    return registry;
  }

  bool negotiated_t::contains(std::string_view name) const {
    return enabled_.find(std::string {name}) != enabled_.end();
  }

  std::uint32_t negotiated_t::version_of(std::string_view name) const {
    const auto entry = enabled_.find(std::string {name});
    return entry == enabled_.end() ? 0 : entry->second;
  }

  negotiated_t negotiate(std::span<const announcement_t> announced) {
    std::map<std::string, std::uint32_t> enabled;

    for (const auto &candidate : announced) {
      const auto known = std::find_if(
        registry.begin(),
        registry.end(),
        [&](const extension_t &entry) {
          return entry.name == candidate.name &&
                 entry.version == candidate.version;
        }
      );
      if (known == registry.end()) {
        continue;
      }

      // A client may announce several versions of the same extension. Settle
      // on the highest both sides named, so either end can be upgraded first.
      auto &agreed = enabled[candidate.name];
      agreed = std::max(agreed, candidate.version);
    }

    return negotiated_t {std::move(enabled)};
  }

}  // namespace protocol::ext
