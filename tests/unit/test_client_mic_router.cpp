/**
 * @file test_client_mic_router.cpp
 * @brief Encrypted microphone ownership for clients sharing one address.
 */
#include <array>
#include <gtest/gtest.h>
#include <opus/opus.h>
#include <src/client_mic_router.h>

namespace {
  constexpr std::uint16_t sequence = 42;
  constexpr std::uint32_t key_id = 1234;

  std::vector<std::uint8_t> microphone_frame() {
    int error;
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder {
      opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error),
      opus_encoder_destroy
    };
    if (!encoder || error != OPUS_OK) {
      return {};
    }
    std::array<opus_int16, 960> silence {};
    std::vector<std::uint8_t> packet(1500);
    const int size = opus_encode(encoder.get(), silence.data(), silence.size(), packet.data(), packet.size());
    if (size <= 0) {
      return {};
    }
    packet.resize(size);
    return packet;
  }

  std::string encrypt(const crypto::aes_t &key, const std::vector<std::uint8_t> &packet, std::uint32_t id = key_id) {
    crypto::cipher::cbc_t encoder {key};
    crypto::aes_t iv(16);
    const auto first = util::endian::big<std::uint32_t>(id + sequence);
    std::memcpy(iv.data(), &first, sizeof(first));
    std::vector<std::uint8_t> ciphertext(packet.size() + 16);
    const int size = encoder.encrypt(
      std::string_view {reinterpret_cast<const char *>(packet.data()), packet.size()},
      ciphertext.data(),
      &iv
    );
    return size > 0 ? std::string {reinterpret_cast<const char *>(ciphertext.data()), static_cast<std::size_t>(size)} : std::string {};
  }

  TEST(ClientMicRouterTest, TriesTheSecondSessionAfterTheFirstKeyDoesNotDecrypt) {
    const crypto::aes_t key(16, 0x22);
    crypto::cipher::cbc_t wrong {crypto::aes_t(16, 0x11)};
    crypto::cipher::cbc_t right {key};
    const auto packet = microphone_frame();
    ASSERT_FALSE(packet.empty());
    const std::array<audio::mic_candidate_t, 2> sessions {{{&wrong, key_id}, {&right, key_id}}};
    const auto result = audio::route_mic_packet(sessions, sequence, encrypt(key, packet));
    ASSERT_TRUE(result);
    EXPECT_EQ(result->candidate, 1);
    EXPECT_EQ(result->opus, packet);
  }

  TEST(ClientMicRouterTest, RejectsAnAmbiguousMatchInsteadOfPickingAnotherSession) {
    const crypto::aes_t key(16, 0x22);
    crypto::cipher::cbc_t first {key};
    crypto::cipher::cbc_t second {key};
    const std::array<audio::mic_candidate_t, 2> sessions {{{&first, key_id}, {&second, key_id}}};
    EXPECT_FALSE(audio::route_mic_packet(sessions, sequence, encrypt(key, microphone_frame())));
  }

  TEST(ClientMicRouterTest, RejectsAnEmptyDecodedPacketAndMalformedCiphertext) {
    const crypto::aes_t key(16, 0x22);
    crypto::cipher::cbc_t decoder {key};
    const std::array<audio::mic_candidate_t, 1> sessions {{{&decoder, key_id}}};
    EXPECT_FALSE(audio::route_mic_packet(sessions, sequence, ""));
    EXPECT_FALSE(audio::route_mic_packet(sessions, sequence, "short packet"));
    EXPECT_FALSE(audio::route_mic_packet(sessions, sequence, std::string(4096, 'x')));
    EXPECT_FALSE(audio::route_mic_packet(sessions, sequence, encrypt(key, {})));
    // The TOC reports two equal frames, but the payload cannot be split.
    const std::vector<std::uint8_t> malformed_opus {0x01, 0x7f};
    ASSERT_GT(opus_packet_get_nb_samples(malformed_opus.data(), malformed_opus.size(), 48000), 0);
    EXPECT_FALSE(audio::route_mic_packet(sessions, sequence, encrypt(key, malformed_opus)));
  }

  TEST(ClientMicRouterTest, UsesTheCurrentStreamsKeyIdAfterReconnect) {
    const crypto::aes_t key(16, 0x22);
    crypto::cipher::cbc_t old_stream {key};
    crypto::cipher::cbc_t resumed {key};
    const std::array<audio::mic_candidate_t, 2> sessions {{{&old_stream, key_id}, {&resumed, key_id + 10000}}};
    const auto packet = microphone_frame();
    const auto result = audio::route_mic_packet(sessions, sequence, encrypt(key, packet, key_id + 10000));
    // CBC's first-block IV is not authenticated. If both plaintexts happen to
    // parse as Opus, routing must fail closed; it must never select the old key.
    EXPECT_TRUE(!result || result->candidate == 1);
  }
}  // namespace
