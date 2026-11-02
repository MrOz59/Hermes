// SPDX-License-Identifier: GPL-3.0-only
/** @file session_broker_transport.h
 *  @brief Deadline-bounded Unix transport for the session broker client.
 */
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace platf::session_broker::detail {
  struct exchange_result {
    std::optional<std::string> reply;
    std::string error;
  };

  /** @brief Longer budgets for serialized account/lifetime mutations, short polls otherwise. */
  std::chrono::milliseconds request_timeout(std::string_view request);

  /**
   * @brief Send one request and read one complete reply within a shared deadline.
   *
   * Connect, partial writes and partial reads remain nonblocking. A disconnected
   * peer cannot raise SIGPIPE. Partial/oversized replies are never accepted.
   * @param path Filesystem Unix socket path.
   * @param request Complete wire request.
   * @param timeout Total operation budget, including a full listen backlog.
   */
  exchange_result exchange(const std::string &path, const std::string &request, std::chrono::milliseconds timeout);
}  // namespace platf::session_broker::detail
