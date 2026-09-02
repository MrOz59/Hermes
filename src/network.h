/**
 * @file src/network.h
 * @brief Declarations for networking related functions.
 */
#pragma once

// standard includes
#include <tuple>
#include <utility>

// lib includes
#include <boost/asio.hpp>
#include <enet/enet.h>

// local includes
#include "utility.h"

namespace net {
  void free_host(ENetHost *host);

  /**
   * @brief Map a specified port based on the base port.
   * @param port The port to map as a difference from the base port.
   * @return The mapped port number.
   * @examples
   * std::uint16_t mapped_port = net::map_port(1);
   * @examples_end
   * @todo Ensure port is not already in use by another application.
   */
  std::uint16_t map_port(int port);

  using host_t = util::safe_ptr<ENetHost, free_host>;
  using peer_t = ENetPeer *;
  using packet_t = util::safe_ptr<ENetPacket, enet_packet_destroy>;

  enum net_e : int {
    PC,  ///< PC
    LAN,  ///< LAN
    WAN  ///< WAN
  };

  enum af_e : int {
    IPV4,  ///< IPv4 only
    BOTH  ///< IPv4 and IPv6
  };

  net_e from_enum_string(const std::string_view &view);
  std::string_view to_enum_string(net_e net);

  net_e from_address(const std::string_view &view);

  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port);

  /**
   * @brief Whether a UDP port can still be bound on this host.
   *
   * The streaming ports are only bound when a session starts, so a port another
   * program has taken is invisible until someone tries to play - at which point
   * the stream fails for a reason that is nowhere near where it is felt. This
   * answers the question ahead of time, by doing exactly what the stream will
   * do: bind the wildcard address on the same address family.
   *
   * @return false when the bind fails, which is what "in use" looks like from
   *         here; a port Hermes itself is already streaming on answers false
   *         too, so callers should ask only when nothing is streaming.
   */
  bool udp_port_available(af_e af, std::uint16_t port);

  /**
   * @brief Get the address family enum value from a string.
   * @param view The config option value.
   * @return The address family enum value.
   */
  af_e af_from_enum_string(const std::string_view &view);

  /**
   * @brief Get the wildcard binding address for a given address family.
   * @param af Address family.
   * @return Normalized address.
   */
  std::string_view af_to_any_address_string(af_e af);

  /**
   * @brief Convert an address to a normalized form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address.
   */
  boost::asio::ip::address normalize_address(boost::asio::ip::address address);

  /**
   * @brief Get the given address in normalized string form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address in string form.
   */
  std::string addr_to_normalized_string(boost::asio::ip::address address);

  /**
   * @brief Get the given address in a normalized form for the host portion of a URL.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize and escape.
   * @return Normalized address in URL-escaped string.
   */
  std::string addr_to_url_escaped_string(boost::asio::ip::address address);

  /**
   * @brief Get the encryption mode for the given remote endpoint address.
   * @param address The address used to look up the desired encryption mode.
   * @return The WAN or LAN encryption mode, based on the provided address.
   */
  int encryption_mode_for_address(boost::asio::ip::address address);

  /**
   * @brief Returns a string for use as the instance name for mDNS.
   * @param hostname The hostname to use for instance name generation.
   * @return Hostname-based instance name or "Sunshine" if hostname is invalid.
   */
  std::string mdns_instance_name(const std::string_view &hostname);
}  // namespace net
