/**
 * @file src/transport.h
 * @brief Injectable transport boundary for GameStream media and control data.
 */
#pragma once

#include "network.h"
#include "platform/common.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace stream::transport {

  /**
   * @brief Outcome of one transport operation.
   *
   * A batch backend reports fallback_required when the caller must preserve
   * the legacy behavior by retrying each datagram individually.
   */
  enum class send_status_e {
    sent,
    failed,
    fallback_required,
  };

  struct send_result_t {
    send_status_e status = send_status_e::failed;
    std::size_t message_count = 0;
    std::size_t bytes_sent = 0;

    [[nodiscard]] constexpr bool succeeded() const noexcept {
      return status == send_status_e::sent;
    }

    [[nodiscard]] constexpr bool requires_fallback() const noexcept {
      return status == send_status_e::fallback_required;
    }
  };

  struct transport_stats_t {
    std::uint64_t datagrams_sent = 0;
    std::uint64_t reliable_messages_sent = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t send_failures = 0;
    std::uint64_t batch_fallbacks = 0;
  };

  using datagram_view_t = platf::send_info_t;
  using datagram_batch_view_t = platf::batched_send_info_t;

  /**
   * @brief Reliable channel used by a transport.
   *
   * GameStream implements this boundary with its existing ENet control
   * server. Keeping the channel separate also makes reliable sends
   * deterministic in unit tests.
   */
  class IReliableChannel {
  public:
    virtual ~IReliableChannel() = default;

    [[nodiscard]] virtual bool send_reliable(
      const std::string_view &payload,
      net::peer_t peer
    ) = 0;
  };

  struct reliable_message_t {
    std::string_view payload;
    IReliableChannel &channel;
    net::peer_t peer;
  };

  /**
   * @brief H1 transport seam shared by media and control broadcasters.
   */
  class ITransport {
  public:
    virtual ~ITransport() = default;

    [[nodiscard]] virtual send_result_t send_datagram(
      datagram_view_t &datagram
    ) = 0;
    [[nodiscard]] virtual send_result_t send_datagram_batch(
      datagram_batch_view_t &batch
    ) = 0;
    [[nodiscard]] virtual send_result_t send_reliable(
      const reliable_message_t &message
    ) = 0;
    [[nodiscard]] virtual transport_stats_t stats() const noexcept = 0;
  };

  /**
   * @brief Injectable adapter around the platform UDP send primitives.
   */
  class IDatagramSender {
  public:
    virtual ~IDatagramSender() = default;

    [[nodiscard]] virtual bool send_datagram(
      datagram_view_t &datagram
    ) = 0;
    [[nodiscard]] virtual bool send_datagram_batch(
      datagram_batch_view_t &batch
    ) = 0;
  };

  /**
   * @brief Production GameStream transport preserving the legacy wire path.
   */
  class game_stream_transport_t final: public ITransport {
  public:
    game_stream_transport_t();
    explicit game_stream_transport_t(
      IDatagramSender &datagram_sender
    ) noexcept;
    ~game_stream_transport_t() override;

    game_stream_transport_t(const game_stream_transport_t &) = delete;
    game_stream_transport_t &operator=(
      const game_stream_transport_t &
    ) = delete;

    [[nodiscard]] send_result_t send_datagram(
      datagram_view_t &datagram
    ) override;
    [[nodiscard]] send_result_t send_datagram_batch(
      datagram_batch_view_t &batch
    ) override;
    [[nodiscard]] send_result_t send_reliable(
      const reliable_message_t &message
    ) override;
    [[nodiscard]] transport_stats_t stats() const noexcept override;

  private:
    std::unique_ptr<IDatagramSender> owned_datagram_sender_;
    IDatagramSender &datagram_sender_;

    std::atomic_uint64_t datagrams_sent_ = 0;
    std::atomic_uint64_t reliable_messages_sent_ = 0;
    std::atomic_uint64_t bytes_sent_ = 0;
    std::atomic_uint64_t send_failures_ = 0;
    std::atomic_uint64_t batch_fallbacks_ = 0;
  };

}  // namespace stream::transport
