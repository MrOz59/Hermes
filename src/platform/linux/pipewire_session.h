/** @file src/platform/linux/pipewire_session.h
 *  @brief Video from an explicitly addressed private-session PipeWire node.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace platf::kwin {
  struct frame_t {
    std::uint32_t width {};
    std::uint32_t height {};
    std::vector<std::uint8_t> bgrx;
    std::chrono::steady_clock::time_point timestamp;
  };

  enum class frame_result_t { ready, timeout, failed };

  class video_receiver_t {
  public:
    static std::unique_ptr<video_receiver_t> open(const std::string &socket,
                                                 std::uint64_t serial,
                                                 std::uint32_t width,
                                                 std::uint32_t height,
                                                 std::uint32_t framerate,
                                                 std::string &error);
    ~video_receiver_t();
    video_receiver_t(const video_receiver_t &) = delete;
    video_receiver_t &operator=(const video_receiver_t &) = delete;
    frame_result_t next(frame_t &frame, std::chrono::milliseconds timeout, std::string &error);

  private:
    struct impl_t;
    explicit video_receiver_t(std::unique_ptr<impl_t> impl);
    std::unique_ptr<impl_t> impl;
  };
}  // namespace platf::kwin
