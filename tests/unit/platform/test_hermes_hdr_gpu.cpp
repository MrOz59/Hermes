/**
 * @file tests/unit/platform/test_hermes_hdr_gpu.cpp
 * @brief Opt-in tests of the real RAM upload and P010 conversion implementation.
 */
#include "../../tests_common.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <src/platform/linux/graphics.h>
#include <src/video_colorspace.h>
#include <vector>

TEST(HermesHdrGpu, PaddedTenBitRgbToP010) {
  const char *node = std::getenv("HERMES_HDR_TEST_RENDER_NODE");
  if (!node || !*node) {
    GTEST_SKIP() << "Set HERMES_HDR_TEST_RENDER_NODE to opt into offscreen GPU testing";
  }
  file_t fd {open(node, O_RDWR | O_CLOEXEC)};
  ASSERT_GE(fd.el, 0);
  ASSERT_EQ(gbm::init(), 0);
  gbm::gbm_t gbm {gbm::create_device(fd.el)};
  ASSERT_TRUE(gbm);
  auto display = egl::make_display(gbm.get());
  ASSERT_TRUE(display);
  auto ctx = egl::make_ctx(display.get());
  ASSERT_TRUE(ctx);
  constexpr int width = 2048, height = 8, stride = width + 3;
  auto target = egl::create_target(width, height, AV_PIX_FMT_P010LE);
  ASSERT_TRUE(target);
  auto sws = egl::sws_t::make(width, height, width, height, AV_PIX_FMT_P010LE);
  ASSERT_TRUE(sws);
  std::vector<uint32_t> input(stride * height, 0xdeadbeefU);
  platf::img_t img;
  img.width = width;
  img.height = height;
  img.pixel_pitch = 4;
  img.row_pitch = stride * 4;
  img.data = reinterpret_cast<uint8_t *>(input.data());
  const auto rgb_at = [](int x, int y) -> std::array<unsigned, 3> {
    if (y < 4) {
      const auto value = static_cast<unsigned>(x / 2);
      return {value, value, value};
    }
    return y < 6 ? std::array<unsigned, 3> {1023, 0, 0} : std::array<unsigned, 3> {0, 0, 1023};
  };
  for (const auto fourcc : {DRM_FORMAT_XRGB2101010, DRM_FORMAT_ARGB2101010, DRM_FORMAT_XBGR2101010, DRM_FORMAT_ABGR2101010}) {
    const bool bgr = fourcc == DRM_FORMAT_XBGR2101010 || fourcc == DRM_FORMAT_ABGR2101010;
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const auto rgb = rgb_at(x, y);
        input[y * stride + x] = 0xc0000000U | rgb[bgr ? 0 : 2] | (rgb[1] << 10) | (rgb[bgr ? 2 : 0] << 20);
      }
    }
    // Exercise restoration of unrelated GL upload state as well as a stride
    // that is not a multiple of the caller's eight-byte unpack alignment.
    gl::ctx.PixelStorei(GL_UNPACK_ALIGNMENT, 8);
    gl::ctx.PixelStorei(GL_UNPACK_ROW_LENGTH, 17);
    ASSERT_EQ(sws->load_ram(img, fourcc), 0);
    GLint alignment, row_length;
    gl::ctx.GetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    gl::ctx.GetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    EXPECT_EQ(alignment, 8);
    EXPECT_EQ(row_length, 17);
    for (bool full : {false, true}) {
      sws->apply_colorspace({video::colorspace_e::bt2020, full, 10});
      ASSERT_EQ(sws->convert((*target)->buf), 0);
      for (int plane = 0; plane < 2; ++plane) {
        const int rows = height / (plane + 1);
        std::vector<uint16_t> output(width * rows);
        gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, (*target)->buf[plane]);
        gl::ctx.ReadBuffer(GL_COLOR_ATTACHMENT0 + plane);
        gl::ctx.ReadPixels(0, 0, width / (plane + 1), rows, plane ? GL_RG : GL_RED, GL_UNSIGNED_SHORT, output.data());
        ASSERT_EQ(gl::ctx.GetError(), GL_NO_ERROR);
        for (int y = 0; y < rows; ++y) {
          for (int x = 0; x < width; ++x) {
            const auto rgb = rgb_at(plane ? x / 2 * 2 : x, y * (plane + 1));
            const double r = rgb[0] / 1023.0, g = rgb[1] / 1023.0, b = rgb[2] / 1023.0;
            const double luma = .2627 * r + .6780 * g + .0593 * b;
            const double code = plane ? 512 + (full ? 1023 : 896) *
                                                ((x % 2) ? (r - luma) / (2 * (1 - .2627)) : (b - luma) / (2 * (1 - .0593))) :
                                        (full ? 0 : 64) + (full ? 1023 : 876) * luma;
            const int expected = std::clamp(static_cast<int>(std::floor(code + .5)), 0, 1023);
            const auto actual = output[y * width + x];
            ASSERT_EQ(actual & 63, 0) << fourcc << ',' << full << ',' << plane << ',' << x << ',' << y;
            ASSERT_LE(std::abs(static_cast<int>(actual >> 6) - expected), 1)
              << fourcc << ',' << full << ',' << plane << ',' << x << ',' << y;
            if (y * (plane + 1) < 4 && (plane || x < 2 || x >= width - 2)) {
              ASSERT_EQ(actual >> 6, expected);
            }
          }
        }
      }
    }
  }
}

#ifdef SUNSHINE_BUILD_CUDA
  #include <src/platform/linux/cuda.h>
extern "C" {
  #include <libavcodec/avcodec.h>
  #include <libavutil/mastering_display_metadata.h>
}

TEST(HermesHdrCuda, CpuUploadToCudaAndNvencMain10) {
  if (!std::getenv("HERMES_HDR_TEST_CUDA")) {
    GTEST_SKIP() << "Set HERMES_HDR_TEST_CUDA=1 for synthetic CUDA/NVENC HDR testing on CUDA device 0";
  }
  ASSERT_EQ(gbm::init(), 0);
  constexpr int width = 128, height = 64;
  auto converter = cuda::make_avcodec_gl_ram_encode_device(width, height, DRM_FORMAT_XRGB2101010);
  ASSERT_TRUE(converter);
  const auto unref = [](AVBufferRef *p) {
    av_buffer_unref(&p);
  };
  AVBufferRef *raw_device = nullptr;
  ASSERT_EQ(av_hwdevice_ctx_create(&raw_device, AV_HWDEVICE_TYPE_CUDA, "0", nullptr, 1 /* primary context */), 0);
  std::unique_ptr<AVBufferRef, decltype(unref)> device {raw_device, unref};
  std::unique_ptr<AVBufferRef, decltype(unref)> frames {av_hwframe_ctx_alloc(device.get()), unref};
  ASSERT_TRUE(frames);
  auto *frame_ctx = reinterpret_cast<AVHWFramesContext *>(frames->data);
  frame_ctx->format = AV_PIX_FMT_CUDA;
  frame_ctx->sw_format = AV_PIX_FMT_P010LE;
  frame_ctx->width = width;
  frame_ctx->height = height;
  ASSERT_EQ(av_hwframe_ctx_init(frames.get()), 0);
  frame_t frame {av_frame_alloc()};
  ASSERT_TRUE(frame);
  frame->width = width;
  frame->height = height;
  frame->format = AV_PIX_FMT_CUDA;
  frame->color_primaries = AVCOL_PRI_BT2020;
  frame->color_trc = AVCOL_TRC_SMPTE2084;
  frame->colorspace = AVCOL_SPC_BT2020_NCL;
  frame->color_range = AVCOL_RANGE_MPEG;
  auto *mastering = av_mastering_display_metadata_create_side_data(frame.get());
  ASSERT_TRUE(mastering);
  mastering->max_luminance = {1000, 1};
  mastering->min_luminance = {1, 10000};
  mastering->display_primaries[0][0] = {34000, 50000};
  mastering->display_primaries[0][1] = {16000, 50000};
  mastering->display_primaries[1][0] = {13250, 50000};
  mastering->display_primaries[1][1] = {34500, 50000};
  mastering->display_primaries[2][0] = {7500, 50000};
  mastering->display_primaries[2][1] = {3000, 50000};
  mastering->white_point[0] = {15635, 50000};
  mastering->white_point[1] = {16450, 50000};
  mastering->has_luminance = mastering->has_primaries = 1;
  auto *light = av_content_light_metadata_create_side_data(frame.get());
  ASSERT_TRUE(light);
  light->MaxCLL = 1000;
  light->MaxFALL = 400;
  ASSERT_EQ(converter->set_frame(frame.release(), frames.get()), 0);
  converter->colorspace = {video::colorspace_e::bt2020, false, 10};
  converter->apply_colorspace();
  std::vector<uint32_t> pixels(width * height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint32_t v = x < width / 2 ? 0 : 1023;
      pixels[y * width + x] = v | (v << 10) | (v << 20) | 0xc0000000U;
    }
  }
  platf::img_t image;
  image.width = width;
  image.height = height;
  image.pixel_pitch = 4;
  image.row_pitch = width * 4;
  image.data = reinterpret_cast<uint8_t *>(pixels.data());
  ASSERT_EQ(converter->convert(image), 0);
  frame_t cpu {av_frame_alloc()};
  ASSERT_TRUE(cpu);
  ASSERT_EQ(av_hwframe_transfer_data(cpu.get(), converter->frame, 0), 0);
  for (int y = 0; y < height; ++y) {
    const auto *row = reinterpret_cast<const uint16_t *>(cpu->data[0] + y * cpu->linesize[0]);
    for (int x = 0; x < width; ++x) {
      ASSERT_EQ(row[x], (x < width / 2 ? 64 : 940) << 6);
    }
  }
  for (int y = 0; y < height / 2; ++y) {
    const auto *row = reinterpret_cast<const uint16_t *>(cpu->data[1] + y * cpu->linesize[1]);
    for (int x = 0; x < width; ++x) {
      ASSERT_EQ(row[x], 512 << 6);
    }
  }
  const AVCodec *codec = avcodec_find_encoder_by_name("hevc_nvenc");
  ASSERT_TRUE(codec);
  const auto free_codec = [](AVCodecContext *p) {
    avcodec_free_context(&p);
  };
  std::unique_ptr<AVCodecContext, decltype(free_codec)> encoder {avcodec_alloc_context3(codec), free_codec};
  ASSERT_TRUE(encoder);
  encoder->width = width;
  encoder->height = height;
  encoder->pix_fmt = AV_PIX_FMT_CUDA;
  encoder->profile = AV_PROFILE_HEVC_MAIN_10;
  encoder->time_base = {1, 60};
  encoder->framerate = {60, 1};
  encoder->gop_size = 1;
  encoder->max_b_frames = 0;
  encoder->bit_rate = 2000000;
  encoder->color_primaries = AVCOL_PRI_BT2020;
  encoder->color_trc = AVCOL_TRC_SMPTE2084;
  encoder->colorspace = AVCOL_SPC_BT2020_NCL;
  encoder->color_range = AVCOL_RANGE_MPEG;
  encoder->hw_frames_ctx = av_buffer_ref(frames.get());
  ASSERT_EQ(avcodec_open2(encoder.get(), codec, nullptr), 0);
  converter->frame->pts = 0;
  ASSERT_EQ(avcodec_send_frame(encoder.get(), converter->frame), 0);
  ASSERT_EQ(avcodec_send_frame(encoder.get(), nullptr), 0);
  const auto free_packet = [](AVPacket *p) {
    av_packet_free(&p);
  };
  std::unique_ptr<AVPacket, decltype(free_packet)> packet {av_packet_alloc(), free_packet};
  ASSERT_TRUE(packet);
  ASSERT_EQ(avcodec_receive_packet(encoder.get(), packet.get()), 0);
  ASSERT_GT(packet->size, 0);
  // The pinned FFmpeg build deliberately omits decoders. Keep the sample in
  // the test build directory for independent ffprobe metadata verification.
  const auto bitstream = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "hermes-hdr-nvenc.hevc";
  std::ofstream output {bitstream, std::ios::binary | std::ios::trunc};
  ASSERT_TRUE(output);
  output.write(reinterpret_cast<const char *>(packet->data), packet->size);
  output.close();
  ASSERT_TRUE(output);
  RecordProperty("hdr_bitstream", bitstream.string());
}
#endif
