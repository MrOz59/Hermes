/**
 * @file src/platform/linux/pyrowave.cpp
 * @brief GPU conversion for the experimental PyroWave codec.
 */
// standard includes
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

// lib includes
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

// local includes
#include "graphics.h"
#include "pyrowave.h"
#include "src/config.h"
#include "src/logging.h"

using namespace std::literals;

namespace pyrowave {
  namespace {
    // EGL_ANDROID_native_fence_sync. The glad loader Hermes ships was not
    // generated with it, so the one entry point it adds is looked up by hand.
    constexpr EGLenum sync_native_fence_android = 0x3144;
    using dup_native_fence_fd_fn = EGLint (*)(EGLDisplay, EGLSync);

    struct device_deleter_t {
      void operator()(pyrowave_device_opaque *device) const {
        pyrowave_device_destroy(device);
      }
    };

    using device_ptr = std::unique_ptr<pyrowave_device_opaque, device_deleter_t>;

    bool succeeded(pyrowave_result result, std::string_view operation) {
      if (result == PYROWAVE_SUCCESS) {
        return true;
      }
      BOOST_LOG(error) << "[PyroWave] "sv << operation << " failed with status "sv << static_cast<int>(result);
      return false;
    }

    /**
     * The PyroWave device on the GPU @p render_fd belongs to.
     *
     * The C API selects a GPU by PCI ids. A render node knows its own, so this
     * picks the device that can import the node's buffers; two identical
     * cards could still be confused, which the API offers no way around on
     * Linux short of handing it a VkDevice we would have to create ourselves.
     */
    device_ptr create_device(int render_fd) {
      std::uint32_t vendor = 0;
      std::uint32_t product = 0;
      drmDevicePtr drm_device = nullptr;
      if (drmGetDevice2(render_fd, 0, &drm_device) == 0 && drm_device) {
        if (drm_device->bustype == DRM_BUS_PCI && drm_device->deviceinfo.pci) {
          vendor = drm_device->deviceinfo.pci->vendor_id;
          product = drm_device->deviceinfo.pci->device_id;
        }
        drmFreeDevice(&drm_device);
      }

      pyrowave_device raw {};
      if (!succeeded(pyrowave_create_device_by_compat(vendor, product, nullptr, nullptr, nullptr, &raw), "Vulkan device creation"sv)) {
        return nullptr;
      }
      device_ptr device {raw};
      if (!pyrowave_device_confirm_interop_support(device.get())) {
        BOOST_LOG(error) << "[PyroWave] The Vulkan driver for "sv << util::hex(vendor).to_string_view() << ':'
                         << util::hex(product).to_string_view()
                         << " cannot import DMA-BUFs and sync files, so frames cannot stay on the GPU."sv;
        return nullptr;
      }
      // Hermes encodes from another process than the one rendering. The async
      // compute queue keeps the encode from waiting behind a busy game.
      pyrowave_device_set_queue_type(device.get(), VK_QUEUE_COMPUTE_BIT);
      return device;
    }

    /** One GBM buffer, shared by the OpenGL conversion and the Vulkan encoder. */
    struct plane_t {
      plane_t() = default;
      plane_t(const plane_t &) = delete;
      plane_t &operator=(const plane_t &) = delete;

      ~plane_t() {
        reset();
      }

      void reset() {
        if (image) {
          pyrowave_image_destroy(image);
          image = nullptr;
        }
        if (bo) {
          gbm_bo_destroy(bo);
          bo = nullptr;
        }
      }

      gbm_bo *bo {};
      pyrowave_image image {};
      int width {};
      int height {};
      std::uint32_t fourcc {};
      std::uint64_t modifier {DRM_FORMAT_MOD_INVALID};
      int plane_count {};
    };

    /**
     * Allocate @p plane. A driver-chosen layout is tried first, since tiled
     * render targets are cheaper to write; LINEAR is what every driver can
     * share when it cannot name the layout it picked.
     */
    bool allocate(gbm_device *gbm, plane_t &plane, int width, int height, std::uint32_t fourcc, bool linear) {
      plane.reset();
      plane.width = width;
      plane.height = height;
      plane.fourcc = fourcc;
      plane.bo = gbm_bo_create(gbm, width, height, fourcc, GBM_BO_USE_RENDERING | (linear ? GBM_BO_USE_LINEAR : 0));
      if (!plane.bo) {
        return false;
      }
      plane.modifier = gbm_bo_get_modifier(plane.bo);
      if (plane.modifier == DRM_FORMAT_MOD_INVALID && linear) {
        plane.modifier = DRM_FORMAT_MOD_LINEAR;
      }
      plane.plane_count = gbm_bo_get_plane_count(plane.bo);
      // An implicit layout cannot be described to Vulkan, and more than four
      // memory planes cannot be described to EGL.
      if (plane.modifier == DRM_FORMAT_MOD_INVALID || plane.plane_count < 1 || plane.plane_count > 4) {
        plane.reset();
        return false;
      }
      return true;
    }

    egl::surface_descriptor_t describe(const plane_t &plane, int fd) {
      egl::surface_descriptor_t sd {};
      sd.width = plane.width;
      sd.height = plane.height;
      sd.fourcc = plane.fourcc;
      sd.modifier = plane.modifier;
      std::fill_n(sd.fds, 4, -1);
      for (int x = 0; x < plane.plane_count; ++x) {
        sd.fds[x] = fd;
        sd.pitches[x] = gbm_bo_get_stride_for_plane(plane.bo, x);
        sd.offsets[x] = gbm_bo_get_offset(plane.bo, x);
      }
      return sd;
    }

    bool import_plane(pyrowave_device device, plane_t &plane, VkFormat format) {
      std::array<VkSubresourceLayout, 4> layouts {};
      for (int x = 0; x < plane.plane_count; ++x) {
        layouts[x].offset = gbm_bo_get_offset(plane.bo, x);
        layouts[x].rowPitch = gbm_bo_get_stride_for_plane(plane.bo, x);
      }

      VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
      modifier_info.drmFormatModifier = plane.modifier;
      modifier_info.drmFormatModifierPlaneCount = static_cast<std::uint32_t>(plane.plane_count);
      modifier_info.pPlaneLayouts = layouts.data();

      VkImageCreateInfo image_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      image_info.pNext = &modifier_info;
      image_info.imageType = VK_IMAGE_TYPE_2D;
      image_info.format = format;
      image_info.extent = {static_cast<std::uint32_t>(plane.width), static_cast<std::uint32_t>(plane.height), 1};
      image_info.mipLevels = 1;
      image_info.arrayLayers = 1;
      image_info.samples = VK_SAMPLE_COUNT_1_BIT;
      image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
      image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

      const int fd = gbm_bo_get_fd(plane.bo);
      if (fd < 0) {
        return false;
      }
      pyrowave_image_create_info create_info {};
      create_info.device = device;
      create_info.external_handle = static_cast<pyrowave_os_handle>(fd);
      create_info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      create_info.image_create_info = &image_info;
      // A successful import owns the descriptor. After a failure it is not
      // known whether the driver got as far as taking it, so it is left open
      // rather than risk closing a number another thread has since reused.
      return pyrowave_image_create(&create_info, &plane.image) == PYROWAVE_SUCCESS;
    }

    struct av_frame_deleter_t {
      void operator()(AVFrame *frame) const {
        av_frame_free(&frame);
      }
    };
  }  // namespace

  class egl_encode_device_t: public gpu_encode_device_t {
  public:
    ~egl_encode_device_t() override {
      drop_fence();
      drop_sync();
    }

    int init(int in_width, int in_height, file_t &&render_device, int offset_x, int offset_y, bool vram, std::uint32_t ram_fourcc) {
      file = std::move(render_device);
      width = in_width;
      height = in_height;
      this->offset_x = offset_x;
      this->offset_y = offset_y;
      this->vram = vram;
      this->ram_fourcc = ram_fourcc;

      if (!gbm::create_device) {
        BOOST_LOG(warning) << "[PyroWave] libgbm is not loaded."sv;
        return -1;
      }
      gbm.reset(gbm::create_device(file.el));
      if (!gbm) {
        BOOST_LOG(error) << "[PyroWave] Could not create a GBM device: "sv << std::strerror(errno);
        return -1;
      }
      display = egl::make_display(gbm.get());
      if (!display) {
        return -1;
      }
      auto ctx_opt = egl::make_ctx(display.get());
      if (!ctx_opt) {
        return -1;
      }
      ctx = std::move(*ctx_opt);

      const char *extensions = eglQueryString(display.get(), EGL_EXTENSIONS);
      if (extensions && std::strstr(extensions, "EGL_ANDROID_native_fence_sync") && eglCreateSync && eglDestroySync) {
        dup_native_fence_fd = reinterpret_cast<dup_native_fence_fd_fn>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));
      }
      if (!dup_native_fence_fd) {
        BOOST_LOG(info) << "[PyroWave] EGL cannot export fences; each frame is finished on the CPU before it is encoded."sv;
      }

      pyro = create_device(file.el);
      if (!pyro) {
        return -1;
      }

      // Sharing planes between OpenGL and Vulkan is what can fail on a given
      // driver (GBM formats, modifiers, DMA-BUF import), and set_frame() is too
      // late for a caller to choose another path. Find out now, on a tiny
      // frame.
      if (!import_targets(64, 64, false) && !import_targets(64, 64, true)) {
        BOOST_LOG(warning) << "[PyroWave] This GPU cannot share planes between OpenGL and Vulkan."sv;
        return -1;
      }
      return 0;
    }

    pyrowave_device device() const override {
      return pyro.get();
    }

    int set_frame(AVFrame *frame, AVBufferRef * /* hw_frames_ctx */) override {
      owned_frame.reset(frame);
      this->frame = frame;
      if (frame->width <= 0 || frame->height <= 0 || (frame->width & 1) || (frame->height & 1)) {
        BOOST_LOG(error) << "[PyroWave] 4:2:0 needs even dimensions, not "sv << frame->width << 'x' << frame->height;
        return -1;
      }

      bool linear = false;
      if (!import_targets(frame->width, frame->height, false)) {
        linear = true;
        if (!import_targets(frame->width, frame->height, true)) {
          BOOST_LOG(error) << "[PyroWave] Could not share a "sv << frame->width << 'x' << frame->height
                           << " target between OpenGL and Vulkan on this GPU."sv;
          return -1;
        }
      }
      BOOST_LOG(info) << "[PyroWave] Converting "sv << (vram ? "captured GPU buffers"sv : "frames uploaded from system memory"sv)
                      << " on the GPU into "sv << (linear ? "linear"sv : "tiled"sv) << ' ' << frame->width << 'x'
                      << frame->height << " planes (modifier "sv << util::hex(luma.modifier).to_string_view() << ')';

      auto sws_opt = egl::sws_t::make(width, height, frame->width, frame->height, AV_PIX_FMT_NV12);
      if (!sws_opt) {
        return -1;
      }
      sws = std::move(*sws_opt);
      return 0;
    }

    void apply_colorspace() override {
      sws.apply_colorspace(colorspace);
    }

    int convert(platf::img_t &img) override {
      if (!vram) {
        // A capture into system memory: one texture upload, then the same
        // conversion as for a DMA-BUF.
        if (sws.load_ram(img, ram_fourcc)) {
          return -1;
        }
        sws.convert(nv12->buf);
        drop_fence();
        fence_fd = export_fence();
        converted = true;
        return 0;
      }

      auto &descriptor = (egl::img_descriptor_t &) img;

      if (descriptor.sequence == 0) {
        // The blank frame encoded before the first capture arrives.
        rgb = egl::create_blank(img);
      } else if (descriptor.sequence > sequence) {
        sequence = descriptor.sequence;
        rgb = egl::rgb_t {};
        auto rgb_opt = egl::import_source(display.get(), descriptor.sd);
        if (!rgb_opt) {
          return -1;
        }
        rgb = std::move(*rgb_opt);
      }

      sws.load_vram(descriptor, offset_x, offset_y, rgb->tex[0]);
      sws.convert(nv12->buf);

      drop_fence();
      fence_fd = export_fence();
      converted = true;
      return 0;
    }

    bool planes(pyrowave_gpu_buffers &out, const pyrowave_gpu_sync_operation *&acquire, const pyrowave_gpu_sync_operation *&release) override {
      if (!converted) {
        return false;
      }

      drop_sync();
      acquire_op.sync = {};
      if (fence_fd >= 0) {
        pyrowave_sync_object_create_info sync_info {};
        sync_info.device = pyro.get();
        sync_info.external_handle = static_cast<pyrowave_os_handle>(fence_fd);
        sync_info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        sync_info.semaphore_type = VK_SEMAPHORE_TYPE_BINARY;
        sync_info.import_flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        if (pyrowave_sync_object_create(&sync_info, &sync) == PYROWAVE_SUCCESS) {
          fence_fd = -1;  // owned by the sync object now
          acquire_op.sync.semaphore = pyrowave_sync_object_get_semaphore(sync);
        } else {
          // Wait here instead: slower, but the frame is still complete. The
          // bound keeps a hung GPU from holding the encode thread forever.
          sync = nullptr;
          pollfd pfd {fence_fd, POLLIN, 0};
          int ready;
          while ((ready = ::poll(&pfd, 1, 1000)) < 0 && errno == EINTR) {
          }
          if (ready == 0) {
            BOOST_LOG(warning) << "[PyroWave] The conversion did not finish within a second; encoding the frame anyway."sv;
          }
          drop_fence();
        }
      }

      out = buffers;
      acquire = &acquire_op;
      release = &release_op;
      return true;
    }

    void encoded() override {
      // The encode that waited on the semaphore has completed by now, so it
      // can be destroyed; destroying it any earlier would be invalid Vulkan.
      drop_sync();
    }

  private:
    /**
     * A sync_file that signals when the conversion finished writing, or -1
     * after waiting for it here.
     */
    int export_fence() {
      if (dup_native_fence_fd) {
        const EGLAttrib attribs[] {EGL_NONE};
        EGLSync fence = eglCreateSync(display.get(), sync_native_fence_android, attribs);
        if (fence != EGL_NO_SYNC) {
          // The fence only gets a file descriptor once it has been flushed.
          gl::ctx.Flush();
          const int fd = dup_native_fence_fd(display.get(), fence);
          eglDestroySync(display.get(), fence);
          if (fd >= 0) {
            return fd;
          }
        }
      }
      gl::ctx.Finish();
      return -1;
    }

    bool import_targets(int out_width, int out_height, bool linear) {
      nv12 = egl::nv12_t {};
      auto *native = reinterpret_cast<gbm_device *>(gbm.get());
      if (!allocate(native, luma, out_width, out_height, DRM_FORMAT_R8, linear) || !allocate(native, chroma, out_width / 2, out_height / 2, DRM_FORMAT_GR88, linear)) {
        return false;
      }

      std::array<file_t, egl::nv12_img_t::num_fds> fds;
      fds[0] = gbm_bo_get_fd(luma.bo);
      fds[1] = gbm_bo_get_fd(chroma.bo);
      if (fds[0].el < 0 || fds[1].el < 0) {
        return false;
      }
      const auto luma_sd = describe(luma, fds[0].el);
      const auto chroma_sd = describe(chroma, fds[1].el);
      auto nv12_opt = egl::import_target(display.get(), std::move(fds), luma_sd, chroma_sd);
      if (!nv12_opt) {
        return false;
      }

      // GR88 keeps Cb in the low byte, which Vulkan names R8G8.
      if (!import_plane(pyro.get(), luma, VK_FORMAT_R8_UNORM) || !import_plane(pyro.get(), chroma, VK_FORMAT_R8G8_UNORM)) {
        BOOST_LOG(info) << "[PyroWave] Vulkan could not import the "sv << (linear ? "linear" : "driver-chosen")
                        << " layout (modifier "sv << util::hex(luma.modifier).to_string_view() << ')';
        return false;
      }

      // Cb and Cr are read from one interleaved image through swizzles.
      if (!succeeded(pyrowave_image_get_image_view(luma.image, VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &buffers.planes[0]), "luma view"sv) || !succeeded(pyrowave_image_get_image_view(chroma.image, VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &buffers.planes[1]), "Cb view"sv) || !succeeded(pyrowave_image_get_image_view(chroma.image, VK_IMAGE_ASPECT_PLANE_2_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &buffers.planes[2]), "Cr view"sv)) {
        return false;
      }

      // The planes change hands between OpenGL and Vulkan every frame.
      references[0] = {luma.image, VK_QUEUE_FAMILY_EXTERNAL};
      references[1] = {chroma.image, VK_QUEUE_FAMILY_EXTERNAL};
      acquire_op = {references.data(), references.size(), {}};
      release_op = {references.data(), references.size(), {}};

      nv12 = std::move(*nv12_opt);
      return true;
    }

    void drop_fence() {
      if (fence_fd >= 0) {
        ::close(fence_fd);
        fence_fd = -1;
      }
    }

    void drop_sync() {
      if (sync) {
        pyrowave_sync_object_destroy(sync);
        sync = nullptr;
      }
    }

    // Declaration order is destruction order in reverse: the Vulkan device
    // outlives its images, the GBM device its buffers, and the EGL context
    // every OpenGL object.
    file_t file;
    gbm::gbm_t gbm;
    egl::display_t display;
    egl::ctx_t ctx;

    device_ptr pyro;
    plane_t luma;
    plane_t chroma;
    pyrowave_gpu_buffers buffers {};
    std::array<pyrowave_gpu_external_reference, 2> references {};
    pyrowave_gpu_sync_operation acquire_op {};
    pyrowave_gpu_sync_operation release_op {};
    pyrowave_sync_object sync {};
    int fence_fd {-1};
    bool converted {false};

    std::unique_ptr<AVFrame, av_frame_deleter_t> owned_frame;
    egl::sws_t sws;
    egl::nv12_t nv12;
    egl::rgb_t rgb;
    std::uint64_t sequence {};

    dup_native_fence_fd_fn dup_native_fence_fd {};
    bool vram {true};
    std::uint32_t ram_fourcc {};
    int width {};
    int height {};
    int offset_x {};
    int offset_y {};
  };

  namespace {
    file_t open_adapter() {
      const auto *render_device = config::video.adapter_name.empty() ? "/dev/dri/renderD128" : config::video.adapter_name.c_str();
      file_t file = ::open(render_device, O_RDWR | O_CLOEXEC);
      if (file.el < 0) {
        BOOST_LOG(error) << "[PyroWave] Could not open "sv << render_device << ": "sv << std::strerror(errno);
      }
      return file;
    }
  }  // namespace

  std::unique_ptr<gpu_encode_device_t> make_gpu_encode_device(int width, int height, file_t &&render_device, int offset_x, int offset_y) {
    auto device = std::make_unique<egl_encode_device_t>();
    if (device->init(width, height, std::move(render_device), offset_x, offset_y, true, 0)) {
      return nullptr;
    }
    return device;
  }

  std::unique_ptr<gpu_encode_device_t> make_gpu_encode_device(int width, int height, int offset_x, int offset_y) {
    file_t file = open_adapter();
    if (file.el < 0) {
      return nullptr;
    }
    return make_gpu_encode_device(width, height, std::move(file), offset_x, offset_y);
  }

  std::unique_ptr<platf::avcodec_encode_device_t> make_ram_encode_device(int width, int height, file_t &&render_device, std::uint32_t fourcc) {
    if (render_device.el >= 0) {
      auto device = std::make_unique<egl_encode_device_t>();
      if (!device->init(width, height, std::move(render_device), 0, 0, false, fourcc)) {
        return device;
      }
    }
    BOOST_LOG(warning) << "[PyroWave] Frames will be converted on the CPU, which takes several milliseconds per frame."sv;
    return std::make_unique<platf::avcodec_encode_device_t>();
  }

  std::unique_ptr<platf::avcodec_encode_device_t> make_ram_encode_device(int width, int height, std::uint32_t fourcc) {
    return make_ram_encode_device(width, height, open_adapter(), fourcc);
  }
}  // namespace pyrowave
