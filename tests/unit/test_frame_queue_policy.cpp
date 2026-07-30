/**
 * @file tests/unit/test_frame_queue_policy.cpp
 * @brief Deterministic tests for Hermes H2 encoded-frame queue protection.
 */

#include "../tests_common.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <src/frame_queue_policy.h>
#include <thread>

using namespace std::literals;

namespace {

  const void *session_key(std::uintptr_t value) {
    return reinterpret_cast<const void *>(value);
  }

}  // namespace

TEST(FrameQueuePolicyTest, SendsFreshFramesWithTypedPriority) {
  stream::queueing::frame_queue_policy_t policy;
  const auto now =
    stream::queueing::frame_queue_time_point_t {} + 20ms;

  const auto normal = policy.evaluate({
    .session_key = session_key(1),
    .is_idr = false,
    .encoded_at = now - 5ms,
    .max_queue_time = 30ms,
    .now = now,
  });
  const auto reference = policy.evaluate({
    .session_key = session_key(1),
    .is_idr = true,
    .encoded_at = now - 4ms,
    .max_queue_time = 30ms,
    .now = now,
  });

  EXPECT_TRUE(normal.should_send());
  EXPECT_EQ(
    normal.priority,
    stream::priority::media_priority_e::video_normal
  );
  EXPECT_EQ(normal.queue_time, 5ms);
  EXPECT_TRUE(reference.should_send());
  EXPECT_EQ(
    reference.priority,
    stream::priority::media_priority_e::video_reference
  );
}

TEST(FrameQueuePolicyTest, ExpiredFrameStartsIdrRecovery) {
  stream::queueing::frame_queue_policy_t policy;
  const auto now =
    stream::queueing::frame_queue_time_point_t {} + 50ms;

  const auto expired = policy.evaluate({
    .session_key = session_key(1),
    .is_idr = false,
    .encoded_at = now - 40ms,
    .max_queue_time = 30ms,
    .now = now,
  });
  const auto dependent = policy.evaluate({
    .session_key = session_key(1),
    .is_idr = false,
    .encoded_at = now,
    .max_queue_time = 30ms,
    .now = now,
  });

  EXPECT_FALSE(expired.should_send());
  EXPECT_EQ(
    expired.drop_reason,
    stream::queueing::frame_queue_drop_reason_e::deadline_expired
  );
  EXPECT_EQ(expired.queue_time, 40ms);
  EXPECT_TRUE(expired.request_idr);

  EXPECT_FALSE(dependent.should_send());
  EXPECT_EQ(
    dependent.drop_reason,
    stream::queueing::frame_queue_drop_reason_e::awaiting_recovery
  );
  EXPECT_FALSE(dependent.request_idr);
}

TEST(FrameQueuePolicyTest, FreshIdrEndsRecovery) {
  stream::queueing::frame_queue_policy_t policy;
  const auto now =
    stream::queueing::frame_queue_time_point_t {} + 50ms;

  static_cast<void>(policy.evaluate({
    .session_key = session_key(1),
    .encoded_at = now - 40ms,
    .max_queue_time = 30ms,
    .now = now,
  }));
  const auto idr = policy.evaluate({
    .session_key = session_key(1),
    .is_idr = true,
    .encoded_at = now - 2ms,
    .max_queue_time = 30ms,
    .now = now,
  });
  const auto following = policy.evaluate({
    .session_key = session_key(1),
    .encoded_at = now,
    .max_queue_time = 30ms,
    .now = now,
  });

  EXPECT_TRUE(idr.should_send());
  EXPECT_TRUE(following.should_send());
}

TEST(FrameQueuePolicyTest, ExpiredIdrRequestsAnotherRecoveryFrame) {
  stream::queueing::frame_queue_policy_t policy;
  const auto now =
    stream::queueing::frame_queue_time_point_t {} + 50ms;

  const auto idr = policy.evaluate({
    .session_key = session_key(1),
    .is_idr = true,
    .encoded_at = now - 40ms,
    .max_queue_time = 30ms,
    .now = now,
  });

  EXPECT_FALSE(idr.should_send());
  EXPECT_EQ(
    idr.drop_reason,
    stream::queueing::frame_queue_drop_reason_e::deadline_expired
  );
  EXPECT_TRUE(idr.request_idr);
}

TEST(FrameQueuePolicyTest, RateLimitsExternalIdrRequestsPerSession) {
  stream::queueing::frame_queue_policy_t policy;
  const auto now =
    stream::queueing::frame_queue_time_point_t {} + 1s;

  EXPECT_TRUE(policy.allow_idr_request(
    session_key(1),
    now
  ));
  EXPECT_FALSE(policy.allow_idr_request(
    session_key(1),
    now + 99ms
  ));
  EXPECT_TRUE(policy.allow_idr_request(
    session_key(1),
    now + 100ms
  ));
  EXPECT_TRUE(policy.allow_idr_request(
    session_key(2),
    now
  ));
}

TEST(FrameQueuePolicyTest, ConcurrentIdrRequestsAcceptOnePerWindow) {
  stream::queueing::frame_queue_policy_t policy;
  const auto now =
    stream::queueing::frame_queue_time_point_t {} + 1s;
  std::atomic<int> accepted {0};
  std::array<std::thread, 8> workers;

  for (auto &worker : workers) {
    worker = std::thread([&]() {
      if (policy.allow_idr_request(session_key(1), now)) {
        ++accepted;
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }

  EXPECT_EQ(accepted.load(), 1);
}

TEST(FrameQueuePolicyTest, ExternalRequestSatisfiesPendingRecovery) {
  stream::queueing::frame_queue_policy_t policy;
  const auto key = session_key(1);
  const auto now =
    stream::queueing::frame_queue_time_point_t {} + 1s;

  policy.mark_recovery_required(key);
  ASSERT_TRUE(policy.allow_idr_request(key, now));
  const auto dependent = policy.evaluate({
    .session_key = key,
    .encoded_at = now,
    .max_queue_time = 30ms,
    .now = now,
  });

  EXPECT_FALSE(dependent.should_send());
  EXPECT_FALSE(dependent.request_idr);
  EXPECT_FALSE(dependent.idr_request_rate_limited);
}

TEST(FrameQueuePolicyTest, DeferredRecoveryRetriesWhenGateOpens) {
  stream::queueing::frame_queue_policy_t policy;
  const auto key = session_key(1);
  const auto now =
    stream::queueing::frame_queue_time_point_t {} + 1s;

  ASSERT_TRUE(policy.allow_idr_request(key, now));
  policy.mark_recovery_required(key);

  const auto limited = policy.evaluate({
    .session_key = key,
    .encoded_at = now + 50ms,
    .max_queue_time = 30ms,
    .now = now + 50ms,
  });
  const auto retried = policy.evaluate({
    .session_key = key,
    .encoded_at = now + 100ms,
    .max_queue_time = 30ms,
    .now = now + 100ms,
  });

  EXPECT_FALSE(limited.should_send());
  EXPECT_FALSE(limited.request_idr);
  EXPECT_TRUE(limited.idr_request_rate_limited);
  EXPECT_FALSE(retried.should_send());
  EXPECT_TRUE(retried.request_idr);
  EXPECT_FALSE(retried.idr_request_rate_limited);
}

TEST(FrameQueuePolicyTest, PacketDeadlineCauseSurvivesDeferredRetry) {
  stream::queueing::frame_queue_policy_t policy;
  const auto key = session_key(1);
  const auto now =
    stream::queueing::frame_queue_time_point_t {} + 1s;

  ASSERT_TRUE(policy.allow_idr_request(key, now));
  policy.mark_recovery_required(
    key,
    stream::queueing::frame_recovery_cause_e::
      packet_deadline_expired
  );
  const auto limited = policy.evaluate({
    .session_key = key,
    .now = now + 50ms,
  });
  const auto retried = policy.evaluate({
    .session_key = key,
    .now = now + 100ms,
  });

  EXPECT_EQ(
    limited.recovery_cause,
    stream::queueing::frame_recovery_cause_e::
      packet_deadline_expired
  );
  EXPECT_TRUE(limited.idr_request_rate_limited);
  EXPECT_TRUE(retried.request_idr);
  EXPECT_EQ(
    retried.recovery_cause,
    stream::queueing::frame_recovery_cause_e::
      packet_deadline_expired
  );
}

TEST(FrameQueuePolicyTest, ExpiredReplacementIdrKeepsDeferredRetry) {
  stream::queueing::frame_queue_policy_t policy;
  const auto key = session_key(1);
  const auto now =
    stream::queueing::frame_queue_time_point_t {} + 1s;

  ASSERT_TRUE(policy.allow_idr_request(key, now));
  const auto expired_idr = policy.evaluate({
    .session_key = key,
    .is_idr = true,
    .encoded_at = now,
    .max_queue_time = 30ms,
    .now = now + 40ms,
  });
  const auto retried = policy.evaluate({
    .session_key = key,
    .encoded_at = now + 100ms,
    .max_queue_time = 30ms,
    .now = now + 100ms,
  });

  EXPECT_FALSE(expired_idr.should_send());
  EXPECT_FALSE(expired_idr.request_idr);
  EXPECT_TRUE(expired_idr.idr_request_rate_limited);
  EXPECT_FALSE(retried.should_send());
  EXPECT_TRUE(retried.request_idr);
}

TEST(FrameQueuePolicyTest, ZeroBudgetDisablesDeadline) {
  stream::queueing::frame_queue_policy_t policy;
  const auto now =
    stream::queueing::frame_queue_time_point_t {} + 5s;

  const auto decision = policy.evaluate({
    .session_key = session_key(1),
    .encoded_at = now - 4s,
    .max_queue_time = 0us,
    .now = now,
  });

  EXPECT_TRUE(decision.should_send());
  EXPECT_EQ(decision.queue_time, 4s);
}

TEST(FrameQueuePolicyTest, BoundsStateAcrossSessionChurn) {
  stream::queueing::frame_queue_policy_t policy;

  for (
    std::size_t index = 0;
    index < stream::queueing::frame_queue_policy_t::max_sessions + 5;
    ++index) {
    EXPECT_TRUE(policy.evaluate({
                                  .session_key = session_key(index + 1),
                                })
                  .should_send());
  }

  EXPECT_EQ(
    policy.active_sessions(),
    stream::queueing::frame_queue_policy_t::max_sessions
  );
}

TEST(FrameQueuePolicyTest, QueueOverflowRequestsOneIdrAndSuppressesDependents) {
  stream::queueing::frame_queue_policy_t policy;
  const auto key = session_key(1);

  policy.mark_recovery_required(key);
  const auto first = policy.evaluate({
    .session_key = key,
  });
  const auto second = policy.evaluate({
    .session_key = key,
  });
  const auto idr = policy.evaluate({
    .session_key = key,
    .is_idr = true,
  });
  const auto recovered = policy.evaluate({
    .session_key = key,
  });

  EXPECT_FALSE(first.should_send());
  EXPECT_EQ(
    first.drop_reason,
    stream::queueing::frame_queue_drop_reason_e::awaiting_recovery
  );
  EXPECT_EQ(
    first.recovery_cause,
    stream::queueing::frame_recovery_cause_e::
      encoded_queue_overflow
  );
  EXPECT_TRUE(first.request_idr);
  EXPECT_FALSE(second.should_send());
  EXPECT_FALSE(second.request_idr);
  EXPECT_TRUE(idr.should_send());
  EXPECT_TRUE(recovered.should_send());
}

TEST(FrameQueuePolicyTest, ErasePreventsStateLeakAcrossSessionReuse) {
  stream::queueing::frame_queue_policy_t policy;
  const auto key = session_key(1);
  const auto now =
    stream::queueing::frame_queue_time_point_t {} + 1s;

  ASSERT_TRUE(policy.allow_idr_request(key, now));
  policy.mark_recovery_required(key);
  policy.erase(key);

  EXPECT_TRUE(policy.allow_idr_request(key, now));
  EXPECT_TRUE(policy.evaluate({
                                .session_key = key,
                              })
                .should_send());
  EXPECT_EQ(policy.active_sessions(), 1);
}

TEST(FrameQueuePolicyTest, ConcurrentOverflowMarksRemainBounded) {
  stream::queueing::frame_queue_policy_t policy;
  std::atomic<bool> valid {true};
  std::array<std::thread, 8> workers;

  for (std::size_t index = 0; index < workers.size(); ++index) {
    workers[index] = std::thread([&, index]() {
      const auto key = session_key(index + 1);
      for (int iteration = 0; iteration < 100; ++iteration) {
        policy.mark_recovery_required(key);
        const auto recovery = policy.evaluate({
          .session_key = key,
        });
        const auto idr = policy.evaluate({
          .session_key = key,
          .is_idr = true,
        });
        if (
          recovery.should_send() ||
          recovery.recovery_cause !=
            stream::queueing::frame_recovery_cause_e::
              encoded_queue_overflow ||
          !idr.should_send()
        ) {
          valid.store(false);
        }
      }
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }

  EXPECT_TRUE(valid.load());
  EXPECT_EQ(policy.active_sessions(), workers.size());
}

// Sequence numbers are stamped on every shard before the send loop runs, so a
// deadline abort must still consume the full block. Rewinding to the sent count
// would reuse those numbers for different payload in the next frame.
TEST(FrameQueuePolicyTest, PartialSendStillConsumesEverySequenceNumber) {
  using stream::queueing::sequence_numbers_consumed;

  EXPECT_EQ(sequence_numbers_consumed(40, 40), 40u);
  EXPECT_EQ(sequence_numbers_consumed(40, 10), 40u);
  EXPECT_EQ(sequence_numbers_consumed(40, 0), 40u);
  EXPECT_EQ(sequence_numbers_consumed(0, 0), 0u);
}
