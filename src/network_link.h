/**
 * @file src/network_link.h
 * @brief Whether an address shares a subnet with one of this host's interfaces.
 *
 * Pure logic over raw addresses, so it can be tested without a network: the
 * platform lists the interfaces, net::from_address() decides with them.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

// platform includes
#include <netinet/in.h>
#include <sys/socket.h>

namespace net {

  /**
   * @brief An IP address as the bytes the network carries it in.
   */
  struct raw_address_t {
    bool v6 {false};  ///< IPv6 when set, IPv4 otherwise.
    std::array<std::uint8_t, 16> bytes {};  ///< Network byte order; IPv4 uses the first four.
    std::uint32_t scope {0};  ///< IPv6 scope (interface index), 0 when it has none.

    /// Bytes that carry the address: 4 for IPv4, 16 for IPv6.
    std::size_t size() const {
      return v6 ? 16 : 4;
    }
  };

  /**
   * @brief An address of one of this host's interfaces and the length of its subnet's prefix.
   */
  struct interface_address_t {
    raw_address_t address;
    unsigned prefix {0};  ///< 0 when the interface reported no netmask.
  };

  /**
   * @brief Whether two scopes can name the same link.
   * @details An address without a scope matches any; two scoped addresses
   * (IPv6 link-local) only match on the same interface.
   */
  inline bool same_scope(const raw_address_t &a, const raw_address_t &b) {
    return a.scope == 0 || b.scope == 0 || a.scope == b.scope;
  }

  /**
   * @brief The address a sockaddr holds.
   * @details IPv4-mapped IPv6 comes back as the IPv4 address it carries.
   * @return The address, or nothing for a null pointer or another family.
   */
  inline std::optional<raw_address_t> raw_from_sockaddr(const sockaddr *addr) {
    if (addr == nullptr) {
      return std::nullopt;
    }
    raw_address_t raw;
    if (addr->sa_family == AF_INET) {
      const auto *sin = reinterpret_cast<const sockaddr_in *>(addr);
      std::memcpy(raw.bytes.data(), &sin->sin_addr, 4);
      return raw;
    }
    if (addr->sa_family != AF_INET6) {
      return std::nullopt;
    }
    const auto *sin6 = reinterpret_cast<const sockaddr_in6 *>(addr);
    std::memcpy(raw.bytes.data(), &sin6->sin6_addr, 16);
    constexpr std::array<std::uint8_t, 12> v4_mapped {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::equal(v4_mapped.begin(), v4_mapped.end(), raw.bytes.begin())) {
      std::memmove(raw.bytes.data(), raw.bytes.data() + 12, 4);
      std::fill(raw.bytes.begin() + 4, raw.bytes.end(), 0);
      return raw;
    }
    raw.v6 = true;
    raw.scope = sin6->sin6_scope_id;
    return raw;
  }

  /**
   * @brief The prefix length of a netmask.
   * @details The mask is read as the address's family (@p v6): some platforms
   * leave the netmask's own family unset. Counting stops at the first zero bit.
   * @return The prefix length, 0 for a null mask.
   */
  inline unsigned prefix_from_netmask(const sockaddr *mask, bool v6) {
    if (mask == nullptr) {
      return 0;
    }
    std::array<std::uint8_t, 16> bytes {};
    std::size_t size = 4;
    if (v6) {
      std::memcpy(bytes.data(), &reinterpret_cast<const sockaddr_in6 *>(mask)->sin6_addr, 16);
      size = 16;
    } else {
      std::memcpy(bytes.data(), &reinterpret_cast<const sockaddr_in *>(mask)->sin_addr, 4);
    }
    unsigned prefix = 0;
    for (std::size_t i = 0; i < size; ++i) {
      const auto ones = static_cast<unsigned>(std::countl_one(bytes[i]));
      prefix += ones;
      if (ones < 8) {
        break;
      }
    }
    return prefix;
  }

  /**
   * @brief Whether @p address lies in @p network / @p prefix.
   * @details Addresses of different families never match.
   */
  inline bool in_subnet(const raw_address_t &address, const raw_address_t &network, unsigned prefix) {
    if (address.v6 != network.v6) {
      return false;
    }
    const auto bits = std::min<std::size_t>(prefix, address.size() * 8);
    const auto whole = bits / 8;
    if (!std::equal(address.bytes.begin(), address.bytes.begin() + whole, network.bytes.begin())) {
      return false;
    }
    if (const auto rest = bits % 8) {
      const auto mask = static_cast<std::uint8_t>(0xff << (8 - rest));
      return (address.bytes[whole] & mask) == (network.bytes[whole] & mask);
    }
    return true;
  }

  /**
   * @brief Whether @p address is on a subnet one of @p links is attached to.
   * @details Hestia RS decides "local" the same way (netif::on_link): an IPv6
   * global address of the home network shares the interface's /64 even though
   * no private range covers it. A VPN such as Tailscale gives the host a host
   * route (/32, /128), which covers no other address, and an interface without
   * a netmask covers nothing.
   */
  inline bool on_link(const raw_address_t &address, std::span<const interface_address_t> links) {
    return std::ranges::any_of(links, [&](const interface_address_t &link) {
      return link.prefix > 0 && same_scope(address, link.address) && in_subnet(address, link.address, link.prefix);
    });
  }

  /**
   * @brief Whether an interface address is the address the host was reached on.
   * @details Compares the bytes, not the text: a link-local address printed
   * with its scope (`fe80::1%eth0`) and the same address read from the
   * interface are equal, as long as the scopes do not name different links.
   */
  inline bool same_address(const raw_address_t &interface_address, const raw_address_t &wanted) {
    return interface_address.v6 == wanted.v6 && same_scope(interface_address, wanted) &&
           std::equal(wanted.bytes.begin(), wanted.bytes.begin() + wanted.size(), interface_address.bytes.begin());
  }

}  // namespace net
