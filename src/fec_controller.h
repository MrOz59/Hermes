/**
 * @file src/fec_controller.h
 * @brief Injectable FEC boundary for the legacy GameStream media paths.
 */
#pragma once

#include "platform/common.h"
#include "utility.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace stream::fec {

  /// Protocol limit: the multi-FEC header carries a two-bit block count.
  inline constexpr std::size_t maximum_fec_blocks = 4;
  /// Reed-Solomon limit, shared by the data and parity shards of one block.
  inline constexpr std::size_t maximum_shards_per_block = 255;

  /**
   * @brief Protection actually applied to one encoded frame.
   *
   * `fec_percentage` may be below what the caller asked for: a frame only fits
   * in `maximum_fec_blocks` blocks while its data shards fit alongside the
   * parity shards the percentage implies.
   */
  struct frame_fec_plan_t {
    std::size_t fec_percentage = 0;
    std::size_t block_count = 1;
    /// Protection was lowered so the frame still fits the block limit.
    bool reduced_to_fit = false;
    /// The frame is too large for any protection at all.
    bool unprotected = false;
  };

  /**
   * @brief Choose the protection one frame can actually carry.
   *
   * Frames large enough to exhaust the block limit are almost always
   * keyframes, which are exactly the frames a loss burst must not take out --
   * losing one costs a black screen and another IDR round trip. Falling back
   * to the highest percentage that still fits keeps them protected instead of
   * leaving the most expensive frame in the stream completely bare.
   */
  [[nodiscard]] frame_fec_plan_t plan_frame_fec(
    std::size_t payload_bytes,
    std::size_t block_size_bytes,
    std::size_t requested_fec_percentage
  ) noexcept;

  struct encode_request_t {
    std::string_view payload;
    std::size_t block_size = 0;
    std::size_t fec_percentage = 0;
    std::size_t minimum_parity_shards = 0;
    std::size_t prefix_size = 0;
  };

  /**
   * @brief Move-only encoded shard block.
   *
   * Data shards may borrow storage from the request payload. The caller must
   * therefore keep that payload alive until every shard has been consumed.
   * Repair shards and optional per-shard prefixes are owned by this object.
   */
  class encoded_block_t {
  public:
    encoded_block_t() = default;
    encoded_block_t(
      std::size_t data_shards,
      std::size_t shard_count,
      std::size_t fec_percentage,
      std::size_t block_size,
      std::size_t prefix_size,
      util::buffer_t<char> &&owned_shards,
      util::buffer_t<char> &&prefixes,
      util::buffer_t<uint8_t *> &&shard_pointers,
      std::vector<platf::buffer_descriptor_t> &&payload_buffers
    );

    encoded_block_t(const encoded_block_t &) = delete;
    encoded_block_t &operator=(const encoded_block_t &) = delete;
    encoded_block_t(encoded_block_t &&) noexcept = default;
    encoded_block_t &operator=(encoded_block_t &&) noexcept = default;

    [[nodiscard]] std::size_t data_shard_count() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t fec_percentage() const noexcept;
    [[nodiscard]] std::size_t block_size() const noexcept;
    [[nodiscard]] std::size_t prefix_size() const noexcept;

    [[nodiscard]] char *data(std::size_t index) noexcept;
    [[nodiscard]] const char *data(std::size_t index) const noexcept;
    [[nodiscard]] char *prefix(std::size_t index) noexcept;
    [[nodiscard]] const char *prefix(std::size_t index) const noexcept;
    [[nodiscard]] char *prefix_data() noexcept;
    [[nodiscard]] std::vector<platf::buffer_descriptor_t> &
      payload_buffers() noexcept;

  private:
    std::size_t data_shards_ = 0;
    std::size_t shard_count_ = 0;
    std::size_t fec_percentage_ = 0;
    std::size_t block_size_ = 0;
    std::size_t prefix_size_ = 0;
    util::buffer_t<char> owned_shards_;
    util::buffer_t<char> prefixes_;
    util::buffer_t<uint8_t *> shard_pointers_;
    std::vector<platf::buffer_descriptor_t> payload_buffers_;
  };

  struct in_place_encoder_config_t {
    std::size_t data_shards = 0;
    std::size_t parity_shards = 0;
    std::span<const uint8_t> generator_matrix;
  };

  class IInPlaceFecEncoder {
  public:
    virtual ~IInPlaceFecEncoder() = default;

    virtual int encode(
      std::span<uint8_t *> shards,
      std::size_t block_size
    ) = 0;
  };

  /**
   * @brief FEC policy and encoder boundary used by media broadcasters.
   *
   * Instances are owned by one broadcaster thread. `encode()` may return data
   * views borrowing the request payload and must not retain the request.
   */
  class IFecController {
  public:
    virtual ~IFecController() = default;

    virtual encoded_block_t encode(const encode_request_t &request) = 0;
    virtual std::unique_ptr<IInPlaceFecEncoder>
      create_in_place_encoder(
        const in_place_encoder_config_t &config
      ) = 0;
  };

  /**
   * @brief Existing nanors/Reed-Solomon behavior behind the H1 boundary.
   */
  class legacy_reed_solomon_fec_controller_t final:
      public IFecController {
  public:
    encoded_block_t encode(const encode_request_t &request) override;
    std::unique_ptr<IInPlaceFecEncoder> create_in_place_encoder(
      const in_place_encoder_config_t &config
    ) override;
  };

}  // namespace stream::fec
