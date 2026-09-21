/**
 * @file tests/unit/platform/test_hermes_color.cpp
 * @brief Validation and metadata lifetime tests for Hermes-KMS HDR capture.
 */
#include "../../tests_common.h"

#include <src/platform/common.h>
#include <src/platform/linux/hermes_kms_color.h>

namespace {
  using namespace VDISPLAY::hermes_kms;

  color_t hdr() {
    color_t color {};
    color.flags = color_valid | color_hdr_valid | color_rgb_full_range;
    color.colorspace = colorspace_bt2020_rgb;
    color.eotf = 2;
    return color;
  }
}  // namespace

TEST(HermesColor, UnknownStateAndSdrAreNotHdr) {
  color_t color {};
  EXPECT_FALSE(color.known());
  EXPECT_FALSE(color.supported());
  EXPECT_FALSE(color.pq());
  color.flags = color_valid | color_rgb_full_range;
  EXPECT_TRUE(color.supported());
  EXPECT_FALSE(color.pq());
  color.flags |= color_hdr_valid;
  EXPECT_TRUE(color.supported());
  EXPECT_FALSE(color.pq());
}

TEST(HermesColor, UnspecifiedLuminanceDoesNotDisablePq) {
  const auto color = hdr();
  EXPECT_TRUE(color.supported());
  EXPECT_TRUE(color.pq());
  SS_HDR_METADATA out {};
  ASSERT_TRUE(color.copy_hdr_metadata(out));
  EXPECT_EQ(out.maxContentLightLevel, 0);
  EXPECT_EQ(out.maxFrameAverageLightLevel, 0);
  EXPECT_EQ(out.maxDisplayLuminance, 0);
}

TEST(HermesColor, CopiesDrmUnitsAndClearsOldMetadataOnSdr) {
  auto color = hdr();
  color.primaries[0] = {34000, 16000};
  color.primaries[1] = {13250, 34500};
  color.primaries[2] = {7500, 3000};
  color.white_point = {15635, 16450};
  color.max_luminance = 1000;
  color.min_luminance = 50;
  color.max_cll = 1200;
  color.max_fall = 400;
  SS_HDR_METADATA out {};
  ASSERT_TRUE(color.copy_hdr_metadata(out));
  EXPECT_EQ(out.displayPrimaries[0].x, 34000);
  EXPECT_EQ(out.displayPrimaries[1].y, 34500);
  EXPECT_EQ(out.displayPrimaries[2].x, 7500);
  EXPECT_EQ(out.whitePoint.y, 16450);
  EXPECT_EQ(out.maxDisplayLuminance, 1000);
  EXPECT_EQ(out.minDisplayLuminance, 50);
  EXPECT_EQ(out.maxContentLightLevel, 1200);
  EXPECT_EQ(out.maxFrameAverageLightLevel, 400);
  color = {};
  color.flags = color_valid | color_rgb_full_range;
  EXPECT_FALSE(color.copy_hdr_metadata(out));
  EXPECT_EQ(out.maxDisplayLuminance, 0);
  EXPECT_EQ(out.displayPrimaries[0].x, 0);
}

TEST(HermesColor, RejectsMalformedOrUnsupportedInterpretation) {
  auto valid = hdr();
  for (int field = 0; field < 9; ++field) {
    auto color = valid;
    switch (field) {
      case 0:
        color.flags |= 1U << 31;
        break;
      case 1:
        color.flags &= ~color_rgb_full_range;
        break;
      case 2:
        color.metadata_type = 1;
        break;
      case 3:
        color.descriptor_type = 1;
        break;
      case 4:
        color.eotf = 3;
        break;
      case 5:
        color.colorspace = colorspace_default;
        break;
      case 6:
        color.padding = 1;
        break;
      case 7:
        color.reserved[1] = 1;
        break;
      case 8:
        color.flags &= ~color_hdr_valid;
        break;
    }
    EXPECT_FALSE(color.supported()) << field;
    EXPECT_FALSE(color == valid);
  }
}
