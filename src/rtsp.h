/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <string>

// local includes
#include "crypto.h"
#include "thread_safe.h"
#include "uuid.h"

#ifdef _WIN32
  #include <windows.h>
#endif

// Resolve circular dependencies
namespace stream {
  struct session_t;
}

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;

  struct launch_session_t {
    uint32_t id;
    // Serialize the final RTSP handoff against an HTTPS cancellation. A
    // socket may retain this object after the pending event is removed.
    std::mutex lifecycle_mutex;
    std::atomic_bool cancelled {false};

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;

    std::string device_name;
    std::string unique_id;
    crypto::PERM perm;

    bool input_only;
    bool host_audio;
    int width;
    int height;
    int fps;
    int gcmap;
    int surround_info;
    std::string surround_params;
    bool enable_hdr;
    bool enable_sops;
    bool virtual_display;
    // Experimental Linux Hermes-KMS multi-output state. The display is owned
    // by the streaming session rather than by the process/app lifetime.
    bool session_scoped_virtual_display = false;
    // True only while the launch/RTSP handshake still owns cleanup. This is
    // cleared when ownership moves to stream::session_t.
    bool session_virtual_display_cleanup_pending = false;
    std::string display_name;
    // Experimental Linux session isolation. One Hermes process may own many
    // independent compositor/app runtimes, one for each Moonlight session.
    bool isolated_session = false;
    uint32_t isolated_runtime_owner_id = 0;
    std::string isolated_session_profile;
    std::string isolated_runtime_id;
    std::string isolated_seat_id;
    std::string drm_device_path;
    std::string wayland_display;
    uint32_t scale_factor;
    std::string launch_mode;

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;

    std::list<crypto::command_entry_t> client_do_cmds;
    std::list<crypto::command_entry_t> client_undo_cmds;

#ifdef _WIN32
    GUID display_guid{};
#else
    uuid_util::uuid_t display_guid{};
#endif
  };

  /**
   * Publish a launch for the RTSP handshake.
   * @return false when another handshake is already pending.
   */
  bool launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * Cancel a pending RTSP launch for one paired client without touching any
   * other client's handshake.
   */
  bool cancel_pending_launch(const std::string_view &uuid);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  /**
   * @brief Get a short identifier for why the most recent session ended.
   * @return e.g. "client_quit", "client_lost", "server_stopped", "unknown".
   */
  std::string_view last_termination_reason();

  std::shared_ptr<stream::session_t>
  find_session(const std::string_view& uuid);

  std::list<std::string>
  get_all_session_uuids();

  /** Terminates only the session associated with a paired client. */
  bool terminate_session(const std::string_view &uuid);

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
