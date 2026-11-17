// A GPU-free regression for the ownership contract used by cuda_ram_t.
// Compile with -std=c++17 -I. and run directly (also exercised by branch CI).
#include "src/platform/linux/cuda_upload_lifetime.h"

#include <cassert>
#include <memory>

struct frame_t {
  explicit frame_t(int &destroyed): destroyed(destroyed) {}
  ~frame_t() { ++destroyed; }
  int &destroyed;
};

int main() {
  int destroyed = 0;
  int waits = 0;
  bool completed = false;
  auto wait = [&] {
    ++waits;
    return completed;
  };
  cuda::upload_lifetime_t<frame_t> upload;
  auto pool = std::make_shared<frame_t>(destroyed);
  assert(upload.begin(pool, wait));
  assert(waits == 0); // no previous DMA to wait for
  assert(pool.use_count() == 2); // capture pool must not reuse this frame

  auto next = std::make_shared<frame_t>(destroyed);
  assert(!upload.begin(next, wait));
  assert(pool.use_count() == 2);
  assert(next.use_count() == 1); // failed wait cannot replace the in-flight source
  assert(!upload.finish(wait));
  pool.reset();
  assert(destroyed == 0); // even dropping the pool cannot free in-flight RAM

  completed = true;
  assert(upload.begin(next, wait));
  assert(destroyed == 1);
  assert(next.use_count() == 2);
  next.reset();
  assert(destroyed == 1);
  assert(upload.finish(wait));
  assert(destroyed == 2);
  const int previous_waits = waits;
  assert(upload.finish(wait));
  assert(waits == previous_waits); // idle teardown must not wait on a stale event

  auto retained = std::make_shared<frame_t>(destroyed);
  assert(upload.begin(retained, wait));
  assert(!upload.begin({}, wait));
  assert(retained.use_count() == 2); // invalid submissions preserve the lease
  assert(upload.finish(wait));
  assert(retained.use_count() == 1); // completion returns the slot to the pool
}
