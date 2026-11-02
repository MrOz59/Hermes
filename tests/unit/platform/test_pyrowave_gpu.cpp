/**
 * @file tests/unit/platform/test_pyrowave_gpu.cpp
 * @brief Opt-in tests of the PyroWave paths that convert frames on the GPU.
 *
 * A captured frame is converted with OpenGL into planes the encoder's Vulkan
 * device imports - straight from a DMA-BUF, or after an upload when the
 * capture delivers system memory, which is how Hermes-KMS displays reach the
 * encoder on NVIDIA. Nothing along either chain can be checked piecewise from
 * the CPU, so these encode a known frame, decode the bitstream, and compare
 * the picture with the Rec. 709 values the frame should have become.
 */
#include "../../tests_common.h"

#ifdef HAVE_PYROWAVE
  #include <array>
  #include <cmath>
  #include <cstdlib>
  #include <drm_fourcc.h>
  #include <fcntl.h>
  #include <gbm.h>
  #include <src/platform/linux/graphics.h>
  #include <src/platform/linux/pyrowave.h>
  #include <unistd.h>
  #include <vector>

extern "C" {
  #include <libavutil/frame.h>
}

namespace {
  constexpr int width = 512;
  constexpr int height = 256;
  constexpr int bar_width = width / 8;

  // SMPTE-style bars at full intensity: white, yellow, cyan, green, magenta,
  // red, blue, black. Red and blue sit at opposite ends of Cr and Cb, so a
  // swapped chroma pair cannot pass.
  constexpr std::array<std::array<int, 3>, 8> bars {{
    {255, 255, 255},
    {255, 255, 0},
    {0, 255, 255},
    {0, 255, 0},
    {255, 0, 255},
    {255, 0, 0},
    {0, 0, 255},
    {0, 0, 0},
  }};

  /// A pixel of the bars as XRGB8888, which is BGRA in memory.
  uint32_t xrgb8888(int x) {
    const auto &rgb = bars[x / bar_width];
    return 0xff000000U | uint32_t(rgb[0]) << 16 | uint32_t(rgb[1]) << 8 | uint32_t(rgb[2]);
  }

  /// The same pixel as XRGB2101010, which a 10-bit Hermes-KMS output scans out.
  uint32_t xrgb2101010(int x) {
    const auto &rgb = bars[x / bar_width];
    const auto ten = [](int value) {
      return uint32_t(value) * 1023u / 255u;
    };
    return 0xc0000000U | ten(rgb[0]) << 20 | ten(rgb[1]) << 10 | ten(rgb[2]);
  }

  /// Rec. 709, limited range, 8-bit: what the client is told to expect.
  std::array<double, 3> rec709_limited(const std::array<int, 3> &rgb) {
    const double r = rgb[0] / 255.0;
    const double g = rgb[1] / 255.0;
    const double b = rgb[2] / 255.0;
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    return {16.0 + 219.0 * y, 128.0 + 224.0 * (b - y) / 1.8556, 128.0 + 224.0 * (r - y) / 1.5748};
  }

  struct gbm_device_deleter_t {
    void operator()(gbm_device *device) const {
      gbm_device_destroy(device);
    }
  };

  struct gbm_bo_deleter_t {
    void operator()(gbm_bo *bo) const {
      gbm_bo_destroy(bo);
    }
  };

  const char *render_node() {
    const char *node = std::getenv("HERMES_PYROWAVE_TEST_RENDER_NODE");
    return node && *node ? node : nullptr;
  }

  /// Size the device's planes for the test frame, as a session does.
  void prepare(pyrowave::gpu_encode_device_t &device) {
    device.colorspace = {video::colorspace_e::rec709, false, 8};
    auto *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    ASSERT_EQ(device.set_frame(frame, nullptr), 0);
    device.apply_colorspace();
  }

  /// Encode what the device last converted and return the bitstream.
  std::vector<uint8_t> encode(pyrowave::gpu_encode_device_t &device, pyrowave_encoder encoder, size_t budget) {
    pyrowave_gpu_buffers buffers {};
    const pyrowave_gpu_sync_operation *acquire = nullptr;
    const pyrowave_gpu_sync_operation *release = nullptr;
    EXPECT_TRUE(device.planes(buffers, acquire, release));
    const pyrowave_rate_control rate_control {budget};
    EXPECT_EQ(pyrowave_encoder_encode_gpu_synchronous(encoder, acquire, release, &buffers, &rate_control), PYROWAVE_SUCCESS);

    pyrowave_packet packet {};
    size_t written = 0;
    std::vector<uint8_t> bitstream(budget);
    EXPECT_EQ(pyrowave_encoder_packetize(encoder, &packet, budget, &written, bitstream.data(), bitstream.size()), PYROWAVE_SUCCESS);
    device.encoded();
    EXPECT_EQ(written, 1u);
    bitstream.resize(packet.offset + packet.size);
    return bitstream;
  }

  /// Decode @p bitstream into YUV 4:2:0 planes laid out back to back.
  std::vector<uint8_t> decode(pyrowave_device device, const std::vector<uint8_t> &bitstream) {
    pyrowave_decoder_create_info info {};
    info.device = device;
    info.width = width;
    info.height = height;
    info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    pyrowave_decoder decoder {};
    EXPECT_EQ(pyrowave_decoder_create(&info, &decoder), PYROWAVE_SUCCESS);
    if (!decoder) {
      return {};
    }

    constexpr size_t luma = size_t(width) * height;
    std::vector<uint8_t> yuv(luma * 3 / 2);
    EXPECT_EQ(pyrowave_decoder_push_packet(decoder, bitstream.data(), bitstream.size()), PYROWAVE_SUCCESS);
    EXPECT_TRUE(pyrowave_decoder_decode_is_ready(decoder, false));
    pyrowave_cpu_buffer output {};
    output.data[0] = yuv.data();
    output.data[1] = yuv.data() + luma;
    output.data[2] = yuv.data() + luma + luma / 4;
    output.row_stride_in_bytes[0] = width;
    output.row_stride_in_bytes[1] = output.row_stride_in_bytes[2] = width / 2;
    output.plane_size_in_bytes[0] = luma;
    output.plane_size_in_bytes[1] = output.plane_size_in_bytes[2] = luma / 4;
    output.width = width;
    output.height = height;
    output.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    EXPECT_EQ(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &output), PYROWAVE_SUCCESS);
    pyrowave_decoder_destroy(decoder);
    return yuv;
  }

  /// The mean of each plane over the inside of each bar, away from the edges
  /// where a wavelet codec rings.
  std::array<std::array<double, 3>, 8> bar_means(const std::vector<uint8_t> &yuv) {
    constexpr size_t luma = size_t(width) * height;
    std::array<std::array<double, 3>, 8> means {};
    if (yuv.size() != luma * 3 / 2) {
      return means;
    }
    for (int bar = 0; bar < 8; ++bar) {
      for (int plane = 0; plane < 3; ++plane) {
        const int scale = plane ? 2 : 1;
        const int stride = width / scale;
        const uint8_t *base = yuv.data() + (plane == 0 ? 0 : plane == 1 ? luma :
                                                                          luma + luma / 4);
        double sum = 0;
        int count = 0;
        for (int y = 16 / scale; y < (height - 16) / scale; ++y) {
          for (int x = (bar * bar_width + 16) / scale; x < ((bar + 1) * bar_width - 16) / scale; ++x) {
            sum += base[y * stride + x];
            ++count;
          }
        }
        means[bar][plane] = sum / count;
      }
    }
    return means;
  }

  /**
   * Convert @p img, encode it twice - the second time as the encode loop
   * does when no new frame arrived - and check both decode to the bars.
   */
  void expect_bars(pyrowave::gpu_encode_device_t &device, platf::img_t &img) {
    ASSERT_EQ(device.convert(img), 0);

    pyrowave_encoder_create_info encoder_info {};
    encoder_info.device = device.device();
    encoder_info.width = width;
    encoder_info.height = height;
    encoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    pyrowave_encoder encoder {};
    ASSERT_EQ(pyrowave_encoder_create(&encoder_info, &encoder), PYROWAVE_SUCCESS);

    // A generous budget, so what is measured is the conversion, not the codec.
    const size_t budget = size_t(width) * height;
    const auto first = encode(device, encoder, budget);
    const auto repeat = encode(device, encoder, budget);
    pyrowave_encoder_destroy(encoder);
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(repeat.empty());

    const auto means = bar_means(decode(device.device(), first));
    for (int bar = 0; bar < 8; ++bar) {
      const auto expected = rec709_limited(bars[bar]);
      for (int plane = 0; plane < 3; ++plane) {
        EXPECT_NEAR(means[bar][plane], expected[plane], 2.5) << "bar " << bar << ", plane " << "YUV"[plane];
      }
    }

    const auto repeated = bar_means(decode(device.device(), repeat));
    for (int bar = 0; bar < 8; ++bar) {
      for (int plane = 0; plane < 3; ++plane) {
        EXPECT_NEAR(repeated[bar][plane], means[bar][plane], 0.5) << "bar " << bar << ", plane " << "YUV"[plane];
      }
    }
  }

  /// A frame in system memory, as a CPU-copy capture hands it over.
  struct ram_frame_t {
    explicit ram_frame_t(uint32_t (*pixel)(int)):
        pixels(size_t(width) * height) {
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          pixels[size_t(y) * width + x] = pixel(x);
        }
      }
      img.width = width;
      img.height = height;
      img.pixel_pitch = 4;
      img.row_pitch = width * 4;
      img.data = reinterpret_cast<uint8_t *>(pixels.data());
    }

    ~ram_frame_t() {
      img.data = nullptr;  // not owned by the image
    }

    std::vector<uint32_t> pixels;
    platf::img_t img;
  };

  void expect_ram_bars(uint32_t (*pixel)(int), uint32_t fourcc) {
    const char *node = render_node();
    if (!node) {
      GTEST_SKIP() << "Set HERMES_PYROWAVE_TEST_RENDER_NODE to opt into the PyroWave GPU tests";
    }
    ASSERT_EQ(gbm::init(), 0);
    file_t fd {open(node, O_RDWR | O_CLOEXEC)};
    ASSERT_GE(fd.el, 0);

    auto made = pyrowave::make_ram_encode_device(width, height, std::move(fd), fourcc);
    auto *device = dynamic_cast<pyrowave::gpu_encode_device_t *>(made.get());
    ASSERT_NE(device, nullptr) << "fell back to the CPU on " << node;
    prepare(*device);
    ram_frame_t frame {pixel};
    expect_bars(*device, frame.img);
  }
}  // namespace

TEST(PyroWaveGpu, CapturedFrameIsEncodedWithoutLeavingTheGpu) {
  const char *node = render_node();
  if (!node) {
    GTEST_SKIP() << "Set HERMES_PYROWAVE_TEST_RENDER_NODE to opt into the PyroWave GPU tests";
  }
  file_t fd {open(node, O_RDWR | O_CLOEXEC)};
  ASSERT_GE(fd.el, 0);
  ASSERT_EQ(gbm::init(), 0);

  // The "captured" frame: a linear XRGB8888 buffer, as a compositor or
  // Hermes-KMS would export it.
  std::unique_ptr<gbm_device, gbm_device_deleter_t> gbm {gbm_create_device(fd.el)};
  ASSERT_TRUE(gbm);
  std::unique_ptr<gbm_bo, gbm_bo_deleter_t> source {
    gbm_bo_create(gbm.get(), width, height, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR)
  };
  ASSERT_TRUE(source);
  {
    uint32_t stride = 0;
    void *mapping = nullptr;
    auto *pixels = static_cast<uint8_t *>(gbm_bo_map(source.get(), 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &stride, &mapping));
    ASSERT_NE(pixels, nullptr);
    for (int y = 0; y < height; ++y) {
      auto *row = reinterpret_cast<uint32_t *>(pixels + size_t(y) * stride);
      for (int x = 0; x < width; ++x) {
        row[x] = xrgb8888(x);
      }
    }
    gbm_bo_unmap(source.get(), mapping);
  }

  egl::img_descriptor_t img;
  img.width = width;
  img.height = height;
  img.data = nullptr;  // no cursor
  img.sequence = 1;
  img.sd = {};
  img.sd.width = width;
  img.sd.height = height;
  std::fill_n(img.sd.fds, 4, -1);
  img.sd.fds[0] = gbm_bo_get_fd(source.get());  // closed by the descriptor
  img.sd.fourcc = DRM_FORMAT_XRGB8888;
  img.sd.modifier = DRM_FORMAT_MOD_LINEAR;
  img.sd.pitches[0] = gbm_bo_get_stride(source.get());
  img.sd.offsets[0] = 0;

  auto device = pyrowave::make_gpu_encode_device(width, height, file_t {dup(fd.el)}, 0, 0);
  ASSERT_TRUE(device) << "the GPU path could not be set up on " << node;
  prepare(*device);
  expect_bars(*device, img);
}

TEST(PyroWaveGpu, FrameFromSystemMemoryIsConvertedOnTheGpu) {
  // The route Hermes-KMS displays take on NVIDIA, where their frames arrive
  // by CPU copy.
  expect_ram_bars(xrgb8888, DRM_FORMAT_XRGB8888);
}

TEST(PyroWaveGpu, TenBitFrameFromSystemMemoryIsConvertedOnTheGpu) {
  // A Hermes-KMS output loaded with color_depth=10 can scan out 10-bit
  // pixels even while the session is SDR.
  expect_ram_bars(xrgb2101010, DRM_FORMAT_XRGB2101010);
}

TEST(PyroWaveGpu, SystemMemoryCaptureFallsBackToTheCpuWithoutAGpu) {
  // No render node: the capture must still reach the encoder, through the
  // CPU conversion, rather than fail the session.
  auto device = pyrowave::make_ram_encode_device(64, 64, file_t {-1}, 0);
  ASSERT_TRUE(device);
  EXPECT_EQ(dynamic_cast<pyrowave::gpu_encode_device_t *>(device.get()), nullptr);
  EXPECT_EQ(device->data, nullptr) << "a hardware device would not take CPU-converted frames";
}
#else
TEST(PyroWaveGpu, CapturedFrameIsEncodedWithoutLeavingTheGpu) {
  GTEST_SKIP() << "Hermes was built without PyroWave";
}
#endif
