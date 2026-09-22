/**
 * @file tests/unit/test_client_link_monitor.cpp
 * @brief A client going silent on the control connection, and what it explains.
 */
#include "../tests_common.h"

#include <src/client_link_monitor.h>

namespace {
  using namespace std::chrono_literals;
  using stream::client_link_monitor_t;
  using clock_t_ = client_link_monitor_t::clock;

  /// Messages every `step` from `start` until `end`, exclusive.
  void messages(client_link_monitor_t &link, clock_t_::time_point start, clock_t_::time_point end, std::chrono::milliseconds step) {
    for (auto t = start; t < end; t += step) {
      EXPECT_FALSE(link.on_message(t)) << "no silence expected inside a steady run";
    }
  }
}  // namespace

TEST(ClientLinkMonitor, SteadyPingsAreNotSilences) {
  client_link_monitor_t link;
  const auto t0 = clock_t_::time_point {};
  link.on_periodic_ping();
  // Pings every 100 ms, with an occasional slow one just under the threshold.
  messages(link, t0, t0 + 2s, 100ms);  // the last one at 1.9 s
  EXPECT_FALSE(link.on_message(t0 + 1900ms + 149ms));

  EXPECT_EQ(link.silence_count(), 0u);
}

TEST(ClientLinkMonitor, ThresholdIsInclusive) {
  client_link_monitor_t link;
  const auto t0 = clock_t_::time_point {};
  link.on_periodic_ping();
  link.on_message(t0);

  const auto silence = link.on_message(t0 + client_link_monitor_t::silence_threshold);

  ASSERT_TRUE(silence);
  EXPECT_EQ(*silence, client_link_monitor_t::silence_threshold);
}

TEST(ClientLinkMonitor, ReportsTheSilenceTheNextMessageEnds) {
  client_link_monitor_t link;
  const auto t0 = clock_t_::time_point {};
  link.on_periodic_ping();
  link.on_message(t0);

  // The pattern from a real HDR session: 657 ms without a single message.
  const auto silence = link.on_message(t0 + 657ms);

  ASSERT_TRUE(silence);
  EXPECT_EQ(*silence, 657ms);
  EXPECT_EQ(link.silence_count(), 1u);
}

TEST(ClientLinkMonitor, ClientWithoutPeriodicPingsIsNeverReported) {
  client_link_monitor_t link;
  const auto t0 = clock_t_::time_point {};
  // A client that sends only input stays quiet while nobody touches anything.
  link.on_message(t0);
  EXPECT_FALSE(link.on_message(t0 + 5s));
  EXPECT_EQ(link.silence_count(), 0u);

  // Once it has shown it pings, the same gap counts.
  link.on_periodic_ping();
  EXPECT_TRUE(link.on_message(t0 + 10s));
}

TEST(ClientLinkMonitor, KeyFrameRequestRightAfterASilenceIsAttributedToIt) {
  client_link_monitor_t link;
  const auto t0 = clock_t_::time_point {};
  link.on_periodic_ping();
  link.on_message(t0);
  link.on_message(t0 + 600ms);

  // The request arrives 18 ms after the client came back.
  const auto silence = link.silence_before(t0 + 618ms);
  ASSERT_TRUE(silence);
  EXPECT_EQ(*silence, 600ms);
  EXPECT_EQ(link.since_last_silence(t0 + 618ms), 18ms);

  // Long after, a request has some other cause.
  EXPECT_FALSE(link.silence_before(t0 + 600ms + client_link_monitor_t::attribution_window + 1ms));
}

TEST(ClientLinkMonitor, NoSilenceMeansNoAttribution) {
  client_link_monitor_t link;
  const auto t0 = clock_t_::time_point {};
  link.on_periodic_ping();
  messages(link, t0, t0 + 1s, 100ms);

  EXPECT_FALSE(link.silence_before(t0 + 1s));
  EXPECT_EQ(link.since_last_silence(t0 + 1s), std::chrono::milliseconds::max());
}

TEST(ClientLinkMonitor, TotalsAcrossRepeatedSilences) {
  client_link_monitor_t link;
  const auto t0 = clock_t_::time_point {};
  link.on_periodic_ping();
  link.on_message(t0);

  // A Wi-Fi scan's rhythm: ~600 ms away, ~100 ms back, several times over.
  auto t = t0;
  for (const auto away : {588ms, 625ms, 560ms}) {
    t += away;
    ASSERT_TRUE(link.on_message(t));
    messages(link, t + 50ms, t + 150ms, 50ms);
    t += 100ms;
    link.on_message(t);
  }

  EXPECT_EQ(link.silence_count(), 3u);
  EXPECT_EQ(link.longest_silence(), 625ms);
  EXPECT_EQ(link.total_silence(), 588ms + 625ms + 560ms);
}
