/**
 * @file tests/unit/test_media_session_lifetime.cpp
 * @brief Regression coverage for media packet/session ownership.
 */

#include <gtest/gtest.h>

#include "src/media_session_lifetime.h"
#include "src/audio.h"
#include "src/thread_safe.h"
#include "src/video.h"

#include <memory>

TEST(MediaSessionLifetimeTest, QueuedVideoPacketRetainsChannelOwner) {
  auto owner = std::make_shared<int>(42);
  std::weak_ptr<int> observed = owner;
  auto *channel_key = owner.get();
  stream::lifetime::register_channel(channel_key, owner);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto queue = mail->queue<video::packet_t>(
    "media-session-lifetime-test"
  );
  auto packet = std::make_unique<video::packet_raw_generic>(
    std::vector<std::uint8_t> {1, 2, 3},
    7,
    false
  );
  packet->channel_data = channel_key;
  video::enqueue_video_packet(queue, std::move(packet));

  owner.reset();
  stream::lifetime::unregister_channel(channel_key);
  EXPECT_FALSE(observed.expired());

  auto queued = queue->pop();
  ASSERT_TRUE(queued);
  EXPECT_FALSE(observed.expired());
  queued.reset();
  EXPECT_TRUE(observed.expired());
  EXPECT_EQ(stream::lifetime::registered_channels(), 0);
}

TEST(MediaSessionLifetimeTest, RegistryDoesNotOwnIdleChannel) {
  auto owner = std::make_shared<int>(9);
  std::weak_ptr<int> observed = owner;
  auto *channel_key = owner.get();
  stream::lifetime::register_channel(channel_key, owner);

  owner.reset();
  EXPECT_TRUE(observed.expired());
  EXPECT_FALSE(stream::lifetime::retain_channel(channel_key));
  EXPECT_EQ(stream::lifetime::registered_channels(), 0);
}

TEST(MediaSessionLifetimeTest, QueuedAudioPacketRetainsChannelOwner) {
  auto owner = std::make_shared<int>(27);
  std::weak_ptr<int> observed = owner;
  auto *channel_key = owner.get();
  stream::lifetime::register_channel(channel_key, owner);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto queue = mail->queue<audio::packet_t>(
    "audio-session-lifetime-test"
  );
  audio::enqueue_packet(
    queue,
    channel_key,
    audio::buffer_t {64}
  );

  owner.reset();
  stream::lifetime::unregister_channel(channel_key);
  EXPECT_FALSE(observed.expired());

  auto queued = queue->pop();
  ASSERT_TRUE(queued);
  EXPECT_FALSE(observed.expired());
  queued.reset();
  EXPECT_TRUE(observed.expired());
  EXPECT_EQ(stream::lifetime::registered_channels(), 0);
}

// Regression: retention can fail when a session ends between encode and
// enqueue. Queueing anyway left the broadcaster dereferencing a dead session.
TEST(MediaSessionLifetimeTest, VideoPacketForEndedSessionIsDiscarded) {
  auto owner = std::make_shared<int>(7);
  auto *channel_key = owner.get();
  stream::lifetime::register_channel(channel_key, owner);
  // The session ends before the encoded frame reaches the queue.
  owner.reset();

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto queue = mail->queue<video::packet_t>(
    "media-session-lifetime-expired-video"
  );
  auto packet = std::make_unique<video::packet_raw_generic>(
    std::vector<std::uint8_t> {1, 2, 3},
    9,
    false
  );
  packet->channel_data = channel_key;
  video::enqueue_video_packet(queue, std::move(packet));

  EXPECT_FALSE(queue->peek());
  stream::lifetime::unregister_channel(channel_key);
}

TEST(MediaSessionLifetimeTest, AudioPacketForEndedSessionIsDiscarded) {
  auto owner = std::make_shared<int>(11);
  auto *channel_key = owner.get();
  stream::lifetime::register_channel(channel_key, owner);
  owner.reset();

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto queue = mail->queue<audio::packet_t>(
    "media-session-lifetime-expired-audio"
  );
  audio::enqueue_packet(queue, channel_key, audio::buffer_t {});

  EXPECT_FALSE(queue->peek());
  stream::lifetime::unregister_channel(channel_key);
}

// A packet with no channel at all is not session-scoped and must still flow.
TEST(MediaSessionLifetimeTest, PacketWithoutChannelIsStillQueued) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto queue = mail->queue<video::packet_t>(
    "media-session-lifetime-no-channel"
  );
  auto packet = std::make_unique<video::packet_raw_generic>(
    std::vector<std::uint8_t> {4, 5},
    3,
    false
  );
  packet->channel_data = nullptr;
  video::enqueue_video_packet(queue, std::move(packet));

  EXPECT_TRUE(queue->peek());
}
