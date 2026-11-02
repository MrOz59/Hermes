/**
 * @file src/platform/linux/pyrowave.h
 * @brief GPU conversion for the experimental PyroWave codec.
 *
 * PyroWave reads its input straight from GPU images, so a captured DMA-BUF can
 * reach the encoder without a trip through system memory. The conversion from
 * the captured RGB to YCbCr is the same OpenGL pass the VAAPI path uses; what
 * differs is the target. Here it is a pair of GBM buffers - luma, and the two
 * chroma planes interleaved at half resolution - that the encoder's Vulkan
 * device imports once and samples every frame.
 */
#pragma once

#ifdef HAVE_PYROWAVE

  // standard includes
  #include <memory>

  // lib includes
  // pyrowave.h refuses to compile unless the Vulkan types come first.
  // clang-format off
  #include <vulkan/vulkan.h>
  #include <pyrowave.h>
  // clang-format on

  // local includes
  #include "misc.h"
  #include "src/platform/common.h"

namespace pyrowave {

  /**
   * @brief An encode device whose converted frames live in PyroWave images.
   *
   * convert() renders the frame; planes() then describes it to
   * pyrowave_encoder_encode_gpu_synchronous(). The images stay valid for the
   * device's lifetime and are overwritten by the next convert().
   */
  class gpu_encode_device_t: public platf::avcodec_encode_device_t {
  public:
    /** The PyroWave device the planes were imported into; create the encoder on it. */
    virtual pyrowave_device device() const = 0;

    /**
     * @brief Describe the last converted frame to the encoder.
     *
     * @p acquire orders the encode after the conversion that wrote the planes,
     * and hands the images from the conversion's API to the encoder's queue;
     * @p release hands them back. Both refer to storage inside the device and
     * stay valid until the next call.
     *
     * @return false when no frame was converted yet.
     */
    virtual bool planes(pyrowave_gpu_buffers &buffers, const pyrowave_gpu_sync_operation *&acquire, const pyrowave_gpu_sync_operation *&release) = 0;

    /** Called once the encode that consumed planes() has been submitted. */
    virtual void encoded() = 0;
  };

  /**
   * @brief Make a device that imports captured DMA-BUFs on @p render_device.
   *
   * @param width Width of the captured image.
   * @param height Height of the captured image.
   * @param render_device The render node the capture's DMA-BUFs belong to.
   * @param offset_x Horizontal offset of the display within the captured image.
   * @param offset_y Vertical offset of the display within the captured image.
   * @return nullptr when EGL or PyroWave cannot be set up on that GPU.
   */
  std::unique_ptr<gpu_encode_device_t> make_gpu_encode_device(int width, int height, file_t &&render_device, int offset_x, int offset_y);

  /** As above, on the render node the configured adapter names (or the first one). */
  std::unique_ptr<gpu_encode_device_t> make_gpu_encode_device(int width, int height, int offset_x, int offset_y);

  /**
   * @brief Make a device for a capture that delivers frames in system memory.
   *
   * The frame is uploaded and converted on the GPU, which costs a fraction of
   * a CPU conversion; this is how Hermes-KMS displays reach PyroWave on
   * NVIDIA, where they are captured with a CPU copy. When the GPU cannot be
   * used this way the result converts on the CPU instead, so a capture into
   * system memory always has a path to the encoder.
   *
   * @param fourcc DRM format of the captured pixels; 0 for BGRA.
   * @return a gpu_encode_device_t, or else a plain device for CPU conversion.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_ram_encode_device(int width, int height, file_t &&render_device, std::uint32_t fourcc = 0);

  /** As above, on the render node the configured adapter names (or the first one). */
  std::unique_ptr<platf::avcodec_encode_device_t> make_ram_encode_device(int width, int height, std::uint32_t fourcc = 0);

}  // namespace pyrowave

#endif
