/**
 * @file src/upnp_mapping.h
 * @brief Ownership and recovery records for UPnP port mappings.
 */
#pragma once

// standard includes
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace upnp {
  /**
   * @brief Detect an owned finite lease that needs replacement on routers that do not renew repeated adds.
   * @param lease Remaining lifetime reported by the router, in seconds.
   * @return Whether a valid nonzero lease has at most ten minutes remaining.
   */
  inline bool lease_needs_refresh(std::string_view lease) {
    unsigned long seconds = 0;
    const auto result = std::from_chars(lease.data(), lease.data() + lease.size(), seconds);
    return result.ec == std::errc {} && result.ptr == lease.data() + lease.size() && seconds > 0 && seconds <= 600;
  }

  /**
   * @brief A forwarding rule Hermes owns, including its gateway and destination.
   */
  struct owned_mapping_t {
    std::string control_url;  ///< Gateway's SOAP endpoint.
    std::string service;  ///< Gateway's WAN connection service.
    std::string target;  ///< Host or inner router receiving the forwarded traffic.
    std::string external_port;  ///< Gateway port.
    std::string internal_port;  ///< Destination port.
    std::string protocol;  ///< TCP or UDP.
    std::string description;  ///< Description used when Hermes created the rule.
    bool permanent = false;  ///< Rule requires recovery after an unexpected process exit.
    std::size_t generation = 0;  ///< Last successful discovery pass that wanted this exact rule.

    /**
     * @brief Compare persistent rule identities, excluding refresh bookkeeping.
     * @param other Rule to compare.
     * @return Whether both records identify the same forwarding rule.
     */
    bool operator==(const owned_mapping_t &other) const {
      return std::tie(control_url, service, target, external_port, internal_port, protocol, description) ==
             std::tie(other.control_url, other.service, other.target, other.external_port, other.internal_port, other.protocol, other.description);
    }
  };

  /**
   * @brief Relationship between a gateway's existing mapping and the requested rule.
   */
  enum class mapping_state_e {
    missing,  ///< Gateway explicitly reports no such mapping.
    ours,  ///< Destination, port and description match Hermes's record.
    existing,  ///< Someone else's rule already forwards to the requested destination.
    conflict,  ///< Rule forwards elsewhere and must be left untouched.
    unknown,  ///< Lookup failed; ownership cannot be established.
  };

  /**
   * @brief Classify a lookup without treating a destination alone as proof of ownership.
   * @param result MiniUPnPc lookup result.
   * @param client Reported destination address.
   * @param port Reported destination port.
   * @param description Reported description.
   * @param wanted Requested or previously recorded rule.
   * @return Ownership classification.
   */
  inline mapping_state_e classify_mapping(int result, std::string_view client, std::string_view port, std::string_view description, const owned_mapping_t &wanted) {
    if (result == 714) {
      return mapping_state_e::missing;
    }
    if (result != 0) {
      return mapping_state_e::unknown;
    }
    if (client != wanted.target || port != wanted.internal_port) {
      return mapping_state_e::conflict;
    }
    return description == wanted.description ? mapping_state_e::ours : mapping_state_e::existing;
  }

  /**
   * @brief Require a local ownership record as well as a matching router response.
   * @details A pre-existing manual mapping may use the same Hermes description.
   */
  inline bool may_manage_mapping(mapping_state_e state, bool recorded) {
    return recorded && state == mapping_state_e::ours;
  }

  /** @brief Fields returned by a specific or generic mapping query. */
  struct mapping_reply_t {
    int result = -1;  ///< MiniUPnPc result; a default reply cannot authorize a mutation.
    std::string client {};  ///< Internal destination address.
    std::string internal_port {};  ///< Internal destination port, possibly the defective zero.
    std::string description {};  ///< Specific query's ownership marker; generic descriptions are unreliable.
    std::string enabled {};  ///< Router's enabled flag.
    std::string lease {};  ///< Remaining lifetime in seconds, or zero for permanent rules.
    std::string external_port {};  ///< Generic entry's external port.
    std::string protocol {};  ///< Generic entry's transport protocol.
    std::string remote_host {};  ///< Generic entry's remote restriction; empty or 0.0.0.0 means wildcard.
  };

  /** @brief Verified ownership and lease information for one mapping. */
  struct mapping_lookup_t {
    mapping_state_e state = mapping_state_e::unknown;  ///< Defaults to preserving the router rule and ownership record.
    std::string enabled {};  ///< Verified enabled flag.
    std::string lease {};  ///< Verified remaining lifetime.
  };

  /**
   * @brief Verify an owned rule even when GetSpecificPortMappingEntry reports port zero.
   * @details Some IGDs report the correct port only through GetGenericPortMappingEntry,
   * but append garbage to its description. Never use that description as ownership
   * evidence: require the exact description in specific queries both before and after
   * a complete, bounded enumeration. An unrecorded rule cannot use this fallback.
   * @param wanted Rule on this exact gateway/service and wildcard remote host.
   * @param recorded Whether Hermes has a local ownership record for this identity.
   * @param specific Query the wildcard mapping by external port and protocol.
   * @param generic Query a mapping by table index, returning 713 at the end.
   */
  template<class Specific, class Generic>
  mapping_lookup_t verify_mapping_lookup(const owned_mapping_t &wanted, bool recorded, Specific specific, Generic generic) {
    const auto first = specific();
    const auto state = classify_mapping(first.result, first.client, first.internal_port, first.description, wanted);
    if (!recorded || first.result != 0 || first.internal_port != "0" || first.client != wanted.target || first.description != wanted.description) {
      return {state, first.enabled, first.lease};
    }
    if (first.enabled != "0" && first.enabled != "1") {
      return {};
    }
    const auto parse_number = [](const std::string &text) -> std::optional<std::uint32_t> {
      std::uint32_t number = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
      if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
      }
      return number;
    };

    std::optional<mapping_reply_t> match;
    std::size_t match_index = 0;
    bool complete = false;
    // A broken IGD must not make renewal or shutdown enumerate indefinitely.
    for (std::size_t index = 0; index < 256; ++index) {
      auto entry = generic(index);
      if (entry.result == 713) {
        complete = true;
        break;
      }
      if (entry.result != 0) {
        return {};
      }
      const auto external_port = parse_number(entry.external_port);
      const auto internal_port = parse_number(entry.internal_port);
      if (!external_port || *external_port == 0 || *external_port > 65535 || !internal_port || *internal_port == 0 || *internal_port > 65535 || entry.client.empty() || (entry.protocol != "TCP" && entry.protocol != "UDP") || (entry.enabled != "0" && entry.enabled != "1")) {
        return {};
      }
      if (entry.external_port != wanted.external_port || entry.protocol != wanted.protocol || (!entry.remote_host.empty() && entry.remote_host != "0.0.0.0")) {
        continue;
      }
      if (match || entry.client != wanted.target || entry.internal_port != wanted.internal_port || entry.enabled != first.enabled) {
        return {};
      }
      match = std::move(entry);
      match_index = index;
    }
    if (!complete || !match) {
      return {};
    }

    // Enumeration is not atomic. Recheck the precise key and ownership marker;
    // an error, disappearance or replacement is uncertainty, never permission to add.
    const auto last = specific();
    if (last.result != 0 || last.client != first.client || last.internal_port != first.internal_port || last.description != first.description || last.enabled != first.enabled) {
      return {};
    }
    // Port zero in the specific recheck cannot reveal a changed internal port.
    // Re-read the selected table row as well; a shifted index or replacement fails closed.
    const auto current = generic(match_index);
    if (current.result != 0 || current.external_port != wanted.external_port || current.protocol != wanted.protocol || (!current.remote_host.empty() && current.remote_host != "0.0.0.0") || current.client != wanted.target || current.internal_port != wanted.internal_port || current.enabled != first.enabled) {
      return {};
    }
    auto duration = parse_number(first.lease);
    if (!duration) {
      return {};
    }
    for (const auto *reply : {&std::as_const(*match), &last, &current}) {
      const auto seconds = parse_number(reply->lease);
      if (!seconds || ((*duration == 0) != (*seconds == 0))) {
        return {};
      }
      duration = std::min(*duration, *seconds);
    }
    // Prefer the smaller remaining lifetime; some IGDs report the original lease
    // through one query. A permanent lease requires agreement from both queries.
    return {mapping_state_e::ours, last.enabled, std::to_string(*duration)};
  }

  /**
   * @brief Keep permanent recovery records when router ownership is ambiguous.
   * @details A gateway may rewrite descriptions without removing the forwarding rule.
   */
  inline bool should_retain_recovery_record(mapping_state_e state, bool permanent) {
    return permanent && (state == mapping_state_e::existing || state == mapping_state_e::conflict || state == mapping_state_e::unknown);
  }

  /**
   * @brief Encode one append-only recovery event before adding or after removing a permanent rule.
   * @param mapping Rule being recorded.
   * @param added Whether this records creation intent rather than completed removal.
   * @return A newline-delimited event, or an empty string for an invalid identity.
   */
  inline std::string mapping_journal_event(const owned_mapping_t &mapping, bool added) {
    for (const auto *field : {&mapping.control_url, &mapping.service, &mapping.target, &mapping.external_port, &mapping.internal_port, &mapping.protocol, &mapping.description}) {
      if (field->empty() || field->find_first_of("\r\n") != std::string::npos) {
        return {};
      }
    }
    std::ostringstream out;
    out << '\n'
        << (added ? '+' : '-') << ' ' << std::quoted(mapping.control_url) << ' ' << std::quoted(mapping.service) << ' '
        << std::quoted(mapping.target) << ' ' << std::quoted(mapping.external_port) << ' ' << std::quoted(mapping.internal_port)
        << ' ' << std::quoted(mapping.protocol) << ' ' << std::quoted(mapping.description) << '\n';
    return out.str();
  }

  /**
   * @brief Recover outstanding permanent rules, ignoring incomplete or invalid journal lines.
   * @param text Previously written recovery events.
   * @return Rules whose creation intent has no matching completed removal.
   */
  inline std::vector<owned_mapping_t> read_mapping_journal(const std::string &text) {
    std::vector<owned_mapping_t> mappings;
    std::istringstream input {text};
    std::string line;
    while (std::getline(input, line)) {
      std::istringstream entry {line};
      char event = 0;
      owned_mapping_t mapping;
      if (!(entry >> event >> std::quoted(mapping.control_url) >> std::quoted(mapping.service) >> std::quoted(mapping.target) >> std::quoted(mapping.external_port) >> std::quoted(mapping.internal_port) >> std::quoted(mapping.protocol) >> std::quoted(mapping.description)) || (event != '+' && event != '-') || mapping.control_url.empty() || mapping.service.empty() || mapping.target.empty() || mapping.external_port.empty() || mapping.internal_port.empty() || (mapping.protocol != "TCP" && mapping.protocol != "UDP") || mapping.description.empty()) {
        continue;
      }
      mapping.permanent = true;
      const auto found = std::find(mappings.begin(), mappings.end(), mapping);
      if (event == '+' && found == mappings.end()) {
        mappings.push_back(std::move(mapping));
      } else if (event == '-' && found != mappings.end()) {
        mappings.erase(found);
      }
    }
    return mappings;
  }
}  // namespace upnp
