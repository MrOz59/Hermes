/**
 * @file tests/unit/test_protocol_extensions.cpp
 * @brief Tests for Hermes protocol extension negotiation.
 */

#include "../tests_common.h"

#include <src/protocol_extensions.h>

namespace {

  protocol::ext::extension_t first_supported() {
    const auto supported = protocol::ext::supported();
    EXPECT_FALSE(supported.empty());
    return supported.front();
  }

}  // namespace

TEST(ProtocolExtensionsTest, RegistryEntriesAreUsable) {
  for (const auto &extension : protocol::ext::supported()) {
    EXPECT_FALSE(extension.name.empty());
    EXPECT_FALSE(extension.summary.empty());
    // Version 0 would be indistinguishable from "not negotiated".
    EXPECT_GT(extension.version, 0u);
  }
}

// Every client built before extensions existed announces nothing, and must get
// a working session out of it.
TEST(ProtocolExtensionsTest, NoAnnouncementNegotiatesNothing) {
  const auto negotiated = protocol::ext::negotiate({});

  EXPECT_TRUE(negotiated.empty());
  EXPECT_FALSE(negotiated.contains("congestion_report"));
  EXPECT_EQ(negotiated.version_of("congestion_report"), 0u);
}

TEST(ProtocolExtensionsTest, MatchingNameAndVersionIsNegotiated) {
  const auto supported = first_supported();
  const std::vector<protocol::ext::announcement_t> announced {
    {.name = std::string {supported.name}, .version = supported.version},
  };

  const auto negotiated = protocol::ext::negotiate(announced);

  EXPECT_FALSE(negotiated.empty());
  EXPECT_TRUE(negotiated.contains(supported.name));
  EXPECT_EQ(negotiated.version_of(supported.name), supported.version);
}

// A client is free to announce what a future host will understand. Rejecting
// the session over it would make every host upgrade a flag day.
TEST(ProtocolExtensionsTest, UnknownExtensionsAreDroppedNotRejected) {
  const auto supported = first_supported();
  const std::vector<protocol::ext::announcement_t> announced {
    {.name = "extension_from_a_later_hermes", .version = 3},
    {.name = std::string {supported.name}, .version = supported.version},
  };

  const auto negotiated = protocol::ext::negotiate(announced);

  EXPECT_FALSE(negotiated.contains("extension_from_a_later_hermes"));
  EXPECT_TRUE(negotiated.contains(supported.name));
}

// Same name, version this build does not implement: the extension is off, but
// the rest of the announcement still stands.
TEST(ProtocolExtensionsTest, UnimplementedVersionsAreDropped) {
  const auto supported = first_supported();
  const std::vector<protocol::ext::announcement_t> announced {
    {.name = std::string {supported.name}, .version = supported.version + 100},
  };

  const auto negotiated = protocol::ext::negotiate(announced);

  EXPECT_TRUE(negotiated.empty());
  EXPECT_EQ(negotiated.version_of(supported.name), 0u);
}

// Versions are independent, so a client that speaks several of them lets
// either side be upgraded first. The highest both know wins.
TEST(ProtocolExtensionsTest, SettlesOnTheHighestCommonVersion) {
  const auto supported = first_supported();
  const std::vector<protocol::ext::announcement_t> announced {
    {.name = std::string {supported.name}, .version = supported.version + 1},
    {.name = std::string {supported.name}, .version = supported.version},
  };

  const auto negotiated = protocol::ext::negotiate(announced);

  EXPECT_EQ(negotiated.version_of(supported.name), supported.version);
}

TEST(ProtocolExtensionsTest, RepeatedAnnouncementOfOneVersionIsStable) {
  const auto supported = first_supported();
  const std::vector<protocol::ext::announcement_t> announced {
    {.name = std::string {supported.name}, .version = supported.version},
    {.name = std::string {supported.name}, .version = supported.version},
  };

  const auto negotiated = protocol::ext::negotiate(announced);

  EXPECT_EQ(negotiated.entries().size(), 1u);
  EXPECT_EQ(negotiated.version_of(supported.name), supported.version);
}
