#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <poll.h>

namespace pyrowave {
  // A failed Vulkan semaphore import must still wait for conversion to finish.
  // EINTR does not renew the deadline; a timeout/error must drop the frame.
  inline bool wait_for_conversion_fence(int fd, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      const auto left = deadline - std::chrono::steady_clock::now();
      if (left <= std::chrono::steady_clock::duration::zero()) return false;
      const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
      pollfd fence {fd, POLLIN, 0};
      const int result = ::poll(&fence, 1, static_cast<int>(std::max<std::int64_t>(1, milliseconds)));
      if (result > 0) return (fence.revents & POLLIN) && !(fence.revents & (POLLERR | POLLHUP | POLLNVAL));
      if (result == 0 || errno != EINTR) return false;
    }
  }
}
