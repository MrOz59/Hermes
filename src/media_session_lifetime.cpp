/**
 * @file src/media_session_lifetime.cpp
 * @brief Shared ownership bridge for raw GameStream media channel keys.
 */

#include "media_session_lifetime.h"

#include <mutex>
#include <unordered_map>

namespace stream::lifetime {
  namespace {
    std::mutex registry_mutex;
    std::unordered_map<const void *, std::weak_ptr<void>> registry;
  }  // namespace

  void register_channel(
    const void *channel_key,
    const std::shared_ptr<void> &owner
  ) {
    if (!channel_key || !owner) {
      return;
    }

    std::lock_guard lock {registry_mutex};
    registry.insert_or_assign(channel_key, owner);
  }

  std::shared_ptr<void> retain_channel(const void *channel_key) {
    if (!channel_key) {
      return {};
    }

    std::lock_guard lock {registry_mutex};
    const auto matching = registry.find(channel_key);
    if (matching == registry.end()) {
      return {};
    }

    auto owner = matching->second.lock();
    if (!owner) {
      registry.erase(matching);
    }
    return owner;
  }

  void unregister_channel(const void *channel_key) {
    if (!channel_key) {
      return;
    }

    std::lock_guard lock {registry_mutex};
    registry.erase(channel_key);
  }

  std::size_t registered_channels() {
    std::lock_guard lock {registry_mutex};
    return registry.size();
  }

}  // namespace stream::lifetime
