/**
 * @file src/protocol_extensions.h
 * @brief Hermes protocol extensions negotiated over the compatible session.
 */
#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace protocol::ext {

  /**
   * @brief One capability this host can speak beyond the GameStream baseline.
   *
   * Extensions are versioned independently of each other and of the Hestia
   * protocol version. A client that speaks none of them, or announces none at
   * all, gets exactly the session it would have had before the extension
   * existed -- which is what keeps unmodified Moonlight clients working.
   */
  struct extension_t {
    std::string_view name;
    std::uint32_t version = 0;
    /// The wire format may still change; a client must not depend on it.
    bool experimental = true;
    std::string_view summary;
  };

  /**
   * @brief What this build implements.
   *
   * A name may appear more than once when the host still accepts an older
   * version of an extension. Negotiation then settles on the highest version
   * both sides know, so a client can be upgraded without waiting for the host
   * and the other way round.
   */
  [[nodiscard]] std::span<const extension_t> supported() noexcept;

  /** @brief One entry of a client's announcement. */
  struct announcement_t {
    std::string name;
    std::uint32_t version = 0;
  };

  /**
   * @brief The extensions in force for a session, by name and agreed version.
   */
  class negotiated_t {
  public:
    negotiated_t() = default;
    explicit negotiated_t(std::map<std::string, std::uint32_t> enabled) noexcept:
        enabled_ {std::move(enabled)} {
    }

    [[nodiscard]] bool contains(std::string_view name) const;

    /** @brief Agreed version, or 0 when the extension is not in force. */
    [[nodiscard]] std::uint32_t version_of(std::string_view name) const;

    [[nodiscard]] bool empty() const noexcept {
      return enabled_.empty();
    }

    [[nodiscard]] const std::map<std::string, std::uint32_t> &entries() const noexcept {
      return enabled_;
    }

  private:
    std::map<std::string, std::uint32_t> enabled_;
  };

  /**
   * @brief Intersect a client's announcement with what this host supports.
   *
   * Deliberately permissive about the announcement and strict about the
   * result. A name this host does not know, or a version it does not
   * implement, is dropped rather than rejected: a client is free to announce
   * things a future host will understand, and must still get a working session
   * from this one. Anything that survives is a version both sides named.
   */
  [[nodiscard]] negotiated_t negotiate(
    std::span<const announcement_t> announced
  );

}  // namespace protocol::ext
