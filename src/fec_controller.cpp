/**
 * @file src/fec_controller.cpp
 * @brief Legacy Reed-Solomon implementation of the injectable FEC boundary.
 */

#include "fec_controller.h"

#include "logging.h"

extern "C" {
  // clang-format off
#include <third-party/nanors/rs.h>
#include "rswrapper.h"
  // clang-format on
}

#include <algorithm>
#include <cstring>

using namespace std::literals;

namespace stream::fec {
  namespace {

    using rs_t = util::safe_ptr<reed_solomon, [](reed_solomon *rs) {
      reed_solomon_release(rs);
    }>;

    class legacy_in_place_fec_encoder_t final:
        public IInPlaceFecEncoder {
    public:
      explicit legacy_in_place_fec_encoder_t(
        const in_place_encoder_config_t &config
      ):
          shard_count_ {config.data_shards + config.parity_shards},
          encoder_ {
            reed_solomon_new(
              static_cast<int>(config.data_shards),
              static_cast<int>(config.parity_shards)
            )
          } {
        if (encoder_ && !config.generator_matrix.empty()) {
          std::memcpy(
            encoder_->p,
            config.generator_matrix.data(),
            config.generator_matrix.size()
          );
        }
      }

      [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(encoder_);
      }

      int encode(
        std::span<uint8_t *> shards,
        std::size_t block_size
      ) override {
        if (!encoder_ || shards.size() != shard_count_) {
          return -1;
        }

        return reed_solomon_encode(
          encoder_.get(),
          shards.data(),
          static_cast<int>(shards.size()),
          static_cast<int>(block_size)
        );
      }

    private:
      std::size_t shard_count_ = 0;
      rs_t encoder_;
    };

    /**
     * @brief Data shards one block can hold at a given protection level.
     *
     * Solves D = 255 - P with P = D * F / 100.
     */
    std::size_t data_shards_per_block(
      std::size_t fec_percentage
    ) noexcept {
      return (maximum_shards_per_block * 100) / (100 + fec_percentage);
    }

    std::size_t blocks_needed(
      std::size_t data_shards,
      std::size_t fec_percentage
    ) noexcept {
      const auto per_block = data_shards_per_block(fec_percentage);
      return (data_shards + per_block - 1) / per_block;
    }

  }  // namespace

  frame_fec_plan_t plan_frame_fec(
    std::size_t payload_bytes,
    std::size_t block_size_bytes,
    std::size_t requested_fec_percentage
  ) noexcept {
    if (payload_bytes == 0 || block_size_bytes == 0) {
      return {
        .fec_percentage = requested_fec_percentage,
        .block_count = 1,
      };
    }

    const auto data_shards =
      (payload_bytes + block_size_bytes - 1) / block_size_bytes;
    const auto requested_blocks =
      blocks_needed(data_shards, requested_fec_percentage);
    if (requested_blocks <= maximum_fec_blocks) {
      return {
        .fec_percentage = requested_fec_percentage,
        .block_count = requested_blocks,
      };
    }

    // Spreading the frame over every available block leaves the most room per
    // block, so this is the smallest per-block shard count achievable.
    const auto shards_per_block =
      (data_shards + maximum_fec_blocks - 1) / maximum_fec_blocks;
    const auto fitted_percentage =
      shards_per_block > maximum_shards_per_block ?
        0 :
        (maximum_shards_per_block * 100) / shards_per_block - 100;
    if (fitted_percentage == 0) {
      // Not even the data shards fit: the frame goes out unprotected, and the
      // block count matches the pre-existing behaviour for this case.
      return {
        .fec_percentage = 0,
        .block_count = maximum_fec_blocks,
        .unprotected = true,
      };
    }

    return {
      .fec_percentage = fitted_percentage,
      .block_count = blocks_needed(data_shards, fitted_percentage),
      .reduced_to_fit = true,
    };
  }

  encoded_block_t::encoded_block_t(
    std::size_t data_shards,
    std::size_t shard_count,
    std::size_t fec_percentage,
    std::size_t block_size,
    std::size_t prefix_size,
    util::buffer_t<char> &&owned_shards,
    util::buffer_t<char> &&prefixes,
    util::buffer_t<uint8_t *> &&shard_pointers,
    std::vector<platf::buffer_descriptor_t> &&payload_buffers
  ):
      data_shards_ {data_shards},
      shard_count_ {shard_count},
      fec_percentage_ {fec_percentage},
      block_size_ {block_size},
      prefix_size_ {prefix_size},
      owned_shards_ {std::move(owned_shards)},
      prefixes_ {std::move(prefixes)},
      shard_pointers_ {std::move(shard_pointers)},
      payload_buffers_ {std::move(payload_buffers)} {
  }

  std::size_t encoded_block_t::data_shard_count() const noexcept {
    return data_shards_;
  }

  std::size_t encoded_block_t::size() const noexcept {
    return shard_count_;
  }

  std::size_t encoded_block_t::fec_percentage() const noexcept {
    return fec_percentage_;
  }

  std::size_t encoded_block_t::block_size() const noexcept {
    return block_size_;
  }

  std::size_t encoded_block_t::prefix_size() const noexcept {
    return prefix_size_;
  }

  char *encoded_block_t::data(std::size_t index) noexcept {
    return reinterpret_cast<char *>(shard_pointers_[index]);
  }

  const char *encoded_block_t::data(std::size_t index) const noexcept {
    return reinterpret_cast<const char *>(shard_pointers_[index]);
  }

  char *encoded_block_t::prefix(std::size_t index) noexcept {
    return prefix_size_ ? &prefixes_[index * prefix_size_] : nullptr;
  }

  const char *encoded_block_t::prefix(std::size_t index) const noexcept {
    return prefix_size_ ? &prefixes_[index * prefix_size_] : nullptr;
  }

  char *encoded_block_t::prefix_data() noexcept {
    return prefixes_.begin();
  }

  std::vector<platf::buffer_descriptor_t> &
    encoded_block_t::payload_buffers() noexcept {
    return payload_buffers_;
  }

  encoded_block_t legacy_reed_solomon_fec_controller_t::encode(
    const encode_request_t &request
  ) {
    const auto payload_size = request.payload.size();
    const auto has_padding = payload_size % request.block_size != 0;
    const auto aligned_data_shards = payload_size / request.block_size;
    const auto data_shards =
      aligned_data_shards + (has_padding ? 1 : 0);
    auto parity_shards =
      (data_shards * request.fec_percentage + 99) / 100;
    auto effective_fec_percentage = request.fec_percentage;

    if (
      parity_shards < request.minimum_parity_shards &&
      effective_fec_percentage != 0
    ) {
      parity_shards = request.minimum_parity_shards;
      effective_fec_percentage = (100 * parity_shards) / data_shards;

      BOOST_LOG(verbose)
        << "Increasing FEC percentage to "sv
        << effective_fec_percentage
        << " to meet parity shard minimum"sv << std::endl;
    }

    const auto shard_count = data_shards + parity_shards;

    // Keep the zero-padded final data shard before parity storage so the
    // descriptors and shard order remain byte-for-byte compatible.
    const auto parity_shard_offset = has_padding ? 1 : 0;
    util::buffer_t<char> owned_shards {
      (parity_shard_offset + parity_shards) * request.block_size
    };
    util::buffer_t<uint8_t *> shard_pointers {shard_count};
    std::vector<platf::buffer_descriptor_t> payload_buffers;
    payload_buffers.reserve(2);

    auto next = std::begin(request.payload);
    for (std::size_t index = 0; index < aligned_data_shards; ++index) {
      shard_pointers[index] =
        reinterpret_cast<uint8_t *>(const_cast<char *>(next));
      next += request.block_size;
    }
    payload_buffers.emplace_back(
      std::begin(request.payload),
      aligned_data_shards * request.block_size
    );

    if (has_padding) {
      shard_pointers[aligned_data_shards] =
        reinterpret_cast<uint8_t *>(&owned_shards[0]);

      const auto copy_length = std::min<std::size_t>(
        request.block_size,
        std::end(request.payload) - next
      );
      std::memcpy(
        shard_pointers[aligned_data_shards],
        next,
        copy_length
      );
      if (copy_length < request.block_size) {
        std::memset(
          shard_pointers[aligned_data_shards] + copy_length,
          0,
          request.block_size - copy_length
        );
      }
    }

    payload_buffers.emplace_back(
      std::begin(owned_shards),
      owned_shards.size()
    );

    if (effective_fec_percentage != 0) {
      for (std::size_t index = 0; index < parity_shards; ++index) {
        shard_pointers[data_shards + index] =
          reinterpret_cast<uint8_t *>(
            &owned_shards[(parity_shard_offset + index) * request.block_size]
          );
      }

      rs_t encoder {
        reed_solomon_new(
          static_cast<int>(data_shards),
          static_cast<int>(parity_shards)
        )
      };
      reed_solomon_encode(
        encoder.get(),
        shard_pointers.begin(),
        static_cast<int>(shard_count),
        static_cast<int>(request.block_size)
      );
    }

    return {
      data_shards,
      shard_count,
      effective_fec_percentage,
      request.block_size,
      request.prefix_size,
      std::move(owned_shards),
      util::buffer_t<char> {shard_count * request.prefix_size},
      std::move(shard_pointers),
      std::move(payload_buffers),
    };
  }

  std::unique_ptr<IInPlaceFecEncoder>
    legacy_reed_solomon_fec_controller_t::create_in_place_encoder(
      const in_place_encoder_config_t &config
    ) {
    if (
      config.data_shards == 0 ||
      config.parity_shards == 0 ||
      (!config.generator_matrix.empty() &&
       config.generator_matrix.size() !=
         config.data_shards * config.parity_shards)
    ) {
      return nullptr;
    }

    auto encoder =
      std::make_unique<legacy_in_place_fec_encoder_t>(config);
    if (!encoder->valid()) {
      return nullptr;
    }
    return encoder;
  }

}  // namespace stream::fec
