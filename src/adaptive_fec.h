/**
 * @file src/adaptive_fec.h
 * @brief Picks the FEC percentage for the next frame from what the client reports losing.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace stream {

  /**
   * Chooses how much parity the next frame gets.
   *
   * The client already tells us what it lost: SS_FRAME_FEC_STATUS (0x5502) reports
   * every block that needed parity or was dropped, and video_loss_monitor_t turns
   * those into cumulative packet counters. This class samples those counters once
   * per second into a ten-second window and keeps the *worst* second, because
   * picture loss is caused by bursts, not by the average.
   *
   * The decision asks for the smallest redundancy whose chance of leaving a block
   * unrecoverable is at most 0.1% at the observed loss rate. Raising is immediate;
   * lowering waits five seconds, so a single clean second does not start a
   * redundancy sawtooth.
   *
   * Header-only and dependency-free so it can be unit tested without a session.
   */
  class adaptive_fec_t {
  public:
    /// Seconds of history the estimate looks at.
    static constexpr int window_seconds = 10;
    /// Never ask for less than this, and never more than the configured ceiling.
    static constexpr int min_percentage = 1;
    static constexpr int max_percentage = 50;
    /// Target chance that any one FEC block cannot be rebuilt.
    static constexpr double target_unrecoverable = 0.001;
    /// Data shards assumed per block when turning a target into a percentage.
    static constexpr int representative_data_shards = 10;
    /// A second needs at least this many sent packets before its ratio is trusted.
    static constexpr std::uint64_t min_packets_per_bucket = 100;
    /// How long a lower estimate must hold before it is applied.
    static constexpr std::int64_t lower_cooldown_ms = 5000;
    /// A bucket must cover this much time before its ratio is trusted.
    static constexpr std::int64_t bucket_span_ms = 1000;
    /// A bucket cut short by a clock boundary still counts if it is this close to a full second.
    ///
    /// Rollovers happen when a frame lands in the next second, so with jitter a bucket
    /// can cover 998 ms; demanding a whole second would throw that away and the next
    /// bucket would start short as well, pushing the first estimate out by seconds.
    static constexpr std::int64_t min_coverage_ms = bucket_span_ms * 3 / 4;

  private:
    struct bucket_t {
      std::int64_t second {-1};
      std::uint64_t sent {0};
      std::uint64_t missing {0};
      /// How much time the delta above actually covers; a sliver is not a sample.
      std::int64_t covered_ms {0};
    };

  public:

    /**
     * The percentage to encode the next frame with.
     * @param sent_total Packets sent so far, parity included (video_loss_monitor_t::sent_packets()).
     * @param missing_total Packets the client reported missing (video_loss_monitor_t::missing_packets()).
     * @param reported_total Blocks the client reported on (video_loss_monitor_t::reported_blocks()).
     *        Zero means the client has never sent SS_FRAME_FEC_STATUS, which is not
     *        evidence of a clean link: the configured value stands.
     * @param now_ms Monotonic milliseconds.
     * @param configured The operator's configured percentage, used until the window has data.
     * @param ceiling The highest percentage this session may ask for.
     */
    int percentage(std::uint64_t sent_total, std::uint64_t missing_total, std::uint64_t reported_total, std::int64_t now_ms,
                 int configured, int ceiling) {
      const auto bound = std::clamp(ceiling, min_percentage, max_percentage);

      if (reported_total == 0) {
        // Traffic alone proves nothing: on_sent() counts every block the host transmits,
        // while only the client's reports say what arrived. Gating on the sent counter
        // would make a client that never reports look like a perfect link.
        current = std::clamp(configured, min_percentage, bound);
        last_second = -1;
        return current;
      }

      const auto second = now_ms / 1000;

      if (last_second < 0) {
        // First call: take a baseline instead of counting the whole session as one second.
        last_sent = sent_total;
        last_missing = missing_total;
        last_second = second;
        last_rollover_ms = now_ms;
        current = std::clamp(configured, min_percentage, bound);
        return current;
      }

      if (second != last_second) {
        const auto delta_sent = sent_total - last_sent;
        const auto delta_missing = missing_total - last_missing;
        const auto covered_ms = now_ms - last_rollover_ms;
        last_sent = sent_total;
        last_missing = missing_total;
        last_rollover_ms = now_ms;

        // Attribute the whole elapsed second to its own bucket. If the thread stalled
        // for several seconds, the sample lands in the newest bucket and the older
        // ones keep whatever they had, so a stall cannot fake a clean window.
        auto &bucket = buckets[static_cast<std::size_t>(second % window_seconds)];
        bucket.second = second;
        bucket.sent = delta_sent;
        bucket.missing = delta_missing;
        bucket.covered_ms = covered_ms;
        last_second = second;
      }

      if (!has_sample(second)) {
        // Either nothing has been sampled yet, or the last trustworthy second has aged
        // out of the window. Hold the last decision: silence is not a clean link, and
        // dropping back to the configured value here would hand back parity without a
        // cooldown after a stall.
        return current;
      }

      if (last_lowered_ms < 0) {
        // The first trustworthy second starts the clock: the configured value is held
        // for a full cooldown before anything is given back.
        last_lowered_ms = now_ms;
      }

      const auto loss = worst_loss(second);
      const auto required = percentage_for(loss, bound);

      if (required > current) {
        current = required;
        // A raise restarts the hold, so the next reduction waits for five quiet
        // seconds after the last one rather than counting time since the last drop.
        last_lowered_ms = now_ms;
        return current;
      }
      if (required < current && now_ms - last_lowered_ms >= lower_cooldown_ms) {
        current = required;
        last_lowered_ms = now_ms;
      }
      return current;
    }

    /// True once some second in the window carried enough packets to judge.
    bool has_sample(std::int64_t second) const {
      for (const auto &bucket : buckets) {
        if (trusted(bucket, second)) {
          return true;
        }
      }
      return false;
    }

    /// The worst packet-loss ratio in the window, 0 when no bucket is trustworthy.
    double worst_loss(std::int64_t second) const {
      double worst = 0;
      for (const auto &bucket : buckets) {
        if (!trusted(bucket, second)) {
          continue;
        }
        const auto ratio = static_cast<double>(bucket.missing) / static_cast<double>(bucket.sent);
        worst = std::max(worst, ratio);
      }
      return worst;
    }

    /**
     * Whether a bucket may influence the estimate.
     *
     * It must carry enough packets, span close to a whole second (crossing a clock-second
     * boundary is not the same as sampling one), and still be inside the window:
     * a stall must not let a sample from twenty seconds ago keep parity high.
     */
    static bool trusted(const bucket_t &bucket, std::int64_t second) {
      if (bucket.second < 0 || bucket.sent < min_packets_per_bucket) {
        return false;
      }
      if (bucket.covered_ms < min_coverage_ms) {
        return false;
      }
      return bucket.second <= second && second - bucket.second < window_seconds;
    }

    /**
     * The smallest percentage whose unrecoverable-block chance stays under target.
     * @param loss Packet loss ratio in [0, 1].
     * @param ceiling Highest percentage allowed.
     */
    static int percentage_for(double loss, int ceiling) {
      const auto bound = std::clamp(ceiling, min_percentage, max_percentage);
      if (!(loss > 0)) {
        return min_percentage;
      }
      const auto n = representative_data_shards;
      for (auto parity = 1; parity <= bound; ++parity) {
        if (unrecoverable_chance(n, parity, loss) <= target_unrecoverable) {
          // Round up so the percentage never undershoots the chosen parity count.
          const auto percentage = static_cast<int>(std::ceil(100.0 * parity / n));
          return std::clamp(percentage, min_percentage, bound);
        }
      }
      return bound;
    }

    /// P(more than @p parity packets of an (n + parity) block are lost).
    static double unrecoverable_chance(int data, int parity, double loss) {
      const auto total = data + parity;
      const auto p = std::clamp(loss, 0.0, 1.0);
      // Sum the upper tail: P(X > parity) for X ~ Binomial(total, p).
      double tail = 0;
      for (auto lost = parity + 1; lost <= total; ++lost) {
        tail += binomial(total, lost) * std::pow(p, lost) * std::pow(1 - p, total - lost);
      }
      return tail;
    }

  private:
    static double binomial(int n, int k) {
      double result = 1;
      for (auto i = 1; i <= k; ++i) {
        result = result * (n - k + i) / i;
      }
      return result;
    }

    std::array<bucket_t, window_seconds> buckets {};
    std::uint64_t last_sent {0};
    std::uint64_t last_missing {0};
    std::int64_t last_second {-1};
    std::int64_t last_rollover_ms {0};
    /// Negative until the first trustworthy sample; it then holds the last reduction.
    std::int64_t last_lowered_ms {-1};
    int current {0};
  };

}  // namespace stream
