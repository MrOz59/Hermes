/** @file src/platform/linux/pipewire_session.cpp
 *  @brief Bounded mapped-buffer capture from a private KWin stream.
 */
#include "pipewire_session.h"
#include "session_frame.h"
#include "session_socket.h"

#include <cerrno>
#include <mutex>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

namespace platf::kwin {
  struct video_receiver_t::impl_t {
    pw_thread_loop *loop {};
    pw_context *context {};
    pw_core *core {};
    pw_stream *stream {};
    spa_hook stream_listener {};
    spa_hook core_listener {};
    spa_video_info_raw format {};
    std::uint32_t width {};
    std::uint32_t height {};
    bool started {};
    bool stopping {};
    bool format_ready {};
    bool pending {};
    bool streaming {};
    std::string failure;
    frame_t latest;

    ~impl_t() {
      if (started) pw_thread_loop_stop(loop);
      stopping = true;
      if (stream) {
        spa_hook_remove(&stream_listener);
        pw_stream_destroy(stream);
      }
      if (core) {
        spa_hook_remove(&core_listener);
        pw_core_disconnect(core);
      }
      if (context) pw_context_destroy(context);
      if (loop) pw_thread_loop_destroy(loop);
    }

    void fail(std::string message) {
      if (failure.empty()) failure = std::move(message);
      pw_thread_loop_signal(loop, false);
    }

    static void state_changed(void *data, pw_stream_state, pw_stream_state state, const char *error) {
      auto &self = *static_cast<impl_t *>(data);
      if (self.stopping) return;
      if (state == PW_STREAM_STATE_ERROR) self.fail(error ? error : "Private PipeWire stream failed");
      if (state == PW_STREAM_STATE_STREAMING) self.streaming = true;
      if (state == PW_STREAM_STATE_UNCONNECTED && self.streaming) self.fail("Private PipeWire stream disconnected");
      pw_thread_loop_signal(self.loop, false);
    }

    static void param_changed(void *data, std::uint32_t id, const spa_pod *param) {
      auto &self = *static_cast<impl_t *>(data);
      if (id != SPA_PARAM_Format) return;
      if (!param) {
        if (self.format_ready) self.fail("Private capture format was removed");
        return;
      }
      spa_video_info_raw format {};
      if (spa_format_video_raw_parse(param, &format) < 0 || format.format != SPA_VIDEO_FORMAT_BGRx ||
          format.size.width != self.width || format.size.height != self.height) {
        self.fail("Private capture negotiated an unexpected pixel format or size");
        return;
      }
      self.format = format;
      self.format_ready = true;
      std::uint8_t buffer[512];
      spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
      const spa_pod *params[] = {
        static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
          SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)))),
      };
      if (pw_stream_update_params(self.stream, params, 1) < 0) self.fail("Could not negotiate mapped private capture buffers");
    }

    static void process(void *data) {
      auto &self = *static_cast<impl_t *>(data);
      auto buffer = pw_stream_dequeue_buffer(self.stream);
      if (!buffer) return;
      bool valid = false;
      auto *spa = buffer->buffer;
      if (self.format_ready && spa && spa->n_datas == 1) {
        const auto &plane = spa->datas[0];
        if ((plane.type == SPA_DATA_MemFd || plane.type == SPA_DATA_MemPtr) && plane.chunk &&
            !(plane.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
          try {
            valid = copy_bgrx(self.latest.bgrx, plane.data, plane.maxsize, plane.chunk->offset,
                              plane.chunk->size, plane.chunk->stride, self.width, self.height);
          } catch (...) {
            // No C++ exception may unwind through a PipeWire callback.
            pw_stream_queue_buffer(self.stream, buffer);
            self.fail("Could not allocate private capture frame");
            return;
          }
        }
      }
      pw_stream_queue_buffer(self.stream, buffer);
      if (!valid) {
        self.fail("Private capture supplied an invalid mapped frame");
        return;
      }
      self.latest.width = self.width;
      self.latest.height = self.height;
      self.latest.timestamp = std::chrono::steady_clock::now();
      self.pending = true;
      pw_thread_loop_signal(self.loop, false);
    }
  };

  video_receiver_t::video_receiver_t(std::unique_ptr<impl_t> state): impl(std::move(state)) {}
  video_receiver_t::~video_receiver_t() = default;

  std::unique_ptr<video_receiver_t> video_receiver_t::open(const std::string &socket, std::uint64_t serial,
                                                         std::uint32_t width, std::uint32_t height,
                                                         std::uint32_t framerate, std::string &error) {
    error.clear();
    if (socket.empty() || socket.front() != '/' || socket.find('\0') != std::string::npos || serial == 0 ||
        !width || !height || width > 16384 || height > 16384 || !framerate || framerate > 1000) {
      error = "Invalid private PipeWire endpoint, node, size, or frame rate";
      return nullptr;
    }
    static std::once_flag initialized;
    std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
    auto state = std::make_unique<impl_t>();
    state->width = width;
    state->height = height;
    state->loop = pw_thread_loop_new("hermes-private-capture", nullptr);
    if (!state->loop) { error = "Could not create private PipeWire loop"; return nullptr; }
    state->context = pw_context_new(pw_thread_loop_get_loop(state->loop), nullptr, 0);
    if (!state->context) { error = "Could not create private PipeWire context"; return nullptr; }
    // PIPEWIRE_REMOTE overrides even an absolute remote.name property. Supply
    // an already connected fd so environment settings cannot select the host.
    const int fd = connect_session_socket(socket, error);
    if (fd < 0) return nullptr;
    // PipeWire takes ownership of fd, including on error.
    state->core = pw_context_connect_fd(state->context, fd, nullptr, 0);
    if (!state->core) {
      error = "Could not initialize the specified PipeWire connection";
      return nullptr;
    }
    static const auto core_events = [] {
      pw_core_events events {};
      events.version = PW_VERSION_CORE_EVENTS;
      events.error = [](void *data, std::uint32_t, int, int, const char *message) {
        static_cast<impl_t *>(data)->fail(message ? message : "Private PipeWire core error");
      };
      return events;
    }();
    pw_core_add_listener(state->core, &state->core_listener, &core_events, state.get());
    const auto target = std::to_string(serial);
    state->stream = pw_stream_new(state->core, "Hermes detached desktop",
      pw_properties_new(PW_KEY_TARGET_OBJECT, target.c_str(), PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    if (!state->stream) { error = "Could not create private PipeWire stream"; return nullptr; }
    static const auto stream_events = [] {
      pw_stream_events events {};
      events.version = PW_VERSION_STREAM_EVENTS;
      events.state_changed = impl_t::state_changed;
      events.param_changed = impl_t::param_changed;
      events.process = impl_t::process;
      return events;
    }();
    pw_stream_add_listener(state->stream, &state->stream_listener, &stream_events, state.get());
    std::uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_video_info_raw format {};
    format.format = SPA_VIDEO_FORMAT_BGRx;
    format.size = {width, height};
    // KWin uses variable-rate capture; negotiate the rate ceiling separately.
    format.framerate = {0, 1};
    format.max_framerate = {framerate, 1};
    const spa_pod *params[] {spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT | PW_STREAM_FLAG_INACTIVE);
    if (pw_stream_connect(state->stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0) {
      error = "Could not connect private capture node";
      return nullptr;
    }
    // PipeWire applies stream.rules and PIPEWIRE_NODE/PIPEWIRE_PROPS during
    // connect. Validate their result before activating or running the loop.
    const auto *properties = pw_stream_get_properties(state->stream);
    const char *actual_target = pw_properties_get(properties, PW_KEY_TARGET_OBJECT);
    const char *reconnect = pw_properties_get(properties, PW_KEY_NODE_DONT_RECONNECT);
    if (!actual_target || target != actual_target || !reconnect || std::strcmp(reconnect, "true") != 0) {
      error = "PipeWire configuration attempted to override the private capture target or reconnect policy";
      return nullptr;
    }
    if (pw_stream_set_active(state->stream, true) < 0) {
      error = "Could not activate private capture stream";
      return nullptr;
    }
    if (pw_thread_loop_start(state->loop) < 0) { error = "Could not start private PipeWire loop"; return nullptr; }
    state->started = true;
    return std::unique_ptr<video_receiver_t>(new video_receiver_t(std::move(state)));
  }

  frame_result_t video_receiver_t::next(frame_t &frame, std::chrono::milliseconds timeout, std::string &error) {
    pw_thread_loop_lock(impl->loop);
    timespec deadline {};
    pw_thread_loop_get_time(impl->loop, &deadline, std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count());
    while (!impl->pending && impl->failure.empty()) {
      int result = pw_thread_loop_timed_wait_full(impl->loop, &deadline);
      if (result == -ETIMEDOUT) break;
      if (result < 0) { impl->fail("Private PipeWire wait failed"); break; }
    }
    auto result = frame_result_t::timeout;
    if (!impl->failure.empty()) {
      error = impl->failure;
      result = frame_result_t::failed;
    } else if (impl->pending) {
      // Move ownership out while holding the loop lock. PipeWire's mapped buffer
      // was already returned; the encoder never holds a server-owned mapping.
      frame = std::move(impl->latest);
      impl->pending = false;
      result = frame_result_t::ready;
    }
    pw_thread_loop_unlock(impl->loop);
    return result;
  }
}  // namespace platf::kwin
