/**
 * @file src/network.cpp
 * @brief Definitions for networking related functions.
 */
// standard includes
#include <algorithm>
#include <boost/asio.hpp>
#include <cerrno>
#include <cstring>
#include <sstream>

#ifndef _WIN32
  #include <fcntl.h>
#endif

// local includes
#include "config.h"
#include "logging.h"
#include "network.h"
#include "utility.h"

using namespace std::literals;

namespace ip = boost::asio::ip;

namespace net {
  std::vector<ip::network_v4> pc_ips_v4 {
    ip::make_network_v4("127.0.0.0/8"sv),
  };
  std::vector<ip::network_v4> lan_ips_v4 {
    ip::make_network_v4("192.168.0.0/16"sv),
    ip::make_network_v4("172.16.0.0/12"sv),
    ip::make_network_v4("10.0.0.0/8"sv),
    ip::make_network_v4("100.64.0.0/10"sv),
    ip::make_network_v4("169.254.0.0/16"sv),
  };

  std::vector<ip::network_v6> pc_ips_v6 {
    ip::make_network_v6("::1/128"sv),
  };
  std::vector<ip::network_v6> lan_ips_v6 {
    ip::make_network_v6("fc00::/7"sv),
    ip::make_network_v6("fe80::/64"sv),
  };

  net_e from_enum_string(const std::string_view &view) {
    if (view == "wan") {
      return WAN;
    }
    if (view == "lan") {
      return LAN;
    }

    return PC;
  }

  net_e from_address(const std::string_view &view) {
    auto addr = normalize_address(ip::make_address(view));

    if (addr.is_v6()) {
      for (auto &range : pc_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return PC;
        }
      }

      for (auto &range : lan_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return LAN;
        }
      }
    } else {
      for (auto &range : pc_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return PC;
        }
      }

      for (auto &range : lan_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return LAN;
        }
      }
    }

    return WAN;
  }

  std::string_view to_enum_string(net_e net) {
    switch (net) {
      case PC:
        return "pc"sv;
      case LAN:
        return "lan"sv;
      case WAN:
        return "wan"sv;
    }

    // avoid warning
    return "wan"sv;
  }

  af_e af_from_enum_string(const std::string_view &view) {
    if (view == "ipv4") {
      return IPV4;
    }
    if (view == "both") {
      return BOTH;
    }

    // avoid warning
    return BOTH;
  }

  std::string_view af_to_any_address_string(af_e af) {
    switch (af) {
      case IPV4:
        return "0.0.0.0"sv;
      case BOTH:
        return "::"sv;
    }

    // avoid warning
    return "::"sv;
  }

  boost::asio::ip::address normalize_address(boost::asio::ip::address address) {
    // Convert IPv6-mapped IPv4 addresses into regular IPv4 addresses
    if (address.is_v6()) {
      auto v6 = address.to_v6();
      if (v6.is_v4_mapped()) {
        return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);
      }
    }

    return address;
  }

  std::string addr_to_normalized_string(boost::asio::ip::address address) {
    return normalize_address(address).to_string();
  }

  std::string addr_to_url_escaped_string(boost::asio::ip::address address) {
    address = normalize_address(address);
    if (address.is_v6()) {
      std::stringstream ss;
      ss << '[' << address.to_string() << ']';
      return ss.str();
    } else {
      return address.to_string();
    }
  }

  int encryption_mode_for_address(boost::asio::ip::address address) {
    auto nettype = net::from_address(address.to_string());
    if (nettype == net::net_e::PC || nettype == net::net_e::LAN) {
      return config::stream.lan_encryption_mode;
    } else {
      return config::stream.wan_encryption_mode;
    }
  }

  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port, boost::system::error_code *error_out) {
    if (error_out) {
      error_out->clear();
    }
    auto fail = [&](boost::system::error_code ec) {
      if (error_out) {
        *error_out = ec;
      }
      BOOST_LOG(error) << "Couldn't initialize ENet control host on ["sv
                       << net::af_to_any_address_string(af) << ':' << port << "]: "
                       << ec.message() << " (" << ec.category().name() << ':' << ec.value() << ')';
    };
    static std::once_flag enet_init_flag;
    static int enet_init_result;
    std::call_once(enet_init_flag, []() {
      enet_init_result = enet_initialize();
    });
    if (enet_init_result != 0) {
      fail(boost::system::errc::make_error_code(boost::system::errc::io_error));
      return host_t {};
    }

    // Build the wildcard address instead of resolving it. enet resolves names
    // through getaddrinfo with AI_ADDRCONFIG, which refuses to return "::" on a
    // host whose only IPv6 address is on loopback - the loopback does not count
    // as a configured address for that flag. enet then reports the failure
    // through a return value nobody reads, leaving the address zeroed, and the
    // bind goes out with a length of zero and fails with EINVAL. The result was
    // a host that could never stream while the wildcard bind it actually needed
    // would have worked perfectly.
    //
    // The wildcard is not a name and has nothing to resolve, so build it.
    std::memset(&addr, 0, sizeof(addr));
    if (af == IPV4) {
      auto *sin = reinterpret_cast<sockaddr_in *>(&addr.address);
      sin->sin_family = AF_INET;
      sin->sin_addr.s_addr = htonl(INADDR_ANY);
      sin->sin_port = htons(port);
      addr.addressLength = sizeof(sockaddr_in);
    } else {
      auto *sin6 = reinterpret_cast<sockaddr_in6 *>(&addr.address);
      sin6->sin6_family = AF_INET6;
      sin6->sin6_addr = in6addr_any;
      sin6->sin6_port = htons(port);
      addr.addressLength = sizeof(sockaddr_in6);
    }

    // Maximum of 128 clients, which should be enough for anyone
    errno = 0;
#ifdef _WIN32
    WSASetLastError(0);
#endif
    auto host = host_t {enet_host_create(af == IPV4 ? AF_INET : AF_INET6, &addr, 128, 0, 0, 0)};
    if (!host) {
      // Snapshot before logging can change the OS error. ENet can also fail
      // allocating memory, so a null host alone does not prove EADDRINUSE.
      const int allocation_error = errno;
#ifdef _WIN32
      const int socket_error = WSAGetLastError();
#else
      const int socket_error = allocation_error;
#endif
      if (socket_error) {
        fail({socket_error, boost::system::system_category()});
      } else if (allocation_error) {
        fail({allocation_error, boost::system::generic_category()});
      } else {
        fail(boost::system::errc::make_error_code(boost::system::errc::io_error));
      }
      return host;
    }

    // The bundled ENet creates sockets without close-on-exec. A helper
    // launched during streaming must not keep our control port alive after
    // the last session releases its ENet host.
    boost::system::error_code ec;
    if (!prevent_socket_inheritance(host->socket, ec)) {
      fail(ec);
      return host_t {};
    }

    // Enable opportunistic QoS tagging (automatically disables if the network appears to drop tagged packets)
    enet_socket_set_option(host->socket, ENET_SOCKOPT_QOS, 1);

    return host;
  }

  bool prevent_socket_inheritance(boost::asio::ip::udp::socket::native_handle_type socket, boost::system::error_code &error) {
    error.clear();
#ifdef _WIN32
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0)) {
      error = {static_cast<int>(GetLastError()), boost::system::system_category()};
      return false;
    }
#else
    const int flags = fcntl(socket, F_GETFD);
    if (flags == -1 || fcntl(socket, F_SETFD, flags | FD_CLOEXEC) == -1) {
      error = {errno, boost::system::system_category()};
      return false;
    }
#endif
    return true;
  }

  bool udp_port_available(af_e af, std::uint16_t port) {
    boost::asio::io_context io_context;
    boost::system::error_code ec;

    const auto protocol = af == IPV4 ? boost::asio::ip::udp::v4() : boost::asio::ip::udp::v6();
    boost::asio::ip::udp::socket probe {io_context};
    probe.open(protocol, ec);
    if (ec) {
      // The family itself is unavailable, which is a different problem from a
      // taken port and not one this answer should claim.
      return true;
    }

    const auto any = af == IPV4 ?
                       boost::asio::ip::udp::endpoint {boost::asio::ip::udp::v4(), port} :
                       boost::asio::ip::udp::endpoint {boost::asio::ip::udp::v6(), port};
    probe.bind(any, ec);
    const bool available = !ec;
    probe.close(ec);
    return available;
  }

  void free_host(ENetHost *host) {
    std::for_each(host->peers, host->peers + host->peerCount, [](ENetPeer &peer_ref) {
      ENetPeer *peer = &peer_ref;

      if (peer) {
        enet_peer_disconnect_now(peer, 0);
      }
    });

    enet_host_destroy(host);
  }

  std::uint16_t map_port(int port) {
    // calculate the port from the config port
    auto mapped_port = (std::uint16_t) ((int) config::sunshine.port + port);

    // Ensure port is in the range of 1024-65535
    if (mapped_port < 1024 || mapped_port > 65535) {
      BOOST_LOG(warning) << "Port out of range: "sv << mapped_port;
    }

    return mapped_port;
  }

  /**
   * @brief Returns a string for use as the instance name for mDNS.
   * @param hostname The hostname to use for instance name generation.
   * @return Hostname-based instance name or "Hermes" if hostname is invalid.
   */
  std::string mdns_instance_name(const std::string_view &hostname) {
    // Start with the unmodified hostname
    std::string instancename {hostname.data(), hostname.size()};

    // Truncate to 63 characters per RFC 6763 section 7.2.
    if (instancename.size() > 63) {
      instancename.resize(63);
    }

    for (auto i = 0; i < instancename.size(); i++) {
      // Replace any spaces with dashes
      if (instancename[i] == ' ') {
        instancename[i] = '-';
      } else if (!std::isalnum(instancename[i]) && instancename[i] != '-') {
        // Stop at the first invalid character
        instancename.resize(i);
        break;
      }
    }

    return !instancename.empty() ? instancename : "Hermes";
  }
}  // namespace net
