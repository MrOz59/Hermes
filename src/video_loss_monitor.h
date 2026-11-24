/**
 * @file src/video_loss_monitor.h
 * @brief Counts the video a client reports losing on the way to it.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

namespace stream {

  /**
   * Control message type of SS_FRAME_FEC_STATUS, client to host
   * (moonlight-common-c, Video.h). Moonlight-family clients send one to any
   * Sunshine-family host for each FEC block of a frame that lost packets on
   * the way: when the block had to be repaired from parity, and when it could
   * not be repaired and the frame was dropped. Blocks that arrive whole are
   * not reported.
   */
  constexpr std::uint16_t frame_fec_status_type = 0x5502;

  /**
   * What the client reported losing over a session, from its FEC status
   * reports.
   *
   * Everything the host measures is on its own side of the network. These
   * reports are the client's view of the same packets, so they show loss the
   * host cannot see: a congested hop, Wi-Fi, a slow port. Reports arrive on
   * the control thread; the totals are atomic so a session can be summarized
   * from whichever thread stops it.
   */
  class video_loss_monitor_t {
  public:
    /// One block's report. The wire format is big-endian.
    struct report_t {
      std::uint32_t frame_index;
      /// Packets missing below the highest sequence number received when the report was made.
      std::uint16_t missing_before_highest;
      std::uint16_t total_data;
      std::uint16_t total_parity;
      std::uint16_t received_data;
      std::uint16_t received_parity;

      /// Too few packets arrived to rebuild the block, so its frame was dropped.
      bool lost() const {
        return received_data + received_parity < total_data;
      }

      /**
       * Packets of the block the network lost.
       *
       * A dropped block is reported once the client has given up on it, so
       * everything that did not arrive was lost. A repaired block is reported
       * the moment enough packets arrived to rebuild it (moonlight-common-c,
       * reconstructFrame()); the host sends data before parity, so the
       * parity still on its way is not loss. Counting `total - received`
       * there would report about the whole parity of every repaired block,
       * and loss would grow with the parity sent. Only the gaps below the
       * highest packet received count.
       */
      std::uint32_t missing_packets() const {
        const std::uint32_t total = total_data + total_parity;
        const std::uint32_t received = received_data + received_parity;
        const std::uint32_t unreceived = total > received ? total - received : 0;
        if (lost()) {
          return unreceived;
        }
        // Data shards precede parity on the wire, so a data shard the block was
        // rebuilt without is below the highest received and counts as missing.
        const std::uint32_t data_short = total_data > received_data ? total_data - received_data : 0;
        return std::min(std::max<std::uint32_t>(missing_before_highest, data_short), unreceived);
      }
    };

    /// The size of SS_FRAME_FEC_STATUS: a 32-bit frame index, seven 16-bit counts and three bytes.
    static constexpr std::size_t report_size = 4 + 7 * 2 + 3;

    /**
     * Parse an SS_FRAME_FEC_STATUS payload.
     * @return The report, or nothing if the payload is not one.
     */
    static std::optional<report_t> parse(std::string_view payload) {
      if (payload.size() < report_size) {
        return std::nullopt;
      }
      const auto *bytes = reinterpret_cast<const unsigned char *>(payload.data());
      const auto u16 = [bytes](std::size_t at) {
        return static_cast<std::uint16_t>(bytes[at] << 8 | bytes[at + 1]);
      };
      report_t report {
        .frame_index = static_cast<std::uint32_t>(bytes[0]) << 24 | static_cast<std::uint32_t>(bytes[1]) << 16 |
                       static_cast<std::uint32_t>(bytes[2]) << 8 | bytes[3],
        // Bytes 4-7: highest received and next contiguous sequence numbers.
        .missing_before_highest = u16(8),
        .total_data = u16(10),
        .total_parity = u16(12),
        .received_data = u16(14),
        .received_parity = u16(16),
      };
      if (report.total_data == 0) {
        return std::nullopt;
      }
      return report;
    }

    /// Count one block's report.
    void on_report(const report_t &report) {
      reports.fetch_add(1, std::memory_order_relaxed);
      missing.fetch_add(report.missing_packets(), std::memory_order_relaxed);
      if (!report.lost()) {
        repaired.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      // A frame with several lost blocks is one lost frame.
      if (!last_lost_frame || *last_lost_frame != report.frame_index) {
        last_lost_frame = report.frame_index;
        lost_frames.fetch_add(1, std::memory_order_relaxed);
      }
    }

    /**
     * Count one FEC block sent to the client. Called by the video thread.
     * @param packets The block's packets, parity included.
     * @param data_packets How many of them carry data.
     */
    void on_sent(std::uint64_t packets, std::uint64_t data_packets) {
      sent.fetch_add(packets, std::memory_order_relaxed);
      sent_data.fetch_add(data_packets, std::memory_order_relaxed);
    }

    /// Video packets sent to the client, parity included.
    std::uint64_t sent_packets() const {
      return sent.load(std::memory_order_relaxed);
    }

    /**
     * Data packets sent to the client: what a loss rate is measured against.
     *
     * The client watches a block until it can rebuild it, which takes as many
     * packets as the block has data. Parity of a block whose data all arrived,
     * and parity behind the packet that completed a rebuild, are never
     * looked at, so their losses are never reported. Dividing the reported
     * loss by every packet sent would underestimate the loss rate by the
     * parity share; dividing by the data packets does not.
     */
    std::uint64_t sent_data_packets() const {
      return sent_data.load(std::memory_order_relaxed);
    }

    /// Blocks the client rebuilt from parity: lost packets, no lost picture.
    std::uint64_t repaired_blocks() const {
      return repaired.load(std::memory_order_relaxed);
    }

    /// Frames the client dropped because a block could not be rebuilt.
    std::uint64_t dropped_frames() const {
      return lost_frames.load(std::memory_order_relaxed);
    }

    /// Packets missing from the blocks the client reported.
    std::uint64_t missing_packets() const {
      return missing.load(std::memory_order_relaxed);
    }

    /// Blocks the client reported on. Zero means it never sent SS_FRAME_FEC_STATUS,
    /// which is not the same as a link that lost nothing.
    std::uint64_t reported_blocks() const {
      return reports.load(std::memory_order_relaxed);
    }

  private:
    std::optional<std::uint32_t> last_lost_frame;

    std::atomic<std::uint64_t> repaired {0};
    std::atomic<std::uint64_t> lost_frames {0};
    std::atomic<std::uint64_t> missing {0};
    std::atomic<std::uint64_t> sent {0};
    std::atomic<std::uint64_t> sent_data {0};
    std::atomic<std::uint64_t> reports {0};
  };

}  // namespace stream
