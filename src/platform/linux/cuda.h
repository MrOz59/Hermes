/**
 * @file src/platform/linux/cuda.h
 * @brief Definitions for CUDA implementation.
 */
#pragma once

#if defined(SUNSHINE_BUILD_CUDA)
  // standard includes
  #include <cstddef>
  #include <cstdint>
  #include <memory>
  #include <optional>
  #include <string>
  #include <vector>

  // local includes
  #include "cuda_pixel.h"
  #include "src/video_colorspace.h"

namespace platf {
  struct avcodec_encode_device_t;
  struct img_t;
}  // namespace platf

namespace cuda {

  namespace nvfbc {
    std::vector<std::string> display_names();
  }

  /**
   * @brief Create a CUDA encoding device.
   * @param width Width of captured frames.
   * @param height Height of captured frames.
   * @param vram Frames arrive as CUDA textures rather than in host memory.
   * @param layout Channel layout of host-memory frames; ignored for VRAM.
   * @return FFmpeg encoding device context.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, bool vram, pixel::layout_e layout = pixel::layout_e::bgra8);

  /**
   * @brief Create a GL->CUDA encoding device for consuming captured dmabufs.
   * @param in_width Width of captured frames.
   * @param in_height Height of captured frames.
   * @param offset_x Offset of content in captured frame.
   * @param offset_y Offset of content in captured frame.
   * @return FFmpeg encoding device context.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_gl_encode_device(int width, int height, int offset_x, int offset_y);

  // CPU scanout upload followed by GL RGB-to-P010/NV12 and CUDA transfer.
  // Avoids importing NVIDIA's problematic system-memory DMA-BUFs.
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_gl_ram_encode_device(int width, int height, std::uint32_t fourcc);

  int init();

  /**
   * @brief Allocate page-locked host memory for frames the encoder uploads.
   *
   * Uploads from pageable memory are staged through a driver buffer: an extra
   * CPU copy of every frame and a transfer below PCIe speed. Page-locked
   * frames are copied by DMA directly. The allocation is portable and made in
   * the primary context of device 0, the context FFmpeg's NVENC uses; if that
   * context is not active yet it is given FFmpeg's flags first, because FFmpeg
   * refuses an active primary context with any others.
   * @return The allocation, or nullptr when CUDA is unavailable.
   */
  void *alloc_host(std::size_t size);

  /** @brief Free memory from alloc_host(). */
  void free_host(void *ptr);
}  // namespace cuda

typedef struct cudaArray *cudaArray_t;

  #if !defined(__CUDACC__)
typedef struct CUstream_st *cudaStream_t;
typedef unsigned long long cudaTextureObject_t;
  #else /* defined(__CUDACC__) */
typedef __location__(device_builtin) struct CUstream_st *cudaStream_t;
typedef __location__(device_builtin) unsigned long long cudaTextureObject_t;
  #endif /* !defined(__CUDACC__) */

namespace cuda {

  class freeCudaPtr_t {
  public:
    void operator()(void *ptr);
  };

  class freeCudaStream_t {
  public:
    void operator()(cudaStream_t ptr);
  };

  using ptr_t = std::unique_ptr<void, freeCudaPtr_t>;
  using stream_t = std::unique_ptr<CUstream_st, freeCudaStream_t>;

  stream_t make_stream(int flags = 0);

  struct viewport_t {
    int width, height;
    int offsetX, offsetY;
  };

  class tex_t {
  public:
    /**
     * @param height Rows of the image.
     * @param pitch Bytes per row, four per pixel for every layout.
     * @param layout Channel layout of the pixels copied in. 8-bit BGRA is
     *               sampled as normalized floats; packed 10-bit words as
     *               unsigned integers the kernel decodes, and filters itself.
     */
    static std::optional<tex_t> make(int height, int pitch, pixel::layout_e layout = pixel::layout_e::bgra8);

    tex_t();
    tex_t(tex_t &&);

    tex_t &operator=(tex_t &&other);

    ~tex_t();

    int copy(std::uint8_t *src, int height, int pitch);

    cudaArray_t array;
    pixel::layout_e layout {pixel::layout_e::bgra8};

    struct texture {
      cudaTextureObject_t point;
      cudaTextureObject_t linear;
    } texture;
  };

  class sws_t {
  public:
    sws_t() = default;
    sws_t(int in_width, int in_height, int out_width, int out_height, int pitch, int threadsPerBlock, ptr_t &&color_matrix);

    /**
     * in_width, in_height -- The width and height of the captured image in pixels
     * out_width, out_height -- the width and height of the NV12 image in pixels
     *
     * pitch -- The size of a single row of pixels in bytes
     */
    static std::optional<sws_t> make(int in_width, int in_height, int out_width, int out_height, int pitch);

    /**
     * layout -- the channel layout of the input texture
     * output_bits -- 8 for NV12, 10 for P010
     */
    static std::optional<sws_t> make(int in_width, int in_height, int out_width, int out_height, int pitch, pixel::layout_e layout, int output_bits);

    // Converts loaded image into a CUDevicePtr
    int convert(std::uint8_t *Y, std::uint8_t *UV, std::uint32_t pitchY, std::uint32_t pitchUV, cudaTextureObject_t texture, stream_t::pointer stream);
    int convert(std::uint8_t *Y, std::uint8_t *UV, std::uint32_t pitchY, std::uint32_t pitchUV, cudaTextureObject_t texture, stream_t::pointer stream, const viewport_t &viewport);

    void apply_colorspace(const video::sunshine_colorspace_t &colorspace);

    int load_ram(platf::img_t &img, cudaArray_t array);

    ptr_t color_matrix;

    /// video::new_color_vectors_from_colorspace() for output_bits, used by every
    /// conversion except 8-bit BGRA to NV12, which keeps its original kernel.
    ptr_t code_matrix;

    pixel::layout_e layout {pixel::layout_e::bgra8};
    int output_bits {8};

    /// Scaling is in effect; packed 10-bit input is then filtered in the kernel.
    bool linear {false};

    int threadsPerBlock;

    viewport_t viewport;

    float scale;
  };
}  // namespace cuda

#endif
