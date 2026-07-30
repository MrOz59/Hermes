/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  struct session_t;

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    /**
     * @brief Why a streaming session ended.
     *
     * Lets the host distinguish a client that dropped (network loss, crash)
     * from one the user or server stopped on purpose, instead of collapsing
     * every path into an indistinct "CLIENT DISCONNECTED".
     */
    enum class termination_reason_e : int {
      UNKNOWN,  ///< No reason recorded yet
      CLIENT_QUIT,  ///< Client closed the stream (ENet disconnect)
      CLIENT_LOST,  ///< Client stopped responding (ping timeout / network loss)
      HANDSHAKE_FAILED,  ///< Video/audio handshake never completed
      PROTOCOL_ERROR,  ///< Malformed/failed control-stream exchange
      SERVER_STOPPED,  ///< Host stopped the session (API stop, shutdown)
      PERMISSION_REVOKED,  ///< View permission was revoked mid-session
    };

    std::string_view termination_reason_str(termination_reason_e reason);
    termination_reason_e last_termination_reason();

    /**
     * @brief Aggregate reconnection/termination stats for diagnostics.
     *
     * Lets the UI distinguish "a client just dropped" from "the last session
     * ended cleanly a while ago", and surface how often clients are dropping
     * (a network-quality signal) without scraping logs.
     */
    struct termination_stats_t {
      termination_reason_e last_reason {termination_reason_e::UNKNOWN};
      /// Steady-clock ms since the last session ended; 0 if none has ended yet.
      uint64_t ms_since_last_end {0};
      uint64_t total_ended {0};  ///< Sessions that have ended this run.
      uint64_t client_lost_count {0};  ///< Of those, ended by client loss (drops).
    };

    struct display_snapshot_t {
      std::string hestia_session_id;
      std::string display_name;
      int width {0};
      int height {0};
      int fps {0};
      bool virtual_display {false};
      bool isolated {false};
    };

    /** @brief Snapshot of the termination/reconnection stats. */
    termination_stats_t termination_stats();

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    std::string uuid(const session_t& session);
    bool uuid_match(const session_t& session, const std::string_view& uuid);
    bool hestia_session_match(
      const session_t &session,
      const std::string_view &uuid,
      const std::string_view &session_id
    );
    display_snapshot_t display_snapshot(const session_t &session);
    bool update_device_info(session_t& session, const std::string& name, const crypto::PERM& newPerm);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session, termination_reason_e reason = termination_reason_e::UNKNOWN);
    void graceful_stop(session_t& session);
    void join(session_t &session);
    state_e state(session_t &session);
    inline bool send(session_t& session, const std::string_view &payload);
  }  // namespace session
}  // namespace stream
