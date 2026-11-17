/**
 * @file src/platform/linux/cuda_upload_lifetime.h
 * @brief Keeps a capture-pool frame leased until its asynchronous upload completes.
 */
#pragma once

#include <memory>
#include <utility>

namespace cuda {
  /**
   * The capture pool may write a frame only when it owns the last reference.
   * Retaining the source here prevents reuse while DMA still reads its pixels.
   * The caller owns the completion event and must finish the lease before
   * destroying that event or the CUDA context. Failed waits keep the lease.
   */
  template<class Frame>
  class upload_lifetime_t {
  public:
    template<class Wait>
    bool begin(std::shared_ptr<Frame> next, Wait &&wait) {
      if (!next || !finish(std::forward<Wait>(wait))) {
        return false;
      }
      source = std::move(next);
      return true;
    }

    template<class Wait>
    bool finish(Wait &&wait) {
      if (source && !wait()) {
        return false;
      }
      source.reset();
      return true;
    }

  private:
    std::shared_ptr<Frame> source;
  };
}  // namespace cuda
