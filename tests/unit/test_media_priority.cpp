/**
 * @file tests/unit/test_media_priority.cpp
 * @brief Tests for explicit H2 GameStream priority classes.
 */

#include "../tests_common.h"

#include <src/media_priority.h>

TEST(MediaPriorityTest, OrdersCompatibilityClassesExplicitly) {
  using stream::priority::media_priority_e;
  using stream::priority::precedes;

  EXPECT_TRUE(precedes(
    media_priority_e::control,
    media_priority_e::audio
  ));
  EXPECT_TRUE(precedes(
    media_priority_e::audio,
    media_priority_e::video_reference
  ));
  EXPECT_TRUE(precedes(
    media_priority_e::video_reference,
    media_priority_e::video_normal
  ));
  EXPECT_TRUE(precedes(
    media_priority_e::video_normal,
    media_priority_e::fec
  ));
}

TEST(MediaPriorityTest, MapsIndependentWorkersToPlatformClasses) {
  using stream::priority::media_priority_e;
  using stream::priority::worker_priority;

  EXPECT_EQ(
    worker_priority(media_priority_e::control),
    platf::thread_priority_e::critical
  );
  EXPECT_EQ(
    worker_priority(media_priority_e::audio),
    platf::thread_priority_e::critical
  );
  EXPECT_EQ(
    worker_priority(media_priority_e::video_reference),
    platf::thread_priority_e::high
  );
  EXPECT_EQ(
    worker_priority(media_priority_e::video_normal),
    platf::thread_priority_e::high
  );
  EXPECT_EQ(
    worker_priority(media_priority_e::fec),
    platf::thread_priority_e::high
  );
}

TEST(MediaPriorityTest, ClassifiesVideoFrames) {
  EXPECT_EQ(
    stream::priority::video_frame_priority(true),
    stream::priority::media_priority_e::video_reference
  );
  EXPECT_EQ(
    stream::priority::video_frame_priority(false),
    stream::priority::media_priority_e::video_normal
  );
}
