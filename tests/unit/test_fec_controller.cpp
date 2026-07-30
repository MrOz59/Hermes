/**
 * @file tests/unit/test_fec_controller.cpp
 * @brief Tests for the injectable Hermes FEC boundary.
 */

#include "../tests_common.h"

#include <src/fec_controller.h>

extern "C" {
  // clang-format off
#include <third-party/nanors/rs.h>
#include <src/rswrapper.h>
  // clang-format on
}

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace {

  class fake_in_place_fec_encoder_t final:
      public stream::fec::IInPlaceFecEncoder {
  public:
    int encode(
      std::span<uint8_t *> shards,
      std::size_t block_size
    ) override {
      ++encode_calls;
      last_shard_count = shards.size();
      last_block_size = block_size;
      return result;
    }

    int result = 17;
    int encode_calls = 0;
    std::size_t last_shard_count = 0;
    std::size_t last_block_size = 0;
  };

  class fake_fec_controller_t final:
      public stream::fec::IFecController {
  public:
    stream::fec::encoded_block_t encode(
      const stream::fec::encode_request_t &request
    ) override {
      ++encode_calls;
      payload.assign(request.payload);
      block_size = request.block_size;
      fec_percentage = request.fec_percentage;
      minimum_parity_shards = request.minimum_parity_shards;
      prefix_size = request.prefix_size;
      return {};
    }

    std::unique_ptr<stream::fec::IInPlaceFecEncoder>
      create_in_place_encoder(
        const stream::fec::in_place_encoder_config_t &config
      ) override {
      ++create_calls;
      data_shards = config.data_shards;
      parity_shards = config.parity_shards;
      generator_matrix.assign(
        config.generator_matrix.begin(),
        config.generator_matrix.end()
      );
      auto encoder = std::make_unique<fake_in_place_fec_encoder_t>();
      last_encoder = encoder.get();
      return encoder;
    }

    std::string payload;
    std::vector<uint8_t> generator_matrix;
    fake_in_place_fec_encoder_t *last_encoder = nullptr;
    std::size_t block_size = 0;
    std::size_t fec_percentage = 0;
    std::size_t minimum_parity_shards = 0;
    std::size_t prefix_size = 0;
    std::size_t data_shards = 0;
    std::size_t parity_shards = 0;
    int encode_calls = 0;
    int create_calls = 0;
  };

  struct reed_solomon_guard_t {
    explicit reed_solomon_guard_t(
      std::size_t data_shards,
      std::size_t parity_shards
    ):
        encoder {
          reed_solomon_new(
            static_cast<int>(data_shards),
            static_cast<int>(parity_shards)
          )
        } {
    }

    ~reed_solomon_guard_t() {
      reed_solomon_release(encoder);
    }

    reed_solomon *encoder = nullptr;
  };

}  // namespace

TEST(FecControllerTest, InterfaceSupportsFakeVideoAndInPlaceEncoders) {
  fake_fec_controller_t fake;
  stream::fec::IFecController &controller = fake;

  auto block = controller.encode({
    .payload = "encoded-frame",
    .block_size = 1200,
    .fec_percentage = 20,
    .minimum_parity_shards = 2,
    .prefix_size = 16,
  });

  constexpr std::array<uint8_t, 4> matrix {1, 2, 3, 4};
  auto encoder = controller.create_in_place_encoder({
    .data_shards = 2,
    .parity_shards = 2,
    .generator_matrix = matrix,
  });
  std::array<std::array<uint8_t, 8>, 4> storage {};
  std::array<uint8_t *, 4> shards {
    storage[0].data(),
    storage[1].data(),
    storage[2].data(),
    storage[3].data(),
  };

  ASSERT_NE(encoder, nullptr);
  EXPECT_EQ(encoder->encode(shards, storage[0].size()), 17);
  EXPECT_EQ(block.size(), 0);
  EXPECT_EQ(fake.encode_calls, 1);
  EXPECT_EQ(fake.payload, "encoded-frame");
  EXPECT_EQ(fake.block_size, 1200);
  EXPECT_EQ(fake.fec_percentage, 20);
  EXPECT_EQ(fake.minimum_parity_shards, 2);
  EXPECT_EQ(fake.prefix_size, 16);
  EXPECT_EQ(fake.create_calls, 1);
  EXPECT_EQ(fake.data_shards, 2);
  EXPECT_EQ(fake.parity_shards, 2);
  EXPECT_EQ(fake.generator_matrix, std::vector<uint8_t>(matrix.begin(), matrix.end()));
  ASSERT_NE(fake.last_encoder, nullptr);
  EXPECT_EQ(fake.last_encoder->encode_calls, 1);
  EXPECT_EQ(fake.last_encoder->last_shard_count, shards.size());
  EXPECT_EQ(fake.last_encoder->last_block_size, storage[0].size());
}

TEST(FecControllerTest, LegacyVideoEncodingMatchesReedSolomonBytes) {
  reed_solomon_init();
  stream::fec::legacy_reed_solomon_fec_controller_t controller;
  std::array<char, 8> payload {
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
  };

  auto block = controller.encode({
    .payload = std::string_view {payload.data(), payload.size()},
    .block_size = 4,
    .fec_percentage = 50,
    .minimum_parity_shards = 0,
    .prefix_size = 3,
  });

  ASSERT_EQ(block.data_shard_count(), 2);
  ASSERT_EQ(block.size(), 3);
  EXPECT_EQ(block.fec_percentage(), 50);
  EXPECT_EQ(block.block_size(), 4);
  EXPECT_EQ(block.prefix_size(), 3);
  EXPECT_EQ(
    std::memcmp(block.data(0), payload.data(), 4),
    0
  );
  EXPECT_EQ(
    std::memcmp(block.data(1), payload.data() + 4, 4),
    0
  );
  EXPECT_NE(block.prefix(0), nullptr);
  ASSERT_EQ(block.payload_buffers().size(), 2);
  EXPECT_EQ(block.payload_buffers()[0].size, payload.size());
  EXPECT_EQ(block.payload_buffers()[1].size, 4);

  std::array<uint8_t, 4> expected_parity {};
  std::array<uint8_t *, 3> expected_shards {
    reinterpret_cast<uint8_t *>(payload.data()),
    reinterpret_cast<uint8_t *>(payload.data() + 4),
    expected_parity.data(),
  };
  reed_solomon_guard_t reference {2, 1};
  ASSERT_NE(reference.encoder, nullptr);
  ASSERT_EQ(
    reed_solomon_encode(
      reference.encoder,
      expected_shards.data(),
      expected_shards.size(),
      expected_parity.size()
    ),
    0
  );
  EXPECT_EQ(
    std::memcmp(
      block.data(2),
      expected_parity.data(),
      expected_parity.size()
    ),
    0
  );
}

TEST(FecControllerTest, LegacyVideoEncodingPreservesPaddingAndMinimumParity) {
  reed_solomon_init();
  stream::fec::legacy_reed_solomon_fec_controller_t controller;
  std::array<char, 6> payload {1, 2, 3, 4, 5, 6};

  auto block = controller.encode({
    .payload = std::string_view {payload.data(), payload.size()},
    .block_size = 4,
    .fec_percentage = 20,
    .minimum_parity_shards = 2,
    .prefix_size = 0,
  });

  ASSERT_EQ(block.data_shard_count(), 2);
  ASSERT_EQ(block.size(), 4);
  EXPECT_EQ(block.fec_percentage(), 100);
  EXPECT_EQ(block.prefix(0), nullptr);
  EXPECT_EQ(block.data(1)[0], 5);
  EXPECT_EQ(block.data(1)[1], 6);
  EXPECT_EQ(block.data(1)[2], 0);
  EXPECT_EQ(block.data(1)[3], 0);
}

TEST(FecControllerTest, LegacyInPlaceEncoderPreservesAudioMatrix) {
  reed_solomon_init();
  constexpr std::size_t data_shards = 4;
  constexpr std::size_t parity_shards = 2;
  constexpr std::size_t shard_count = data_shards + parity_shards;
  constexpr std::size_t block_size = 8;
  constexpr std::array<uint8_t, 8> matrix {
    0x77,
    0x40,
    0x38,
    0x0e,
    0xc7,
    0xa7,
    0x0d,
    0x6c,
  };

  std::array<std::array<uint8_t, block_size>, shard_count>
    actual_storage {};
  std::array<std::array<uint8_t, block_size>, shard_count>
    expected_storage {};
  for (std::size_t shard = 0; shard < data_shards; ++shard) {
    for (std::size_t byte = 0; byte < block_size; ++byte) {
      actual_storage[shard][byte] =
        static_cast<uint8_t>(shard * block_size + byte);
      expected_storage[shard][byte] =
        actual_storage[shard][byte];
    }
  }

  std::array<uint8_t *, shard_count> actual_shards;
  std::array<uint8_t *, shard_count> expected_shards;
  for (std::size_t shard = 0; shard < shard_count; ++shard) {
    actual_shards[shard] = actual_storage[shard].data();
    expected_shards[shard] = expected_storage[shard].data();
  }

  stream::fec::legacy_reed_solomon_fec_controller_t controller;
  auto encoder = controller.create_in_place_encoder({
    .data_shards = data_shards,
    .parity_shards = parity_shards,
    .generator_matrix = matrix,
  });
  ASSERT_NE(encoder, nullptr);
  ASSERT_EQ(encoder->encode(actual_shards, block_size), 0);

  reed_solomon_guard_t reference {data_shards, parity_shards};
  ASSERT_NE(reference.encoder, nullptr);
  std::memcpy(reference.encoder->p, matrix.data(), matrix.size());
  ASSERT_EQ(
    reed_solomon_encode(
      reference.encoder,
      expected_shards.data(),
      expected_shards.size(),
      block_size
    ),
    0
  );

  for (std::size_t shard = data_shards; shard < shard_count; ++shard) {
    EXPECT_EQ(actual_storage[shard], expected_storage[shard]);
  }
}

TEST(FecControllerTest, RejectsInvalidInPlaceGeneratorMatrix) {
  stream::fec::legacy_reed_solomon_fec_controller_t controller;
  constexpr std::array<uint8_t, 3> invalid_matrix {1, 2, 3};

  EXPECT_EQ(
    controller.create_in_place_encoder({
      .data_shards = 2,
      .parity_shards = 2,
      .generator_matrix = invalid_matrix,
    }),
    nullptr
  );
}
