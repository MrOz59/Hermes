/**
 * @file src/platform/linux/cuda.cu
 * @brief CUDA implementation for Linux.
 */
// standard includes
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>

// platform includes
#include <helper_math.h>

// local includes
#include "cuda.h"

using namespace std::literals;

#define SUNSHINE_STRINGVIEW_HELPER(x) x##sv
#define SUNSHINE_STRINGVIEW(x) SUNSHINE_STRINGVIEW_HELPER(x)

#define CU_CHECK(x, y) \
  if (check((x), SUNSHINE_STRINGVIEW(y ": "))) \
  return -1

#define CU_CHECK_VOID(x, y) \
  if (check((x), SUNSHINE_STRINGVIEW(y ": "))) \
    return;

#define CU_CHECK_PTR(x, y) \
  if (check((x), SUNSHINE_STRINGVIEW(y ": "))) \
    return nullptr;

#define CU_CHECK_OPT(x, y) \
  if (check((x), SUNSHINE_STRINGVIEW(y ": "))) \
    return std::nullopt;

#define CU_CHECK_IGNORE(x, y) \
  check((x), SUNSHINE_STRINGVIEW(y ": "))

using namespace std::literals;

// Special declarations
/**
 * NVCC tends to have problems with standard headers.
 * Don't include common.h, instead use bare minimum
 * of standard headers and duplicate declarations of necessary classes.
 * Not pretty and extremely error-prone, fix at earliest convenience.
 */
namespace platf {
  struct img_t: std::enable_shared_from_this<img_t> {
  public:
    std::uint8_t *data {};
    std::int32_t width {};
    std::int32_t height {};
    std::int32_t pixel_pitch {};
    std::int32_t row_pitch {};

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;

    virtual ~img_t() = default;
  };
}  // namespace platf

// End special declarations

namespace cuda {

  struct alignas(16) cuda_color_t {
    float4 color_vec_y;
    float4 color_vec_u;
    float4 color_vec_v;
    float2 range_y;
    float2 range_uv;
  };

  static_assert(sizeof(video::color_t) == sizeof(cuda::cuda_color_t), "color matrix struct mismatch");
  static_assert(sizeof(video::color_t) == sizeof(pixel::matrix_t), "code matrix struct mismatch");

  auto constexpr INVALID_TEXTURE = std::numeric_limits<cudaTextureObject_t>::max();

  template<class T>
  inline T div_align(T l, T r) {
    return (l + r - 1) / r;
  }

  void pass_error(const std::string_view &sv, const char *name, const char *description);

  inline static int check(cudaError_t result, const std::string_view &sv) {
    if (result) {
      auto name = cudaGetErrorName(result);
      auto description = cudaGetErrorString(result);

      pass_error(sv, name, description);
      return -1;
    }

    return 0;
  }

  template<class T>
  ptr_t make_ptr() {
    void *p;
    CU_CHECK_PTR(cudaMalloc(&p, sizeof(T)), "Couldn't allocate color matrix");

    ptr_t ptr {p};

    return ptr;
  }

  void freeCudaPtr_t::operator()(void *ptr) {
    CU_CHECK_IGNORE(cudaFree(ptr), "Couldn't free cuda device pointer");
  }

  void freeCudaStream_t::operator()(cudaStream_t ptr) {
    CU_CHECK_IGNORE(cudaStreamDestroy(ptr), "Couldn't free cuda stream");
  }

  stream_t make_stream(int flags) {
    cudaStream_t stream;

    if (!flags) {
      CU_CHECK_PTR(cudaStreamCreate(&stream), "Couldn't create cuda stream");
    } else {
      CU_CHECK_PTR(cudaStreamCreateWithFlags(&stream, flags), "Couldn't create cuda stream with flags");
    }

    return stream_t {stream};
  }

  inline __device__ float3 bgra_to_rgb(uchar4 vec) {
    return make_float3((float) vec.z, (float) vec.y, (float) vec.x);
  }

  inline __device__ float3 bgra_to_rgb(float4 vec) {
    return make_float3(vec.z, vec.y, vec.x);
  }

  inline __device__ float2 calcUV(float3 pixel, const cuda_color_t *const color_matrix) {
    float4 vec_u = color_matrix->color_vec_u;
    float4 vec_v = color_matrix->color_vec_v;

    float u = dot(pixel, make_float3(vec_u)) + vec_u.w;
    float v = dot(pixel, make_float3(vec_v)) + vec_v.w;

    u = u * color_matrix->range_uv.x + color_matrix->range_uv.y;
    v = v * color_matrix->range_uv.x + color_matrix->range_uv.y;

    return make_float2(u, v);
  }

  inline __device__ float calcY(float3 pixel, const cuda_color_t *const color_matrix) {
    float4 vec_y = color_matrix->color_vec_y;

    return (dot(pixel, make_float3(vec_y)) + vec_y.w) * color_matrix->range_y.x + color_matrix->range_y.y;
  }

  __global__ void RGBA_to_NV12(
    cudaTextureObject_t srcImage,
    std::uint8_t *dstY,
    std::uint8_t *dstUV,
    std::uint32_t dstPitchY,
    std::uint32_t dstPitchUV,
    float scale,
    const viewport_t viewport,
    const cuda_color_t *const color_matrix
  ) {
    int idX = (threadIdx.x + blockDim.x * blockIdx.x) * 2;
    int idY = (threadIdx.y + blockDim.y * blockIdx.y) * 2;

    if (idX >= viewport.width) {
      return;
    }
    if (idY >= viewport.height) {
      return;
    }

    float x = idX * scale;
    float y = idY * scale;

    idX += viewport.offsetX;
    idY += viewport.offsetY;

    uint8_t *dstY0 = dstY + idX + idY * dstPitchY;
    uint8_t *dstY1 = dstY + idX + (idY + 1) * dstPitchY;
    dstUV = dstUV + idX + (idY / 2 * dstPitchUV);

    float3 rgb_lt = bgra_to_rgb(tex2D<float4>(srcImage, x, y));
    float3 rgb_rt = bgra_to_rgb(tex2D<float4>(srcImage, x + scale, y));
    float3 rgb_lb = bgra_to_rgb(tex2D<float4>(srcImage, x, y + scale));
    float3 rgb_rb = bgra_to_rgb(tex2D<float4>(srcImage, x + scale, y + scale));

    float2 uv_lt = calcUV(rgb_lt, color_matrix) * 256.0f;
    float2 uv_rt = calcUV(rgb_rt, color_matrix) * 256.0f;
    float2 uv_lb = calcUV(rgb_lb, color_matrix) * 256.0f;
    float2 uv_rb = calcUV(rgb_rb, color_matrix) * 256.0f;

    float2 uv = (uv_lt + uv_lb + uv_rt + uv_rb) * 0.25f;

    dstUV[0] = uv.x;
    dstUV[1] = uv.y;
    dstY0[0] = calcY(rgb_lt, color_matrix) * 245.0f;  // 245.0f is a magic number to ensure slight changes in luminosity are more visible
    dstY0[1] = calcY(rgb_rt, color_matrix) * 245.0f;  // 245.0f is a magic number to ensure slight changes in luminosity are more visible
    dstY1[0] = calcY(rgb_lb, color_matrix) * 245.0f;  // 245.0f is a magic number to ensure slight changes in luminosity are more visible
    dstY1[1] = calcY(rgb_rb, color_matrix) * 245.0f;  // 245.0f is a magic number to ensure slight changes in luminosity are more visible
  }

  /**
   * Sample the input at unnormalized coordinates, as RGB in [0, 1].
   *
   * 8-bit BGRA comes from a normalized-float texture that the hardware
   * filters, point or linear depending on the texture object passed. Packed
   * 10-bit words come from an unsigned-integer texture, which cannot be
   * filtered, so when scaling the four texels around the sample are decoded
   * and blended here, around texel centres the way the hardware would.
   */
  __device__ pixel::rgb_t fetch_rgb(cudaTextureObject_t tex, pixel::layout_e layout, bool linear, float x, float y) {
    if (layout == pixel::layout_e::bgra8) {
      const float4 bgra = tex2D<float4>(tex, x, y);
      return {bgra.z, bgra.y, bgra.x};
    }
    if (!linear) {
      return pixel::unpack(layout, tex2D<unsigned int>(tex, x, y));
    }

    const float fx = x - 0.5f;
    const float fy = y - 0.5f;
    const float x0 = floorf(fx);
    const float y0 = floorf(fy);
    const float ax = fx - x0;
    const float ay = fy - y0;

    const auto t00 = pixel::unpack(layout, tex2D<unsigned int>(tex, x0 + 0.5f, y0 + 0.5f));
    const auto t10 = pixel::unpack(layout, tex2D<unsigned int>(tex, x0 + 1.5f, y0 + 0.5f));
    const auto t01 = pixel::unpack(layout, tex2D<unsigned int>(tex, x0 + 0.5f, y0 + 1.5f));
    const auto t11 = pixel::unpack(layout, tex2D<unsigned int>(tex, x0 + 1.5f, y0 + 1.5f));
    return pixel::mix(pixel::mix(t00, t10, ax), pixel::mix(t01, t11, ax), ay);
  }

  /**
   * RGB to 4:2:0 YUV in integer code values, for 8-bit (NV12) or 10-bit
   * (P010) samples, from any input layout.
   *
   * Uses the matrices of video::new_color_vectors_from_colorspace(), which
   * produce code values for the target bit depth and range directly. The
   * original 8-bit BGRA to NV12 path keeps RGBA_to_NV12 and its constants.
   */
  template<class sample_t>
  __global__ void RGB_to_YUV420(
    cudaTextureObject_t srcImage,
    pixel::layout_e layout,
    bool linear,
    std::uint8_t *dstY,
    std::uint8_t *dstUV,
    std::uint32_t dstPitchY,
    std::uint32_t dstPitchUV,
    float scale,
    const viewport_t viewport,
    const pixel::matrix_t *const matrix,
    std::uint32_t max_code
  ) {
    int idX = (threadIdx.x + blockDim.x * blockIdx.x) * 2;
    int idY = (threadIdx.y + blockDim.y * blockIdx.y) * 2;

    if (idX >= viewport.width) {
      return;
    }
    if (idY >= viewport.height) {
      return;
    }

    float x = idX * scale;
    float y = idY * scale;

    idX += viewport.offsetX;
    idY += viewport.offsetY;

    const auto lt = fetch_rgb(srcImage, layout, linear, x, y);
    const auto rt = fetch_rgb(srcImage, layout, linear, x + scale, y);
    const auto lb = fetch_rgb(srcImage, layout, linear, x, y + scale);
    const auto rb = fetch_rgb(srcImage, layout, linear, x + scale, y + scale);

    auto *dstY0 = reinterpret_cast<sample_t *>(dstY + idY * dstPitchY) + idX;
    auto *dstY1 = reinterpret_cast<sample_t *>(dstY + (idY + 1) * dstPitchY) + idX;
    auto *uv = reinterpret_cast<sample_t *>(dstUV + (idY / 2) * dstPitchUV) + idX;

    dstY0[0] = pixel::store<sample_t>(pixel::quantize(pixel::encode(matrix->y, lt), max_code));
    dstY0[1] = pixel::store<sample_t>(pixel::quantize(pixel::encode(matrix->y, rt), max_code));
    dstY1[0] = pixel::store<sample_t>(pixel::quantize(pixel::encode(matrix->y, lb), max_code));
    dstY1[1] = pixel::store<sample_t>(pixel::quantize(pixel::encode(matrix->y, rb), max_code));

    const auto chroma = pixel::average(lt, rt, lb, rb);
    uv[0] = pixel::store<sample_t>(pixel::quantize(pixel::encode(matrix->u, chroma), max_code));
    uv[1] = pixel::store<sample_t>(pixel::quantize(pixel::encode(matrix->v, chroma), max_code));
  }

  int tex_t::copy(std::uint8_t *src, int height, int pitch) {
    CU_CHECK(cudaMemcpy2DToArray(array, 0, 0, src, pitch, pitch, height, cudaMemcpyDeviceToDevice), "Couldn't copy to cuda array from deviceptr");

    return 0;
  }

  std::optional<tex_t> tex_t::make(int height, int pitch, pixel::layout_e layout) {
    tex_t tex;
    tex.layout = layout;

    const bool packed = layout != pixel::layout_e::bgra8;
    auto format = packed ? cudaCreateChannelDesc<unsigned int>() : cudaCreateChannelDesc<uchar4>();
    // The BGRA array has always been allocated `pitch` elements wide; a packed
    // array is exactly one element per pixel.
    CU_CHECK_OPT(cudaMallocArray(&tex.array, &format, packed ? pitch / 4 : pitch, height, cudaArrayDefault), "Couldn't allocate cuda array");

    cudaResourceDesc res {};
    res.resType = cudaResourceTypeArray;
    res.res.array.array = tex.array;

    cudaTextureDesc desc {};

    desc.readMode = packed ? cudaReadModeElementType : cudaReadModeNormalizedFloat;
    desc.filterMode = cudaFilterModePoint;
    desc.normalizedCoords = false;

    std::fill_n(std::begin(desc.addressMode), 2, cudaAddressModeClamp);

    CU_CHECK_OPT(cudaCreateTextureObject(&tex.texture.point, &res, &desc, nullptr), "Couldn't create cuda texture that uses point interpolation");

    // Integer textures cannot be filtered by the hardware: the "linear"
    // object of a packed texture samples points too, and fetch_rgb() blends.
    if (!packed) {
      desc.filterMode = cudaFilterModeLinear;
    }

    CU_CHECK_OPT(cudaCreateTextureObject(&tex.texture.linear, &res, &desc, nullptr), "Couldn't create cuda texture that uses linear interpolation");

    return tex;
  }

  tex_t::tex_t():
      array {},
      texture {INVALID_TEXTURE, INVALID_TEXTURE} {
  }

  tex_t::tex_t(tex_t &&other):
      array {other.array},
      layout {other.layout},
      texture {other.texture} {
    other.array = 0;
    other.texture.point = INVALID_TEXTURE;
    other.texture.linear = INVALID_TEXTURE;
  }

  tex_t &tex_t::operator=(tex_t &&other) {
    std::swap(array, other.array);
    std::swap(layout, other.layout);
    std::swap(texture, other.texture);

    return *this;
  }

  tex_t::~tex_t() {
    if (texture.point != INVALID_TEXTURE) {
      CU_CHECK_IGNORE(cudaDestroyTextureObject(texture.point), "Couldn't deallocate cuda texture that uses point interpolation");

      texture.point = INVALID_TEXTURE;
    }

    if (texture.linear != INVALID_TEXTURE) {
      CU_CHECK_IGNORE(cudaDestroyTextureObject(texture.linear), "Couldn't deallocate cuda texture that uses linear interpolation");

      texture.linear = INVALID_TEXTURE;
    }

    if (array) {
      CU_CHECK_IGNORE(cudaFreeArray(array), "Couldn't deallocate cuda array");

      array = cudaArray_t {};
    }
  }

  sws_t::sws_t(int in_width, int in_height, int out_width, int out_height, int pitch, int threadsPerBlock, ptr_t &&color_matrix):
      threadsPerBlock {threadsPerBlock},
      color_matrix {std::move(color_matrix)} {
    // Ensure aspect ratio is maintained
    auto scalar = std::fminf(out_width / (float) in_width, out_height / (float) in_height);
    auto out_width_f = in_width * scalar;
    auto out_height_f = in_height * scalar;

    // result is always positive
    auto offsetX_f = (out_width - out_width_f) / 2;
    auto offsetY_f = (out_height - out_height_f) / 2;

    viewport.width = out_width_f;
    viewport.height = out_height_f;

    viewport.offsetX = offsetX_f;
    viewport.offsetY = offsetY_f;

    scale = 1.0f / scalar;
  }

  std::optional<sws_t> sws_t::make(int in_width, int in_height, int out_width, int out_height, int pitch) {
    cudaDeviceProp props;
    int device;
    CU_CHECK_OPT(cudaGetDevice(&device), "Couldn't get cuda device");
    CU_CHECK_OPT(cudaGetDeviceProperties(&props, device), "Couldn't get cuda device properties");

    auto ptr = make_ptr<cuda_color_t>();
    if (!ptr) {
      return std::nullopt;
    }

    return std::make_optional<sws_t>(in_width, in_height, out_width, out_height, pitch, props.maxThreadsPerMultiProcessor / props.maxBlocksPerMultiProcessor, std::move(ptr));
  }

  std::optional<sws_t> sws_t::make(int in_width, int in_height, int out_width, int out_height, int pitch, pixel::layout_e layout, int output_bits) {
    if (output_bits != 8 && output_bits != 10) {
      return std::nullopt;
    }

    auto sws = make(in_width, in_height, out_width, out_height, pitch);
    if (!sws) {
      return std::nullopt;
    }

    sws->layout = layout;
    sws->output_bits = output_bits;
    if (layout != pixel::layout_e::bgra8 || output_bits != 8) {
      sws->code_matrix = make_ptr<pixel::matrix_t>();
      if (!sws->code_matrix) {
        return std::nullopt;
      }
    }

    return sws;
  }

  int sws_t::convert(std::uint8_t *Y, std::uint8_t *UV, std::uint32_t pitchY, std::uint32_t pitchUV, cudaTextureObject_t texture, stream_t::pointer stream) {
    return convert(Y, UV, pitchY, pitchUV, texture, stream, viewport);
  }

  int sws_t::convert(std::uint8_t *Y, std::uint8_t *UV, std::uint32_t pitchY, std::uint32_t pitchUV, cudaTextureObject_t texture, stream_t::pointer stream, const viewport_t &viewport) {
    int threadsX = viewport.width / 2;
    int threadsY = viewport.height / 2;

    dim3 block(threadsPerBlock);
    dim3 grid(div_align(threadsX, threadsPerBlock), threadsY);

    if (!code_matrix) {
      RGBA_to_NV12<<<grid, block, 0, stream>>>(texture, Y, UV, pitchY, pitchUV, scale, viewport, (cuda_color_t *) color_matrix.get());

      return CU_CHECK_IGNORE(cudaGetLastError(), "RGBA_to_NV12 failed");
    }

    auto matrix = (const pixel::matrix_t *) code_matrix.get();
    if (output_bits == 10) {
      RGB_to_YUV420<std::uint16_t><<<grid, block, 0, stream>>>(texture, layout, linear, Y, UV, pitchY, pitchUV, scale, viewport, matrix, 1023);
    } else {
      RGB_to_YUV420<std::uint8_t><<<grid, block, 0, stream>>>(texture, layout, linear, Y, UV, pitchY, pitchUV, scale, viewport, matrix, 255);
    }

    return CU_CHECK_IGNORE(cudaGetLastError(), "RGB_to_YUV420 failed");
  }

  void sws_t::apply_colorspace(const video::sunshine_colorspace_t &colorspace) {
    auto color_p = video::color_vectors_from_colorspace(colorspace);
    CU_CHECK_IGNORE(cudaMemcpy(color_matrix.get(), color_p, sizeof(video::color_t), cudaMemcpyHostToDevice), "Couldn't copy color matrix to cuda");

    if (code_matrix) {
      // Code values for the samples actually written, whatever depth the
      // client's colorspace names.
      auto target = colorspace;
      target.bit_depth = output_bits;
      auto codes = video::new_color_vectors_from_colorspace(target);
      CU_CHECK_IGNORE(cudaMemcpy(code_matrix.get(), codes, sizeof(video::color_t), cudaMemcpyHostToDevice), "Couldn't copy code matrix to cuda");
    }
  }

  int sws_t::load_ram(platf::img_t &img, cudaArray_t array) {
    return CU_CHECK_IGNORE(cudaMemcpy2DToArray(array, 0, 0, img.data, img.row_pitch, img.width * img.pixel_pitch, img.height, cudaMemcpyHostToDevice), "Couldn't copy to cuda array");
  }

}  // namespace cuda
