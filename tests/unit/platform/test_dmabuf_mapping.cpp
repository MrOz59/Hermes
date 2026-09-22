/**
 * @file tests/unit/platform/test_dmabuf_mapping.cpp
 * @brief Persistent DMA-BUF mappings and the blocked copy used by CPU capture.
 */
#include "../../tests_common.h"

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <numeric>
#include <src/platform/linux/dmabuf_mapping.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {
  using platf::dmabuf::copy_rows;
  using platf::dmabuf::mapping_cache_t;

  /// An anonymous file of `size` bytes whose byte i holds `(i * 7 + seed) & 0xff`.
  int patterned_memfd(std::size_t size, unsigned seed = 0) {
    const int fd = ::memfd_create("hermes-dmabuf-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(size)) < 0) {
      return -1;
    }
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
      bytes[i] = static_cast<std::uint8_t>((i * 7 + seed) & 0xff);
    }
    if (::pwrite(fd, bytes.data(), size, 0) != static_cast<ssize_t>(size)) {
      ::close(fd);
      return -1;
    }
    return fd;
  }

  TEST(DmabufCopyRows, ContiguousImageIsCopiedAcrossBlockBoundaries) {
    std::vector<std::uint8_t> src(4 * 25);
    std::iota(src.begin(), src.end(), std::uint8_t {1});
    std::vector<std::uint8_t> dst(src.size(), 0);

    // 7-byte blocks never line up with the 4-byte rows or the 100-byte total.
    copy_rows(dst.data(), 4, src.data(), 4, 4, 25, 7);

    EXPECT_EQ(dst, src);
  }

  TEST(DmabufCopyRows, PitchedRowsLeavePaddingUntouched) {
    constexpr std::size_t row = 6, src_pitch = 8, dst_pitch = 10, rows = 5;
    std::vector<std::uint8_t> src(src_pitch * rows);
    std::iota(src.begin(), src.end(), std::uint8_t {0});
    std::vector<std::uint8_t> dst(dst_pitch * rows, 0xee);

    copy_rows(dst.data(), dst_pitch, src.data(), src_pitch, row, rows);

    for (std::size_t y = 0; y < rows; ++y) {
      for (std::size_t x = 0; x < dst_pitch; ++x) {
        const auto expected = x < row ? src[y * src_pitch + x] : std::uint8_t {0xee};
        EXPECT_EQ(dst[y * dst_pitch + x], expected) << "row " << y << " byte " << x;
      }
    }
  }

  TEST(DmabufMappingCache, SameBufferIsMappedOnce) {
    const int fd = patterned_memfd(64 * 1024);
    ASSERT_GE(fd, 0);
    const int dup_fd = ::dup(fd);
    ASSERT_GE(dup_fd, 0);
    mapping_cache_t cache;

    const auto *first = cache.map(fd, 64 * 1024);
    ASSERT_NE(first, nullptr);
    // A fresh descriptor for the same buffer - what every ACQUIRE_FRAME of
    // an unchanged framebuffer returns - must reuse the mapping.
    const auto *second = cache.map(dup_fd, 1);

    EXPECT_EQ(second, first);
    EXPECT_EQ(cache.misses(), 1u);
    EXPECT_EQ(cache.hits(), 1u);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(first[10], (10 * 7) & 0xff);
    ::close(dup_fd);
    ::close(fd);
  }

  TEST(DmabufMappingCache, MappingStaysValidAfterTheDescriptorCloses) {
    int fd = patterned_memfd(8192, 3);
    ASSERT_GE(fd, 0);
    mapping_cache_t cache;
    const auto *map = cache.map(fd, 8192);
    ASSERT_NE(map, nullptr);

    // Capture closes the frame's descriptors after every copy.
    ::close(fd);

    EXPECT_EQ(map[8191], (8191 * 7 + 3) & 0xff);
  }

  TEST(DmabufMappingCache, LaterWritesToTheBufferAreVisible) {
    const int fd = patterned_memfd(4096);
    ASSERT_GE(fd, 0);
    mapping_cache_t cache;
    const auto *map = cache.map(fd, 4096);
    ASSERT_NE(map, nullptr);

    // The producer renders the next frame into the same buffer.
    const std::uint8_t next = 0xa5;
    ASSERT_EQ(::pwrite(fd, &next, 1, 100), 1);

    EXPECT_EQ(cache.map(fd, 4096), map);
    EXPECT_EQ(map[100], next);
    ::close(fd);
  }

  TEST(DmabufMappingCache, LeastRecentlyUsedBufferIsEvicted) {
    std::vector<int> fds;
    for (unsigned i = 0; i < 4; ++i) {
      fds.push_back(patterned_memfd(4096, i));
      ASSERT_GE(fds.back(), 0);
    }
    mapping_cache_t cache {3};

    ASSERT_NE(cache.map(fds[0], 1), nullptr);
    ASSERT_NE(cache.map(fds[1], 1), nullptr);
    ASSERT_NE(cache.map(fds[2], 1), nullptr);
    ASSERT_NE(cache.map(fds[0], 1), nullptr);  // 1 is now the oldest
    ASSERT_NE(cache.map(fds[3], 1), nullptr);  // evicts 1
    EXPECT_EQ(cache.size(), 3u);
    EXPECT_EQ(cache.misses(), 4u);

    ASSERT_NE(cache.map(fds[0], 1), nullptr);
    ASSERT_NE(cache.map(fds[2], 1), nullptr);
    EXPECT_EQ(cache.misses(), 4u) << "0 and 2 must still be cached";
    ASSERT_NE(cache.map(fds[1], 1), nullptr);
    EXPECT_EQ(cache.misses(), 5u) << "1 must have been evicted";

    cache.clear();
    EXPECT_EQ(cache.size(), 0u);
    for (const int fd : fds) {
      ::close(fd);
    }
  }

  TEST(DmabufMappingCache, BufferSmallerThanTheFrameIsRefused) {
    const int fd = patterned_memfd(4096);
    ASSERT_GE(fd, 0);
    mapping_cache_t cache;

    errno = 0;
    EXPECT_EQ(cache.map(fd, 4097), nullptr);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(cache.size(), 0u);
    ::close(fd);
  }

  TEST(DmabufMappingCache, BadDescriptorFails) {
    mapping_cache_t cache;
    errno = 0;
    EXPECT_EQ(cache.map(-1, 1), nullptr);
    EXPECT_EQ(errno, EBADF);
  }

  // The same contract against a real DMA-BUF, including the CPU-access
  // bracketing capture performs around every copy.
  TEST(DmabufMappingCache, RealDmabufThroughUdmabuf) {
    const int dev = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (dev < 0) {
      GTEST_SKIP() << "/dev/udmabuf unavailable: " << std::strerror(errno);
    }
    constexpr std::size_t size = 4 * 4096;
    const int memfd = patterned_memfd(size, 9);
    ASSERT_GE(memfd, 0);
    ASSERT_EQ(::fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK), 0);
    udmabuf_create create {};
    create.memfd = static_cast<__u32>(memfd);
    create.size = size;
    const int dmabuf = ::ioctl(dev, UDMABUF_CREATE, &create);
    ::close(dev);
    if (dmabuf < 0) {
      ::close(memfd);
      GTEST_SKIP() << "UDMABUF_CREATE failed: " << std::strerror(errno);
    }

    mapping_cache_t cache;
    const auto *map = cache.map(dmabuf, size);
    ASSERT_NE(map, nullptr);
    dma_buf_sync sync {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    ASSERT_EQ(::ioctl(dmabuf, DMA_BUF_IOCTL_SYNC, &sync), 0);
    std::vector<std::uint8_t> copy(size);
    copy_rows(copy.data(), 4096, map, 4096, 4096, 4);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ASSERT_EQ(::ioctl(dmabuf, DMA_BUF_IOCTL_SYNC, &sync), 0);

    for (std::size_t i = 0; i < size; i += 1021) {
      EXPECT_EQ(copy[i], (i * 7 + 9) & 0xff) << "byte " << i;
    }
    EXPECT_EQ(cache.map(dmabuf, size), map);
    EXPECT_EQ(cache.hits(), 1u);
    ::close(dmabuf);
    ::close(memfd);
  }
}  // namespace
