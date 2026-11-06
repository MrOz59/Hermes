// A GPU-free regression for the EGL import cache used by gl_cuda_vram_t.
// Compile with -std=c++17 -I. and run directly (also exercised by branch CI).
// memfds stand in for dma_bufs: identity only needs fstat()'s st_dev/st_ino.
#include "src/platform/linux/dmabuf_import_cache.h"

#include <cassert>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

namespace {
  struct image_t {
    image_t(int id, int &destroyed):
        id {id},
        destroyed {&destroyed} {
    }

    image_t(image_t &&other) noexcept:
        id {other.id},
        destroyed {other.destroyed} {
      other.destroyed = nullptr;
    }

    image_t &operator=(image_t &&) = delete;
    image_t(const image_t &) = delete;

    ~image_t() {
      if (destroyed) {
        ++*destroyed;
      }
    }

    int id;
    int *destroyed;
  };

  int make_fd() {
    const int fd = ::memfd_create("hermes-import-cache", MFD_CLOEXEC);
    assert(fd >= 0);
    return fd;
  }

  platf::dmabuf::identity_t identity_for(int fd, std::uint32_t pitch = 10240, std::uint64_t modifier = 0) {
    const int fds[4] {fd, -1, -1, -1};
    const std::uint32_t pitches[4] {pitch, 0, 0, 0};
    const std::uint32_t offsets[4] {0, 0, 0, 0};
    auto id = platf::dmabuf::identify(2560, 1440, 0x34325241 /* AR24 */, modifier, fds, pitches, offsets);
    assert(id);
    return *id;
  }
}  // namespace

int main() {
  using platf::dmabuf::identify;
  using platf::dmabuf::import_cache_t;

  const int a = make_fd();
  const int b = make_fd();
  const int c = make_fd();

  // The same buffer reached through another descriptor is the same image:
  // Hermes-KMS hands out a fresh fd for the same dma_buf on every acquire.
  const int a_again = ::dup(a);
  assert(identity_for(a) == identity_for(a_again));
  assert(identity_for(a) != identity_for(b));
  // Layout is part of the identity: same buffer, other interpretation.
  assert(identity_for(a) != identity_for(a, 10496));
  assert(identity_for(a) != identity_for(a, 10240, 0x0300000000606015ULL));

  {
    const int none[4] {-1, -1, -1, -1};
    const std::uint32_t zero[4] {};
    assert(!identify(2560, 1440, 0, 0, none, zero, zero));
    const int one[4] {a, -1, -1, -1};
    assert(!identify(0, 1440, 0, 0, one, zero, zero));
    const int closed[4] {c, -1, -1, -1};
    const int closed_fd = ::dup(c);
    ::close(closed_fd);
    const int stale[4] {closed_fd, -1, -1, -1};
    assert(!identify(2560, 1440, 0, 0, stale, zero, zero));  // fstat fails: no caching
    assert(identify(2560, 1440, 0, 0, closed, zero, zero));
  }

  int destroyed = 0;
  {
    import_cache_t<image_t> disabled;
    assert(!disabled.enabled());
    assert(!disabled.find(identity_for(a)));
    assert(!disabled.insert(identity_for(a), image_t {1, destroyed}));
    assert(destroyed == 1);  // a disabled cache never keeps an import alive
  }

  destroyed = 0;
  {
    import_cache_t<image_t> cache {2};
    assert(!cache.find(identity_for(a)));
    image_t *first = cache.insert(identity_for(a), image_t {1, destroyed});
    image_t *second = cache.insert(identity_for(b), image_t {2, destroyed});
    assert(first && second && first->id == 1 && second->id == 2);

    // Lookups through another fd hit, and inserting does not move survivors.
    assert(cache.find(identity_for(a_again)) == first);
    assert(cache.hits() == 1 && cache.misses() == 1);

    // b is now least recently used and is the one evicted for c.
    image_t *third = cache.insert(identity_for(c), image_t {3, destroyed});
    assert(destroyed == 1 && cache.size() == 2 && cache.evictions() == 1);
    assert(!cache.find(identity_for(b)));
    assert(cache.find(identity_for(a)) == first && first->id == 1);
    assert(cache.find(identity_for(c)) == third);

    // Re-inserting an identity replaces it instead of duplicating it.
    image_t *replaced = cache.insert(identity_for(c), image_t {4, destroyed});
    assert(destroyed == 2 && cache.size() == 2 && replaced->id == 4);
    assert(cache.find(identity_for(c)) == replaced);

    cache.resize(1);  // a shrinking capacity evicts least recently used first
    assert(cache.size() == 1 && destroyed == 3);
    assert(cache.find(identity_for(c)) == replaced);
    assert(!cache.find(identity_for(a)));

    cache.clear();
    assert(cache.size() == 0 && destroyed == 4);
  }

  ::close(a);
  ::close(a_again);
  ::close(b);
  ::close(c);
  std::puts("dmabuf import cache: identity, LRU eviction and ownership PASS");
  return 0;
}
