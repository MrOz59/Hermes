/**
 * @file tests/unit/test_transport.cpp
 * @brief Deterministic tests for the injectable Hermes transport boundary.
 */

#include "../tests_common.h"

#include <cstdint>
#include <src/transport.h>
#include <vector>

namespace {

  class fake_datagram_sender_t final:
      public stream::transport::IDatagramSender {
  public:
    [[nodiscard]] bool send_datagram(
      stream::transport::datagram_view_t &datagram
    ) override {
      ++datagram_calls;
      last_datagram = &datagram;
      return datagram_result;
    }

    [[nodiscard]] bool send_datagram_batch(
      stream::transport::datagram_batch_view_t &batch
    ) override {
      ++batch_calls;
      last_batch = &batch;
      return batch_result;
    }

    bool datagram_result = true;
    bool batch_result = true;
    int datagram_calls = 0;
    int batch_calls = 0;
    stream::transport::datagram_view_t *last_datagram = nullptr;
    stream::transport::datagram_batch_view_t *last_batch = nullptr;
  };

  class fake_reliable_channel_t final:
      public stream::transport::IReliableChannel {
  public:
    [[nodiscard]] bool send_reliable(
      const std::string_view &payload,
      net::peer_t peer
    ) override {
      ++calls;
      last_payload = payload;
      last_peer = peer;
      return result;
    }

    bool result = true;
    int calls = 0;
    std::string_view last_payload;
    net::peer_t last_peer = nullptr;
  };

  class fake_transport_t final:
      public stream::transport::ITransport {
  public:
    [[nodiscard]] stream::transport::send_result_t send_datagram(
      stream::transport::datagram_view_t &
    ) override {
      ++datagram_calls;
      return datagram_result;
    }

    [[nodiscard]] stream::transport::send_result_t
      send_datagram_batch(
        stream::transport::datagram_batch_view_t &
      ) override {
      ++batch_calls;
      return batch_result;
    }

    [[nodiscard]] stream::transport::send_result_t send_reliable(
      const stream::transport::reliable_message_t &
    ) override {
      ++reliable_calls;
      return reliable_result;
    }

    [[nodiscard]] stream::transport::transport_stats_t
      stats() const noexcept override {
      return reported_stats;
    }

    stream::transport::send_result_t datagram_result;
    stream::transport::send_result_t batch_result;
    stream::transport::send_result_t reliable_result;
    stream::transport::transport_stats_t reported_stats;
    int datagram_calls = 0;
    int batch_calls = 0;
    int reliable_calls = 0;
  };

  struct datagram_fixture_t {
    boost::asio::ip::address target =
      boost::asio::ip::make_address("192.0.2.10");
    boost::asio::ip::address source =
      boost::asio::ip::make_address("192.0.2.1");
    std::vector<platf::buffer_descriptor_t> payloads {
      {"payload", 7},
    };
    platf::send_info_t datagram {
      "head",
      4,
      "payload",
      7,
      42,
      target,
      47998,
      source,
    };
    platf::batched_send_info_t batch {
      "headheadhead",
      4,
      payloads,
      7,
      0,
      3,
      42,
      target,
      47998,
      source,
    };
  };

}  // namespace

TEST(TransportTest, InterfaceSupportsFakeImplementation) {
  datagram_fixture_t fixture;
  fake_reliable_channel_t channel;
  fake_transport_t fake;
  fake.datagram_result = {
    .status = stream::transport::send_status_e::sent,
    .message_count = 1,
    .bytes_sent = 11,
  };
  fake.batch_result = {
    .status = stream::transport::send_status_e::fallback_required,
  };
  fake.reliable_result = {
    .status = stream::transport::send_status_e::sent,
    .message_count = 1,
    .bytes_sent = 7,
  };
  fake.reported_stats.datagrams_sent = 9;

  stream::transport::ITransport &transport = fake;
  EXPECT_TRUE(transport.send_datagram(fixture.datagram).succeeded());
  EXPECT_TRUE(
    transport.send_datagram_batch(fixture.batch).requires_fallback()
  );
  EXPECT_TRUE(
    transport.send_reliable({
                              .payload = "control",
                              .channel = channel,
                              .peer = nullptr,
                            })
      .succeeded()
  );

  EXPECT_EQ(transport.stats().datagrams_sent, 9);
  EXPECT_EQ(fake.datagram_calls, 1);
  EXPECT_EQ(fake.batch_calls, 1);
  EXPECT_EQ(fake.reliable_calls, 1);
}

TEST(TransportTest, GameStreamTransportPreservesDatagramViewsAndStats) {
  datagram_fixture_t fixture;
  fake_datagram_sender_t sender;
  stream::transport::game_stream_transport_t transport {sender};

  const auto result = transport.send_datagram(fixture.datagram);

  EXPECT_TRUE(result.succeeded());
  EXPECT_EQ(result.message_count, 1);
  EXPECT_EQ(result.bytes_sent, 11);
  EXPECT_EQ(sender.datagram_calls, 1);
  EXPECT_EQ(sender.last_datagram, &fixture.datagram);

  const auto stats = transport.stats();
  EXPECT_EQ(stats.datagrams_sent, 1);
  EXPECT_EQ(stats.reliable_messages_sent, 0);
  EXPECT_EQ(stats.bytes_sent, 11);
  EXPECT_EQ(stats.send_failures, 0);
  EXPECT_EQ(stats.batch_fallbacks, 0);
}

TEST(TransportTest, GameStreamTransportCountsSuccessfulBatch) {
  datagram_fixture_t fixture;
  fake_datagram_sender_t sender;
  stream::transport::game_stream_transport_t transport {sender};

  const auto result = transport.send_datagram_batch(fixture.batch);

  EXPECT_TRUE(result.succeeded());
  EXPECT_EQ(result.message_count, 3);
  EXPECT_EQ(result.bytes_sent, 33);
  EXPECT_EQ(sender.batch_calls, 1);
  EXPECT_EQ(sender.last_batch, &fixture.batch);

  const auto stats = transport.stats();
  EXPECT_EQ(stats.datagrams_sent, 3);
  EXPECT_EQ(stats.bytes_sent, 33);
  EXPECT_EQ(stats.batch_fallbacks, 0);
}

TEST(TransportTest, FailedBatchRequestsLegacyIndividualFallback) {
  datagram_fixture_t fixture;
  fake_datagram_sender_t sender;
  sender.batch_result = false;
  stream::transport::game_stream_transport_t transport {sender};

  const auto result = transport.send_datagram_batch(fixture.batch);

  EXPECT_TRUE(result.requires_fallback());
  EXPECT_EQ(result.message_count, 0);
  EXPECT_EQ(result.bytes_sent, 0);

  const auto stats = transport.stats();
  EXPECT_EQ(stats.datagrams_sent, 0);
  EXPECT_EQ(stats.bytes_sent, 0);
  EXPECT_EQ(stats.send_failures, 0);
  EXPECT_EQ(stats.batch_fallbacks, 1);
}

TEST(TransportTest, GameStreamTransportDelegatesReliableControl) {
  fake_datagram_sender_t sender;
  fake_reliable_channel_t channel;
  stream::transport::game_stream_transport_t transport {sender};
  auto peer = reinterpret_cast<net::peer_t>(
    static_cast<std::uintptr_t>(0x1234)
  );

  const auto result = transport.send_reliable({
    .payload = "encrypted-control",
    .channel = channel,
    .peer = peer,
  });

  EXPECT_TRUE(result.succeeded());
  EXPECT_EQ(result.message_count, 1);
  EXPECT_EQ(result.bytes_sent, 17);
  EXPECT_EQ(channel.calls, 1);
  EXPECT_EQ(channel.last_payload, "encrypted-control");
  EXPECT_EQ(channel.last_peer, peer);

  const auto stats = transport.stats();
  EXPECT_EQ(stats.reliable_messages_sent, 1);
  EXPECT_EQ(stats.bytes_sent, 17);
  EXPECT_EQ(stats.send_failures, 0);
}

TEST(TransportTest, FailedIndividualAndReliableSendsAreCounted) {
  datagram_fixture_t fixture;
  fake_datagram_sender_t sender;
  fake_reliable_channel_t channel;
  sender.datagram_result = false;
  channel.result = false;
  stream::transport::game_stream_transport_t transport {sender};

  EXPECT_EQ(
    transport.send_datagram(fixture.datagram).status,
    stream::transport::send_status_e::failed
  );
  EXPECT_EQ(
    transport.send_reliable({
                              .payload = "control",
                              .channel = channel,
                              .peer = nullptr,
                            })
      .status,
    stream::transport::send_status_e::failed
  );

  const auto stats = transport.stats();
  EXPECT_EQ(stats.datagrams_sent, 0);
  EXPECT_EQ(stats.reliable_messages_sent, 0);
  EXPECT_EQ(stats.bytes_sent, 0);
  EXPECT_EQ(stats.send_failures, 2);
}
