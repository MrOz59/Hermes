/**
 * @file tests/unit/test_network_link.cpp
 * @brief Whether an address shares a subnet with one of the host's interfaces.
 */
#include "../tests_common.h"

#include <arpa/inet.h>
#include <src/network_link.h>
#include <string>
#include <vector>

namespace {
  using net::interface_address_t;
  using net::raw_address_t;

  /// A sockaddr large enough for either family, as getifaddrs hands them out.
  struct socket_address_t {
    sockaddr_storage storage {};

    const sockaddr *get() const {
      return reinterpret_cast<const sockaddr *>(&storage);
    }
  };

  socket_address_t sockaddr_of(const std::string &text, std::uint32_t scope = 0) {
    socket_address_t out;
    if (text.find(':') == std::string::npos) {
      auto *sin = reinterpret_cast<sockaddr_in *>(&out.storage);
      sin->sin_family = AF_INET;
      inet_pton(AF_INET, text.c_str(), &sin->sin_addr);
    } else {
      auto *sin6 = reinterpret_cast<sockaddr_in6 *>(&out.storage);
      sin6->sin6_family = AF_INET6;
      inet_pton(AF_INET6, text.c_str(), &sin6->sin6_addr);
      sin6->sin6_scope_id = scope;
    }
    return out;
  }

  raw_address_t raw(const std::string &text, std::uint32_t scope = 0) {
    return *net::raw_from_sockaddr(sockaddr_of(text, scope).get());
  }

  interface_address_t subnet(const std::string &text, unsigned prefix, std::uint32_t scope = 0) {
    return {raw(text, scope), prefix};
  }
}  // namespace

TEST(NetworkLink, AGlobalAddressOfTheHomeNetworkIsOnLink) {
  // The case Hestia RS now streams as LAN: no private range covers a GUA, but
  // it shares the interface's /64.
  const std::vector<interface_address_t> links {subnet("2001:db8:1:2::10", 64)};
  EXPECT_TRUE(net::on_link(raw("2001:db8:1:2:abcd::99"), links));
  EXPECT_FALSE(net::on_link(raw("2001:db8:1:3::99"), links)) << "the next /64 is another network";
  EXPECT_FALSE(net::on_link(raw("2001:db8:ffff::1"), links));
}

TEST(NetworkLink, IPv4SubnetsCountTheirPrefixBits) {
  const std::vector<interface_address_t> links {subnet("203.0.113.10", 24), subnet("198.51.100.17", 20)};
  EXPECT_TRUE(net::on_link(raw("203.0.113.200"), links));
  EXPECT_FALSE(net::on_link(raw("203.0.114.1"), links));
  // A /20 splits the third byte: 198.51.96.0 - 198.51.111.255.
  EXPECT_TRUE(net::on_link(raw("198.51.111.254"), links));
  EXPECT_FALSE(net::on_link(raw("198.51.112.1"), links));
}

TEST(NetworkLink, AHostRouteCoversNoOtherAddress) {
  // Tailscale and other VPNs give the host a /32 or /128.
  const std::vector<interface_address_t> links {subnet("100.101.102.103", 32), subnet("fd7a:115c:a1e0::1", 128)};
  EXPECT_FALSE(net::on_link(raw("100.101.102.104"), links));
  EXPECT_FALSE(net::on_link(raw("fd7a:115c:a1e0::2"), links));
}

TEST(NetworkLink, AnInterfaceWithoutANetmaskCoversNothing) {
  const std::vector<interface_address_t> links {subnet("2001:db8::1", 0), subnet("192.0.2.1", 0)};
  EXPECT_FALSE(net::on_link(raw("2001:db8::2"), links));
  EXPECT_FALSE(net::on_link(raw("8.8.8.8"), links)) << "a zero prefix would otherwise match everything";
}

TEST(NetworkLink, FamiliesNeverMatch) {
  EXPECT_FALSE(net::in_subnet(raw("10.0.0.1"), raw("::"), 1));
  EXPECT_FALSE(net::in_subnet(raw("::1"), raw("0.0.0.0"), 1));
}

TEST(NetworkLink, LinkLocalOnAnotherInterfaceIsNotOnLink) {
  const std::vector<interface_address_t> links {subnet("fe80::1", 64, 2)};
  EXPECT_TRUE(net::on_link(raw("fe80::99", 2), links));
  EXPECT_FALSE(net::on_link(raw("fe80::99", 3), links)) << "fe80::/64 exists once per interface";
}

TEST(NetworkLink, IPv4MappedAddressesReadAsIPv4) {
  const auto mapped = raw("::ffff:192.0.2.33");
  EXPECT_FALSE(mapped.v6);
  EXPECT_EQ(mapped.size(), 4u);
  const std::vector<interface_address_t> links {subnet("192.0.2.1", 24)};
  EXPECT_TRUE(net::on_link(mapped, links));
}

TEST(NetworkLink, OtherFamiliesAreNotAddresses) {
  sockaddr other {};
  other.sa_family = AF_UNIX;
  EXPECT_FALSE(net::raw_from_sockaddr(&other).has_value());
  EXPECT_FALSE(net::raw_from_sockaddr(nullptr).has_value());
}

TEST(NetworkLink, PrefixFromNetmask) {
  EXPECT_EQ(net::prefix_from_netmask(sockaddr_of("255.255.255.0").get(), false), 24u);
  EXPECT_EQ(net::prefix_from_netmask(sockaddr_of("255.255.240.0").get(), false), 20u);
  EXPECT_EQ(net::prefix_from_netmask(sockaddr_of("255.255.255.255").get(), false), 32u);
  EXPECT_EQ(net::prefix_from_netmask(sockaddr_of("ffff:ffff:ffff:ffff::").get(), true), 64u);
  EXPECT_EQ(net::prefix_from_netmask(sockaddr_of("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff").get(), true), 128u);
  EXPECT_EQ(net::prefix_from_netmask(nullptr, true), 0u);
}

TEST(NetworkLink, ANetmaskIsReadAsTheAddressFamily) {
  // Some platforms leave the netmask's own family unset.
  auto mask = sockaddr_of("ffff:ffff:ffff:ff00::");
  reinterpret_cast<sockaddr_in6 *>(&mask.storage)->sin6_family = AF_UNSPEC;
  EXPECT_EQ(net::prefix_from_netmask(mask.get(), true), 56u);
}

TEST(NetworkLink, TheReachedAddressFindsItsInterfaceWhateverTheScope) {
  // get_mac_address(): the address a client reached, scope included, against
  // the interface's own address, which getifaddrs scopes to its index.
  EXPECT_TRUE(net::same_address(raw("fe80::1", 2), raw("fe80::1", 2)));
  EXPECT_TRUE(net::same_address(raw("fe80::1", 2), raw("fe80::1"))) << "an unscoped address matches any interface";
  EXPECT_FALSE(net::same_address(raw("fe80::1", 2), raw("fe80::1", 3))) << "the same link-local address on another link";
  EXPECT_TRUE(net::same_address(raw("2001:db8::5"), raw("2001:db8::5")));
  EXPECT_FALSE(net::same_address(raw("2001:db8::5"), raw("2001:db8::6")));
  EXPECT_TRUE(net::same_address(raw("192.0.2.7"), raw("::ffff:192.0.2.7")));
  EXPECT_FALSE(net::same_address(raw("192.0.2.7"), raw("::c000:207"))) << "an IPv4-compatible address is still IPv6";
}
