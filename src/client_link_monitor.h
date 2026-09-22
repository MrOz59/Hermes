/**
 * @file src/client_link_monitor.h
 * @brief Detects a streaming client going silent on the control connection.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace stream {

  /**
   * How long a client goes without sending anything on the control connection.
   *
   * A Moonlight-family client sends a periodic ping every 100 ms besides its
   * input, so a longer silence means its network link, or the client itself,
   * stopped for that long while the host kept streaming. That is invisible in
   * everything the host measures about its own sending, and it is what a
   * client's Wi-Fi scan or power saving looks like from here: the client
   * comes back, finds frames missing, and asks for a key frame.
   *
   * Nothing is reported until the client has sent a periodic ping, so a client
   * that stays quiet while idle is never mistaken for one that went missing.
   * The last-heard state belongs to the control thread; the totals are atomic
   * so a session can be summarized from whichever thread stops it.
   */
  class client_link_monitor_t {
  public:
    using clock = std::chrono::steady_clock;

    /**
     * One and a half periodic-ping intervals. In a real session the gaps
     * between client messages stayed within 90-110 ms and none fell between
     * 110 and 170 ms; each of that session's 14 key-frame requests followed a
     * longer one.
     */
    static constexpr std::chrono::milliseconds silence_threshold {150};

    /// A key-frame request this soon after a silence ended is attributed to it.
    static constexpr std::chrono::milliseconds attribution_window {200};

    /**
     * Record a message from the client.
     * @return The silence this message ended, when it is long enough to report.
     */
    std::optional<std::chrono::milliseconds> on_message(clock::time_point now) {
      const auto previous = last_heard;
      last_heard = now;
      if (!armed || !previous) {
        return std::nullopt;
      }

      const auto silence = std::chrono::duration_cast<std::chrono::milliseconds>(now - *previous);
      if (silence < silence_threshold) {
        return std::nullopt;
      }

      last_silence = silence;
      last_silence_end = now;
      const auto ms = static_cast<std::uint64_t>(silence.count());
      silences.fetch_add(1, std::memory_order_relaxed);
      total_ms.fetch_add(ms, std::memory_order_relaxed);
      auto longest = longest_ms.load(std::memory_order_relaxed);
      while (ms > longest && !longest_ms.compare_exchange_weak(longest, ms, std::memory_order_relaxed)) {
      }
      return silence;
    }

    /// The client sends periodic pings, so its silences mean something.
    void on_periodic_ping() {
      armed = true;
    }

    /**
     * The silence that ended within attribution_window of `now`, if any.
     * Called when the client asks for a key frame.
     */
    std::optional<std::chrono::milliseconds> silence_before(clock::time_point now) const {
      if (!last_silence_end || now - *last_silence_end > attribution_window) {
        return std::nullopt;
      }
      return last_silence;
    }

    /// How long ago the most recent silence ended, relative to `now`.
    std::chrono::milliseconds since_last_silence(clock::time_point now) const {
      return last_silence_end ?
               std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_silence_end) :
               std::chrono::milliseconds::max();
    }

    std::uint64_t silence_count() const {
      return silences.load(std::memory_order_relaxed);
    }

    std::chrono::milliseconds longest_silence() const {
      return std::chrono::milliseconds {longest_ms.load(std::memory_order_relaxed)};
    }

    std::chrono::milliseconds total_silence() const {
      return std::chrono::milliseconds {total_ms.load(std::memory_order_relaxed)};
    }

  private:
    bool armed {false};
    std::optional<clock::time_point> last_heard;
    std::optional<clock::time_point> last_silence_end;
    std::chrono::milliseconds last_silence {0};

    std::atomic<std::uint64_t> silences {0};
    std::atomic<std::uint64_t> total_ms {0};
    std::atomic<std::uint64_t> longest_ms {0};
  };

}  // namespace stream
