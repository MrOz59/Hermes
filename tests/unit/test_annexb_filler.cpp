/**
 * @file tests/unit/test_annexb_filler.cpp
 * @brief Removing the filler data CBR pads H.264 and HEVC frames with.
 */
#include "../tests_common.h"

#include <initializer_list>
#include <src/annexb_filler.h>
#include <vector>

namespace {
  using video::annexb::codec_e;
  using bytes_t = std::vector<std::uint8_t>;

  /// One NAL unit with a 3- or 4-byte start code.
  bytes_t unit(bool four_byte, std::initializer_list<std::uint8_t> body) {
    bytes_t out;
    if (four_byte) {
      out.push_back(0);
    }
    out.insert(out.end(), {0, 0, 1});
    out.insert(out.end(), body);
    return out;
  }

  bytes_t join(std::initializer_list<bytes_t> units) {
    bytes_t out;
    for (const auto &u : units) {
      out.insert(out.end(), u.begin(), u.end());
    }
    return out;
  }

  bytes_t strip(bytes_t frame, codec_e codec) {
    frame.resize(video::annexb::strip_filler_data(frame.data(), frame.size(), codec));
    return frame;
  }

  const auto sps = unit(true, {0x67, 1, 2, 3});
  const auto pps = unit(false, {0x68, 4, 5});
  const auto idr = unit(false, {0x65, 9, 9, 9, 9});
  const auto filler = unit(false, {0x0c, 0xff, 0xff, 0xff, 0x80});  // nal_unit_type 12
}  // namespace

TEST(AnnexBFiller, RemovesH264FillerAnywhere) {
  EXPECT_EQ(strip(join({sps, pps, idr, filler}), codec_e::h264), join({sps, pps, idr}));
  EXPECT_EQ(strip(join({sps, filler, pps, filler, idr}), codec_e::h264), join({sps, pps, idr}));
  EXPECT_EQ(strip(join({filler, idr}), codec_e::h264), idr);
}

TEST(AnnexBFiller, TrailingZerosGoWithTheRemovedUnit) {
  auto padded = idr;
  padded.insert(padded.end(), {0, 0});
  EXPECT_EQ(strip(join({padded, filler}), codec_e::h264), idr);
}

TEST(AnnexBFiller, LeavesFramesWithoutFillerUntouched) {
  const auto frame = join({sps, pps, idr});
  EXPECT_EQ(strip(frame, codec_e::h264), frame);
  EXPECT_EQ(strip(bytes_t {1, 2, 3}, codec_e::h264), (bytes_t {1, 2, 3}));
}

TEST(AnnexBFiller, LeavesAFrameOfOnlyFillerWhole) {
  const auto frame = join({filler, filler});
  EXPECT_EQ(strip(frame, codec_e::h264), frame);
}

TEST(AnnexBFiller, RemovesHevcFillerOnly) {
  const auto vps = unit(true, {0x40, 1, 7});  // VPS_NUT 32
  const auto hevc_sps = unit(false, {0x42, 1, 8});  // SPS_NUT 33
  const auto hevc_idr = unit(false, {0x26, 1, 5, 5, 5});  // IDR_W_RADL 19
  const auto hevc_filler = unit(false, {0x4c, 1, 0xff, 0xff, 0x80});  // FD_NUT 38
  EXPECT_EQ(strip(join({vps, hevc_sps, hevc_idr, hevc_filler, hevc_filler}), codec_e::hevc), join({vps, hevc_sps, hevc_idr}));
  // H.264's filler type means something else in HEVC.
  const auto frame = join({hevc_idr, filler});
  EXPECT_EQ(strip(frame, codec_e::hevc), frame);
}
