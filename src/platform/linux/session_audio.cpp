/**
 * @file src/platform/linux/session_audio.cpp
 * @brief An isolated session's own PipeWire server.
 */
#ifndef BOOST_PROCESS_VERSION
  #define BOOST_PROCESS_VERSION 1
#endif

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

// platform includes
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// lib includes
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/env.hpp>
#include <boost/process/v1/environment.hpp>
#include <boost/process/v1/handles.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/search_path.hpp>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <pulse/pulseaudio.h>

// local includes
#include "session_audio.h"
#include "src/logging.h"

namespace bp = boost::process::v1;
namespace fs = std::filesystem;
using namespace std::literals;

namespace platf::session_audio {
  namespace {
    std::string positions(int channels) {
      switch (channels) {
        case 6:
          return "FL FR FC LFE RL RR";
        case 8:
          return "FL FR FC LFE RL RR SL SR";
        default:
          return "FL FR";
      }
    }

    // No ALSA, Bluetooth or camera monitors: the sink below and Hermes' mic
    // stream are the only devices. The pulse server takes no D-Bus name, which the
    // host's already owns.
    std::string pipewire_conf(const std::string &sink, int channels) {
      std::string conf = R"(context.properties = {
    core.daemon = true
    core.name = pipewire-0
    link.max-buffers = 16
    default.clock.rate = 48000
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    video.convert.* = videoconvert/libspa-videoconvert
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-rt args = { nice.level = -11 rt.prio = 88 rtportal.enabled = false } flags = [ ifexists nofail ] }
    { name = libpipewire-module-protocol-native args = { sockets = [ { name = pipewire-0 } { name = pipewire-0-manager } ] } }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-access }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-protocol-pulse }
)";
      conf += R"(]
pulse.properties = {
    server.address = [ "unix:native" ]
    server.dbus-name = ""
}
context.objects = [
    { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Dummy-Driver node.group = pipewire.dummy priority.driver = 20000 } }
    { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Freewheel-Driver node.group = pipewire.freewheel priority.driver = 19000 node.freewheel = true } }
    { factory = adapter args = {
        factory.name = support.null-audio-sink
        node.name = )" + sink + R"(
        node.description = "Hermes session"
        media.class = Audio/Sink
        audio.position = [ )" + positions(channels) + R"( ]
        monitor.channel-volumes = true
        object.linger = true
    } }
]
)";
      return conf;
    }

    // Linking and default nodes only; nothing is remembered, and nothing
    // talks to the host's session bus (MPRIS pausing would reach its players).
    constexpr auto wireplumber_conf = R"(override.context.modules = [
  { name = libpipewire-module-rt args = { nice.level = -11 rtportal.enabled = false } flags = [ ifexists nofail ] }
  { name = libpipewire-module-protocol-native }
  { name = libpipewire-module-metadata }
]
wireplumber.profiles = {
  hermes-session = {
    inherits = [ policy, mixin.systemwide-session, mixin.stateless ]
    hardware.audio = disabled
    hardware.bluetooth = disabled
    hardware.video-capture = disabled
    support.dbus = disabled
    api.notifications = disabled
    support.mpris = disabled
    hooks.linking.pause-playback = disabled
  }
}
)";

    bool write_file(const fs::path &path, const std::string &contents) {
      std::error_code ec;
      fs::create_directories(path.parent_path(), ec);
      std::ofstream file {path};
      file << contents;
      return file.good();
    }

    void stop(bp::child &child) {
      if (!child.valid()) {
        return;
      }
      if (child.running()) {
        ::kill(child.id(), SIGTERM);
      }
      for (int i = 0; i < 100 && child.running(); ++i) {
        std::this_thread::sleep_for(20ms);
      }
      if (child.running()) {
        child.terminate();
      }
      child.wait();
    }

    // The client's microphone between Hermes and the session: a jitter buffer
    // just deep enough for Wi-Fi. (PipeWire's pipe-tunnel holds 8192 frames,
    // 171 ms, to match clocks; dropping a few samples does that here.)
    class mic_buffer_t {
    public:
      void write(const std::int16_t *samples, int count) {
        std::lock_guard lock {mutex};
        for (int i = 0; i < count; ++i) {
          // Full: the oldest go, so a fast client clock or a burst after a
          // stall never turns into delay.
          if (fill == ring.size()) {
            head = (head + 1) % ring.size();
            --fill;
          }
          ring[(head + fill++) % ring.size()] = samples[i];
        }
      }

      // Real-time side: never waits for the writer.
      void read(std::int16_t *out, std::size_t count) {
        std::size_t n = 0;
        if (std::unique_lock lock {mutex, std::try_to_lock}; lock.owns_lock()) {
          playing = playing || fill >= start_fill;
          if (playing) {
            n = std::min(count, fill);
            for (std::size_t i = 0; i < n; ++i) {
              out[i] = ring[(head + i) % ring.size()];
            }
            head = (head + n) % ring.size();
            fill -= n;
            // Underrun: silence until start_fill is buffered again.
            playing = n == count;
          }
        }
        std::fill(out + n, out + count, 0);
      }

    private:
      static constexpr std::size_t start_fill = 48 * 30;
      std::array<std::int16_t, 48 * 80> ring {};
      std::size_t head {};
      std::size_t fill {};
      bool playing {};
      std::mutex mutex;
    };

    std::mutex mics_mutex;
    std::map<std::string, std::weak_ptr<mic_buffer_t>> mics;

    int connect_socket(const std::string &path) {
      sockaddr_un address {.sun_family = AF_UNIX};
      if (path.size() >= sizeof address.sun_path) {
        return -1;
      }
      path.copy(address.sun_path, path.size());
      const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd >= 0 && ::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof address) < 0) {
        ::close(fd);
        return -1;
      }
      return fd;
    }

    // The session's mic: an Audio/Source stream on its server, fed from buffer.
    class mic_stream_t {
    public:
      mic_stream_t(const std::string &remote, const std::string &name, std::shared_ptr<mic_buffer_t> buffer):
          buffer {std::move(buffer)} {
        static std::once_flag initialized;
        std::call_once(initialized, [] {
          pw_init(nullptr, nullptr);
        });

        loop = pw_thread_loop_new("hermes-mic", nullptr);
        context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
        // PIPEWIRE_REMOTE overrides a remote name, so connect the socket ourselves.
        const int fd = connect_socket(remote);
        core = fd < 0 ? nullptr : pw_context_connect_fd(context, fd, nullptr, 0);
        if (!core) {
          return;
        }

        stream = pw_stream_new(core, "Client microphone", pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CLASS, "Audio/Source", PW_KEY_NODE_NAME, name.c_str(), PW_KEY_NODE_DESCRIPTION, "Client microphone", PW_KEY_NODE_LATENCY, "480/48000", nullptr));
        static const pw_stream_events events {.version = PW_VERSION_STREAM_EVENTS, .process = process};
        pw_stream_add_listener(stream, &listener, &events, this);

        spa_audio_info_raw info {};
        info.format = SPA_AUDIO_FORMAT_S16;
        info.rate = 48000;
        info.channels = 1;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        std::uint8_t pod[1024];
        auto builder = SPA_POD_BUILDER_INIT(pod, sizeof pod);
        const spa_pod *params[] {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
        if (pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1) == 0 &&
            pw_thread_loop_start(loop) == 0) {
          connected = true;
        }
      }

      ~mic_stream_t() {
        pw_thread_loop_stop(loop);
        if (stream) {
          pw_stream_destroy(stream);
        }
        if (core) {
          pw_core_disconnect(core);
        }
        pw_context_destroy(context);
        pw_thread_loop_destroy(loop);
      }

      bool connected {};

    private:
      static void process(void *data) {
        auto *self = static_cast<mic_stream_t *>(data);
        pw_buffer *buffer = pw_stream_dequeue_buffer(self->stream);
        if (!buffer) {
          return;
        }
        spa_data &out = buffer->buffer->datas[0];
        if (out.data) {
          auto frames = out.maxsize / sizeof(std::int16_t);
          if (buffer->requested) {
            frames = std::min<std::size_t>(frames, buffer->requested);
          }
          self->buffer->read(static_cast<std::int16_t *>(out.data), frames);
          out.chunk->offset = 0;
          out.chunk->stride = sizeof(std::int16_t);
          out.chunk->size = frames * sizeof(std::int16_t);
        }
        pw_stream_queue_buffer(self->stream, buffer);
      }

      std::shared_ptr<mic_buffer_t> buffer;
      pw_thread_loop *loop {};
      pw_context *context {};
      pw_core *core {};
      pw_stream *stream {};
      spa_hook listener {};
    };

    class pipewire_t: public server_t {
    public:
      pipewire_t(std::string runtime_dir, std::string mic):
          runtime_dir {std::move(runtime_dir)},
          mic {std::move(mic)} {
      }

      ~pipewire_t() override {
        if (!mic.empty()) {
          std::lock_guard lock {mics_mutex};
          mics.erase(mic);
        }
        mic_stream.reset();
        stop(wireplumber);
        stop(pipewire);
      }

      std::string pulse_server() const override {
        return "unix:" + runtime_dir + "/pulse/native";
      }

      std::string pipewire_remote() const override {
        return runtime_dir + "/pipewire-0";
      }

      std::string runtime_dir;
      std::string mic;
      std::unique_ptr<mic_stream_t> mic_stream;
      bp::child pipewire;
      bp::child wireplumber;
    };

    // A socket on disk is not a ready audio graph. In particular WirePlumber
    // can exit after exec, or the session's default node may not exist yet.
    // Ask the explicitly named private server; never autospawn or probe the
    // host's default server while bringing an isolated session up.
    bool wait_for_audio_graph(pipewire_t &server, const std::string &sink, const std::string &mic) {
      util::safe_ptr<pa_mainloop, pa_mainloop_free> loop {pa_mainloop_new()};
      if (!loop) {
        return false;
      }
      util::safe_ptr<pa_context, pa_context_unref> context {
        pa_context_new(pa_mainloop_get_api(loop.get()), "Hermes session readiness")
      };
      if (!context || pa_context_connect(context.get(), server.pulse_server().c_str(), PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
        return false;
      }
      auto disconnect = util::fail_guard([&]() {
        pa_context_disconnect(context.get());
      });
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (std::chrono::steady_clock::now() < deadline) {
        if (!server.pipewire.running() || !server.wireplumber.running() || !PA_CONTEXT_IS_GOOD(pa_context_get_state(context.get()))) {
          return false;
        }
        if (pa_mainloop_iterate(loop.get(), 0, nullptr) < 0) {
          return false;
        }
        if (pa_context_get_state(context.get()) != PA_CONTEXT_READY) {
          std::this_thread::sleep_for(10ms);
          continue;
        }

        struct graph_t {
          const std::string &sink;
          const std::string &mic;
          bool output = false;
          bool input = false;
          bool defaults = false;
        } graph {sink, mic, false, mic.empty(), false};

        util::safe_ptr<pa_operation, pa_operation_unref> output {
          pa_context_get_sink_info_by_name(context.get(), sink.c_str(), [](pa_context *, const pa_sink_info *info, int, void *data) {
            auto &graph = *static_cast<graph_t *>(data);
            if (info && info->name) {
              graph.output = graph.sink == info->name;
            }
          },
                                           &graph)
        };
        util::safe_ptr<pa_operation, pa_operation_unref> input;
        if (!mic.empty()) {
          input.reset(pa_context_get_source_info_by_name(context.get(), mic.c_str(), [](pa_context *, const pa_source_info *info, int, void *data) {
            auto &graph = *static_cast<graph_t *>(data);
            if (info && info->name) {
              graph.input = graph.mic == info->name;
            }
          },
                                                         &graph));
        }
        util::safe_ptr<pa_operation, pa_operation_unref> defaults {
          pa_context_get_server_info(context.get(), [](pa_context *, const pa_server_info *info, void *data) {
            auto &graph = *static_cast<graph_t *>(data);
            graph.defaults = info && info->default_sink_name && graph.sink == info->default_sink_name &&
                             (graph.mic.empty() || (info->default_source_name && graph.mic == info->default_source_name));
          },
                                     &graph)
        };
        auto cancel_operations = util::fail_guard([&]() {
          for (auto *operation : {output.get(), input.get(), defaults.get()}) {
            if (operation) {
              pa_operation_cancel(operation);
            }
          }
        });
        if (!output || !defaults || (!mic.empty() && !input)) {
          return false;
        }
        const auto pending = [&]() {
          return pa_operation_get_state(output.get()) == PA_OPERATION_RUNNING ||
                 pa_operation_get_state(defaults.get()) == PA_OPERATION_RUNNING ||
                 (input && pa_operation_get_state(input.get()) == PA_OPERATION_RUNNING);
        };
        while (pending() && std::chrono::steady_clock::now() < deadline) {
          if (!server.pipewire.running() || !server.wireplumber.running() || !PA_CONTEXT_IS_GOOD(pa_context_get_state(context.get())) || pa_mainloop_iterate(loop.get(), 0, nullptr) < 0) {
            return false;
          }
          std::this_thread::sleep_for(10ms);
        }
        if (pending()) {
          return false;
        }
        if (graph.output && graph.input && graph.defaults) {
          return true;
        }
        std::this_thread::sleep_for(20ms);
      }
      return false;
    }
  }  // namespace

  std::unique_ptr<server_t> start(const std::string &runtime_dir, const std::string &sink, int channels, const std::string &mic) {
    const auto pipewire = bp::search_path("pipewire");
    const auto wireplumber = bp::search_path("wireplumber");
    if (pipewire.empty() || wireplumber.empty()) {
      BOOST_LOG(warning) << "PipeWire or WirePlumber is not installed; no private audio server"sv;
      return nullptr;
    }

    const fs::path dir = fs::path {runtime_dir} / "audio";
    // Not named pipewire.conf, so no system or user fragments apply.
    const auto conf = dir / "hermes-session.conf";
    if (!write_file(conf, pipewire_conf(sink, channels)) ||
        !write_file(dir / "config/wireplumber/wireplumber.conf.d/hermes-session.conf", wireplumber_conf)) {
      BOOST_LOG(warning) << "Couldn't write the private audio server's config to "sv << dir.string();
      return nullptr;
    }
    std::error_code ec;

    // The user's own PipeWire config and WirePlumber state stay out of it.
    auto env = bp::environment {boost::this_process::environment()};
    for (const auto *name : {"PIPEWIRE_REMOTE", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_CONFIG_DIR", "PIPEWIRE_CONFIG_NAME", "PIPEWIRE_CORE", "PIPEWIRE_NODE", "PIPEWIRE_PROPS", "PULSE_SERVER", "PULSE_RUNTIME_PATH", "PULSE_SINK", "PULSE_SOURCE", "WIREPLUMBER_CONFIG_DIR", "DBUS_SESSION_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY"}) {
      env.erase(name);
    }
    env["XDG_RUNTIME_DIR"] = runtime_dir;
    env["XDG_CONFIG_HOME"] = (dir / "config").string();
    env["XDG_STATE_HOME"] = (dir / "state").string();

    auto server = std::make_unique<pipewire_t>(runtime_dir, mic);
    server->pipewire = bp::child(pipewire, "-c", conf.string(), env, bp::std_in < bp::null, bp::limit_handles, ec);
    if (ec) {
      BOOST_LOG(warning) << "Couldn't start PipeWire: "sv << ec.message();
      return nullptr;
    }

    // Applications start once the pulse socket exists; it is created last.
    const auto socket = fs::path {runtime_dir} / "pulse" / "native";
    for (auto deadline = std::chrono::steady_clock::now() + 5s; !fs::exists(socket, ec);) {
      if (!server->pipewire.running() || std::chrono::steady_clock::now() > deadline) {
        BOOST_LOG(warning) << "The private PipeWire server didn't come up in "sv << runtime_dir;
        return nullptr;
      }
      std::this_thread::sleep_for(20ms);
    }

    server->wireplumber = bp::child(wireplumber, "-p", "hermes-session", env, bp::std_in < bp::null, bp::limit_handles, ec);
    if (ec) {
      BOOST_LOG(warning) << "Couldn't start WirePlumber: "sv << ec.message();
      return nullptr;
    }
    // The mic is Hermes' own stream, so the graph is only complete once it exists.
    if (!mic.empty()) {
      auto buffer = std::make_shared<mic_buffer_t>();
      server->mic_stream = std::make_unique<mic_stream_t>(server->pipewire_remote(), mic, buffer);
      if (!server->mic_stream->connected) {
        BOOST_LOG(warning) << "Couldn't create the session mic ["sv << mic << ']';
        return nullptr;
      }
      std::lock_guard lock {mics_mutex};
      mics[mic] = buffer;
    }

    if (!wait_for_audio_graph(*server, sink, mic)) {
      BOOST_LOG(warning) << "The private audio graph was not ready in "sv << runtime_dir
                         << "; expected sink [" << sink << "] and source [" << mic << ']';
      return nullptr;
    }

    BOOST_LOG(info) << "Started a private PipeWire server in "sv << runtime_dir << " with sink ["sv << sink << ']'
                    << (mic.empty() ? ""s : " and mic ["s + mic + ']');
    return server;
  }

  namespace {
    class buffer_mic_writer_t: public mic_writer_t {
    public:
      explicit buffer_mic_writer_t(std::shared_ptr<mic_buffer_t> buffer):
          buffer {std::move(buffer)} {
      }

      void write(const std::int16_t *samples, int count) override {
        buffer->write(samples, count);
      }

    private:
      std::shared_ptr<mic_buffer_t> buffer;
    };
  }  // namespace

  std::unique_ptr<mic_writer_t> open_mic(const std::string &mic) {
    std::lock_guard lock {mics_mutex};
    const auto it = mics.find(mic);
    auto buffer = it == mics.end() ? nullptr : it->second.lock();
    if (!buffer) {
      BOOST_LOG(warning) << "No session mic ["sv << mic << ']';
      return nullptr;
    }
    return std::make_unique<buffer_mic_writer_t>(std::move(buffer));
  }
}  // namespace platf::session_audio
