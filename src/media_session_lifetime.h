/**
 * @file src/media_session_lifetime.h
 * @brief Keeps a streaming session alive while global media queues use it.
 */
#pragma once

#include <cstddef>
#include <memory>

namespace stream::lifetime {

  /**
   * @brief Associate the raw channel key carried on GameStream packets with
   * the shared owner of that channel.
   *
   * The registry itself keeps only a weak reference. Encoded packets acquire a
   * strong reference immediately before entering the process-wide media queue.
   */
  void register_channel(
    const void *channel_key,
    const std::shared_ptr<void> &owner
  );

  /**
   * @brief Retain a registered channel for the lifetime of one queued packet.
   */
  [[nodiscard]] std::shared_ptr<void> retain_channel(
    const void *channel_key
  );

  /**
   * @brief Stop future packet retention for a channel.
   *
   * Existing packets keep their strong references until they are sent or
   * discarded.
   */
  void unregister_channel(const void *channel_key);

  /** @brief Number of live registry entries, exposed for bounded-state tests. */
  [[nodiscard]] std::size_t registered_channels();

}  // namespace stream::lifetime
