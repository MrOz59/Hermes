/**
 * @file src/platform/linux/dmabuf_import_cache.h
 * @brief GPU imports of DMA-BUFs, kept across frames by buffer identity.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace platf::dmabuf {

  /**
   * What makes two DMA-BUF descriptors the same GPU image: the dma_buf behind
   * every plane and the layout it is interpreted with.
   *
   * Since Linux 5.19 (backported to 5.15.y) every dma_buf takes its inode
   * number from a 64-bit counter that is never reused ("dma-buf: ensure unique
   * directory name for dmabuf stats"), so (st_dev, st_ino) names one buffer
   * for the lifetime of the system. A stale entry can therefore only miss; it
   * can never be mistaken for a newer buffer. Hermes-KMS re-exports the same
   * dma_buf for a framebuffer on every ACQUIRE_FRAME, PRIME-imported
   * GPU-origin buffers included, which is what makes hits possible at all.
   *
   * The layout is part of the identity because an EGLImage is created for
   * one interpretation of the bytes: the same buffer offered with another
   * pitch, offset, format or modifier must be imported again.
   */
  struct identity_t {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t fourcc {};
    std::uint64_t modifier {};
    std::array<dev_t, 4> device {};
    std::array<ino_t, 4> inode {};
    std::array<std::uint32_t, 4> pitch {};
    std::array<std::uint32_t, 4> offset {};
    std::array<bool, 4> present {};

    friend bool operator==(const identity_t &a, const identity_t &b) {
      return a.width == b.width && a.height == b.height && a.fourcc == b.fourcc &&
             a.modifier == b.modifier && a.device == b.device && a.inode == b.inode &&
             a.pitch == b.pitch && a.offset == b.offset && a.present == b.present;
    }

    friend bool operator!=(const identity_t &a, const identity_t &b) {
      return !(a == b);
    }
  };

  /**
   * Identify a descriptor in the egl::surface_descriptor_t shape. Planes
   * whose fd is negative are absent, matching how EGL attributes are built.
   * @return The identity, or nullopt when there is no plane, the geometry is
   *         invalid or a plane cannot be stat()ed. Callers then import the
   *         frame without caching it, exactly as before the cache existed.
   */
  inline std::optional<identity_t> identify(
    int width,
    int height,
    std::uint32_t fourcc,
    std::uint64_t modifier,
    const int (&fds)[4],
    const std::uint32_t (&pitches)[4],
    const std::uint32_t (&offsets)[4]
  ) {
    if (width <= 0 || height <= 0) {
      return std::nullopt;
    }

    identity_t id;
    id.width = static_cast<std::uint32_t>(width);
    id.height = static_cast<std::uint32_t>(height);
    id.fourcc = fourcc;
    id.modifier = modifier;

    bool any_plane = false;
    for (std::size_t plane = 0; plane < 4; ++plane) {
      if (fds[plane] < 0) {
        continue;
      }
      struct stat st;
      if (::fstat(fds[plane], &st) < 0) {
        return std::nullopt;
      }
      id.present[plane] = true;
      id.device[plane] = st.st_dev;
      id.inode[plane] = st.st_ino;
      id.pitch[plane] = pitches[plane];
      id.offset[plane] = offsets[plane];
      any_plane = true;
    }
    if (!any_plane) {
      return std::nullopt;
    }
    return id;
  }

  /**
   * GPU imports (EGLImage + texture) of the DMA-BUFs a producer rotates
   * through, kept across frames.
   *
   * A compositor scans out two to four buffers in rotation, so importing on
   * every frame repeats the same work: an eglCreateImage, a texture bind and
   * an eglDestroyImage per frame. On NVIDIA (595.71 through at least 610.43)
   * importing a buffer another process allocated and then sampling it also
   * leaves importer VRAM behind after the image is destroyed, reclaimed only
   * at process exit (NVIDIA forum report "VRAM not reclaimed for sampled
   * cross-process dma_buf imports"). That is exactly what capturing a
   * compositor's scanout does, so per-frame imports grow toward that pool;
   * one import per buffer does not.
   *
   * A cached image keeps referencing the buffer, which can keep a buffer the
   * compositor released alive until the entry is evicted. The cache is
   * therefore small and owned by one encode device, so a reinit drops it.
   *
   * Whether sampling a cached image sees what the producer wrote since the
   * import is a driver property, not something this class can establish:
   * Hermes-KMS's tools/hermes-import-cache-check measures it on a given
   * driver, with a second process rendering known patterns into the buffers
   * while kept and fresh imports are checked pixel by pixel.
   *
   * Destroying an Image must be valid wherever find(), insert() or clear()
   * run; for EGL that means on the thread with the context current.
   */
  template<class Image>
  class import_cache_t {
  public:
    explicit import_cache_t(std::size_t capacity = 0):
        capacity {capacity} {
    }

    import_cache_t(const import_cache_t &) = delete;
    import_cache_t &operator=(const import_cache_t &) = delete;

    /** A capacity of zero disables caching: find() always misses. */
    bool enabled() const {
      return capacity > 0;
    }

    /**
     * Change the capacity, evicting least recently used entries if it shrank.
     */
    void resize(std::size_t new_capacity) {
      capacity = new_capacity;
      while (entries.size() > capacity) {
        evict_oldest();
      }
    }

    /**
     * The image imported earlier for this identity, marked most recently used.
     * The pointer stays valid until that entry is evicted or cleared.
     */
    Image *find(const identity_t &id) {
      if (!enabled()) {
        return nullptr;
      }
      for (auto &entry : entries) {
        if (entry->id == id) {
          entry->last_use = ++clock;
          ++hit_count;
          return &entry->image;
        }
      }
      ++miss_count;
      return nullptr;
    }

    /**
     * Keep a new import, evicting the least recently used entry when full.
     * An identity already present is replaced rather than duplicated.
     * @return The stored image, or nullptr (image destroyed) when disabled.
     */
    Image *insert(const identity_t &id, Image &&image) {
      if (!enabled()) {
        return nullptr;
      }
      auto existing = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
        return entry->id == id;
      });
      if (existing != entries.end()) {
        entries.erase(existing);
      }
      while (entries.size() >= capacity) {
        evict_oldest();
      }
      entries.push_back(std::make_unique<entry_t>(entry_t {id, std::move(image), ++clock}));
      return &entries.back()->image;
    }

    /** Drop every import, releasing the buffers they kept alive. */
    void clear() {
      entries.clear();
    }

    std::size_t size() const {
      return entries.size();
    }

    std::uint64_t hits() const {
      return hit_count;
    }

    std::uint64_t misses() const {
      return miss_count;
    }

    std::uint64_t evictions() const {
      return eviction_count;
    }

  private:
    struct entry_t {
      identity_t id;
      Image image;
      std::uint64_t last_use;
    };

    void evict_oldest() {
      if (entries.empty()) {
        return;
      }
      auto oldest = std::min_element(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return a->last_use < b->last_use;
      });
      entries.erase(oldest);
      ++eviction_count;
    }

    std::size_t capacity;
    // unique_ptr keeps every surviving entry's address stable across inserts.
    std::vector<std::unique_ptr<entry_t>> entries;
    std::uint64_t clock {0};
    std::uint64_t hit_count {0};
    std::uint64_t miss_count {0};
    std::uint64_t eviction_count {0};
  };

}  // namespace platf::dmabuf
