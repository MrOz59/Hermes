/**
 * @file src/platform/linux/dmabuf_mapping.h
 * @brief Persistent CPU mappings of DMA-BUFs, and a copy suited to reading them.
 */
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace platf::dmabuf {

  /**
   * Read-only mappings of the DMA-BUFs a producer rotates through, kept
   * across frames.
   *
   * Mapping a scanout buffer for every frame costs more than copying it. A
   * drm_gem_shmem export is mapped VM_PFNMAP and faults in one 4 KiB page at a
   * time: about 5 ms of a 6.5 ms 1440p CPU capture, 3600 faults per frame. A
   * compositor rotates through two or three buffers, and Hermes-KMS hands back
   * the same dma_buf for the same framebuffer, so a handful of cached mappings
   * leaves every frame after the first free of faults.
   *
   * A mapping holds its own reference to the dma_buf, so the inode keying an
   * entry cannot be reused by another buffer while the entry exists. The
   * reference also keeps a buffer the compositor has released alive until the
   * entry is evicted, which is why the cache is small and cleared on reinit.
   */
  class mapping_cache_t {
  public:
    explicit mapping_cache_t(std::size_t capacity = 4):
        capacity {std::max<std::size_t>(capacity, 1)} {
    }

    ~mapping_cache_t() {
      clear();
    }

    mapping_cache_t(const mapping_cache_t &) = delete;
    mapping_cache_t &operator=(const mapping_cache_t &) = delete;

    /**
     * Map the whole DMA-BUF behind fd, or return the mapping made earlier.
     * @param fd Any descriptor for the DMA-BUF; it is not retained.
     * @param required Bytes from the start of the buffer the caller will read.
     * @return The mapping, or nullptr with errno set. A buffer smaller than
     *         `required` is refused with EINVAL instead of being mapped.
     */
    const std::uint8_t *map(int fd, std::size_t required) {
      struct stat st {};
      if (::fstat(fd, &st) < 0) {
        return nullptr;
      }
      if (st.st_size <= 0 ||
          static_cast<std::uintmax_t>(required) > static_cast<std::uintmax_t>(st.st_size)) {
        errno = EINVAL;
        return nullptr;
      }
      const auto length = static_cast<std::size_t>(st.st_size);

      ++clock;
      for (auto &entry : entries) {
        if (entry.device == st.st_dev && entry.inode == st.st_ino && entry.length == length) {
          entry.last_use = clock;
          ++hit_count;
          return entry.address;
        }
      }

      void *address = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
      if (address == MAP_FAILED) {
        return nullptr;
      }
      if (entries.size() >= capacity) {
        auto oldest = std::min_element(entries.begin(), entries.end(), [](const entry_t &a, const entry_t &b) {
          return a.last_use < b.last_use;
        });
        ::munmap(oldest->address, oldest->length);
        entries.erase(oldest);
      }
      entries.push_back({st.st_dev, st.st_ino, static_cast<std::uint8_t *>(address), length, clock});
      ++miss_count;
      return static_cast<std::uint8_t *>(address);
    }

    /** Drop every mapping, releasing the buffers they kept alive. */
    void clear() {
      for (const auto &entry : entries) {
        ::munmap(entry.address, entry.length);
      }
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

  private:
    struct entry_t {
      dev_t device;
      ino_t inode;
      std::uint8_t *address;
      std::size_t length;
      std::uint64_t last_use;
    };

    std::size_t capacity;
    std::vector<entry_t> entries;
    std::uint64_t clock {0};
    std::uint64_t hit_count {0};
    std::uint64_t miss_count {0};
  };

  /**
   * Largest single memcpy used when copying a contiguous image. One glibc
   * memcpy of a whole 4K frame (31.6 MiB) switches to its large-copy strategy,
   * which reads a VM_PFNMAP DMA-BUF mapping at a third of the speed: 9 ms
   * instead of 3 ms on a Ryzen 7 5700X. Blocks from 1 to 16 MiB all avoid it;
   * 2 MiB stays under the threshold on CPUs with a smaller L3 as well.
   */
  constexpr std::size_t copy_block_bytes = std::size_t {2} << 20;

  /**
   * Copy `rows` rows of `row_bytes` from a pitched source to a pitched
   * destination, never in a single memcpy larger than `block_bytes`.
   */
  inline void copy_rows(
    std::uint8_t *dst,
    std::size_t dst_pitch,
    const std::uint8_t *src,
    std::size_t src_pitch,
    std::size_t row_bytes,
    std::size_t rows,
    std::size_t block_bytes = copy_block_bytes
  ) {
    if (src_pitch == row_bytes && dst_pitch == row_bytes) {
      const std::size_t total = row_bytes * rows;
      block_bytes = std::max<std::size_t>(block_bytes, 1);
      for (std::size_t offset = 0; offset < total; offset += block_bytes) {
        std::memcpy(dst + offset, src + offset, std::min(block_bytes, total - offset));
      }
      return;
    }
    for (std::size_t y = 0; y < rows; ++y) {
      std::memcpy(dst + y * dst_pitch, src + y * src_pitch, row_bytes);
    }
  }

}  // namespace platf::dmabuf
