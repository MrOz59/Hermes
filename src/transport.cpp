/**
 * @file src/transport.cpp
 * @brief GameStream implementation of the injectable transport boundary.
 */

#include "transport.h"

#include <memory>

namespace stream::transport {

  namespace {

    class platform_datagram_sender_t final: public IDatagramSender {
    public:
      [[nodiscard]] bool send_datagram(
        datagram_view_t &datagram
      ) override {
        return platf::send(datagram);
      }

      [[nodiscard]] bool send_datagram_batch(
        datagram_batch_view_t &batch
      ) override {
        return platf::send_batch(batch);
      }
    };

  }  // namespace

  game_stream_transport_t::game_stream_transport_t():
      owned_datagram_sender_ {
        std::make_unique<platform_datagram_sender_t>()
      },
      datagram_sender_ {*owned_datagram_sender_} {
  }

  game_stream_transport_t::game_stream_transport_t(
    IDatagramSender &datagram_sender
  ) noexcept:
      datagram_sender_ {datagram_sender} {
  }

  game_stream_transport_t::~game_stream_transport_t() = default;

  send_result_t game_stream_transport_t::send_datagram(
    datagram_view_t &datagram
  ) {
    if (!datagram_sender_.send_datagram(datagram)) {
      send_failures_.fetch_add(1, std::memory_order_relaxed);
      return {};
    }

    const auto bytes = datagram.header_size + datagram.payload_size;
    datagrams_sent_.fetch_add(1, std::memory_order_relaxed);
    bytes_sent_.fetch_add(bytes, std::memory_order_relaxed);
    return {
      .status = send_status_e::sent,
      .message_count = 1,
      .bytes_sent = bytes,
    };
  }

  send_result_t game_stream_transport_t::send_datagram_batch(
    datagram_batch_view_t &batch
  ) {
    if (!datagram_sender_.send_datagram_batch(batch)) {
      // The legacy platform API deliberately combines "unsupported" and
      // "failed" into false. Its caller has always retried the batch as
      // individual datagrams, so this is a typed fallback rather than a
      // terminal send failure.
      batch_fallbacks_.fetch_add(1, std::memory_order_relaxed);
      return {
        .status = send_status_e::fallback_required,
      };
    }

    const auto bytes_per_message = batch.header_size + batch.payload_size;
    const auto bytes = bytes_per_message * batch.block_count;
    datagrams_sent_.fetch_add(
      batch.block_count,
      std::memory_order_relaxed
    );
    bytes_sent_.fetch_add(bytes, std::memory_order_relaxed);
    return {
      .status = send_status_e::sent,
      .message_count = batch.block_count,
      .bytes_sent = bytes,
    };
  }

  send_result_t game_stream_transport_t::send_reliable(
    const reliable_message_t &message
  ) {
    if (!message.channel.send_reliable(message.payload, message.peer)) {
      send_failures_.fetch_add(1, std::memory_order_relaxed);
      return {};
    }

    reliable_messages_sent_.fetch_add(1, std::memory_order_relaxed);
    bytes_sent_.fetch_add(
      message.payload.size(),
      std::memory_order_relaxed
    );
    return {
      .status = send_status_e::sent,
      .message_count = 1,
      .bytes_sent = message.payload.size(),
    };
  }

  transport_stats_t game_stream_transport_t::stats() const noexcept {
    return {
      .datagrams_sent =
        datagrams_sent_.load(std::memory_order_relaxed),
      .reliable_messages_sent =
        reliable_messages_sent_.load(std::memory_order_relaxed),
      .bytes_sent = bytes_sent_.load(std::memory_order_relaxed),
      .send_failures =
        send_failures_.load(std::memory_order_relaxed),
      .batch_fallbacks =
        batch_fallbacks_.load(std::memory_order_relaxed),
    };
  }

}  // namespace stream::transport
