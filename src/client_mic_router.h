/**
 * @file src/client_mic_router.h
 * @brief Route legacy encrypted client-microphone packets to one session.
 */
#pragma once

#include "crypto.h"
#include "utility.h"

#include <array>
#include <cstring>
#include <optional>
#include <opus/opus.h>
#include <span>
#include <vector>

namespace audio {
  struct mic_candidate_t {
    crypto::cipher::cbc_t *cipher;
    std::uint32_t key_id;
  };

  struct routed_mic_packet_t {
    std::size_t candidate;
    std::vector<std::uint8_t> opus;
  };

  // Several clients can share an address (NAT). A failed decryption for the
  // first session must not prevent trying the next one. The legacy CBC format
  // has no authenticated session ID, so require a valid Opus frame and reject
  // ambiguous matches instead of sending one client's voice to another.
  inline std::optional<routed_mic_packet_t> route_mic_packet(
    std::span<const mic_candidate_t> candidates,
    std::uint16_t sequence,
    std::string_view encrypted
  ) {
    if (encrypted.empty() || encrypted.size() > 2048 || encrypted.size() % 16 != 0) {
      return std::nullopt;
    }
    std::optional<routed_mic_packet_t> result;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      const auto &candidate = candidates[index];
      if (!candidate.cipher) {
        continue;
      }
      crypto::aes_t iv(16);
      const auto first = util::endian::big<std::uint32_t>(candidate.key_id + sequence);
      std::memcpy(iv.data(), &first, sizeof(first));
      std::vector<std::uint8_t> plaintext;
      if (candidate.cipher->decrypt(encrypted, plaintext, &iv) != 0 || plaintext.empty()) {
        continue;
      }
      const int samples = opus_packet_get_nb_samples(plaintext.data(), static_cast<opus_int32>(plaintext.size()), 48000);
      if (samples <= 0 || samples > 5760) {
        continue;
      }
      unsigned char toc;
      std::array<const unsigned char *, 48> frames;
      std::array<opus_int16, 48> sizes;
      int payload_offset;
      if (opus_packet_parse(plaintext.data(), static_cast<opus_int32>(plaintext.size()), &toc, frames.data(), sizes.data(), &payload_offset) <= 0) {
        continue;
      }
      if (result) {
        return std::nullopt;
      }
      result = routed_mic_packet_t {index, std::move(plaintext)};
    }
    return result;
  }
}  // namespace audio
