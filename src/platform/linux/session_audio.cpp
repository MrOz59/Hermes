/**
 * @file src/platform/linux/session_audio.cpp
 * @brief An isolated session's own PipeWire server.
 */
#ifndef BOOST_PROCESS_VERSION
  #define BOOST_PROCESS_VERSION 1
#endif

// standard includes
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

// platform includes
#include <fcntl.h>
#include <unistd.h>

// lib includes
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/env.hpp>
#include <boost/process/v1/environment.hpp>
#include <boost/process/v1/handles.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/search_path.hpp>
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

    // No ALSA, Bluetooth or camera monitors: the sink and the mic below are
    // the only devices. The pulse server takes no D-Bus name, which the
    // host's already owns.
    std::string pipewire_conf(const std::string &sink, int channels, const std::string &mic) {
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
      if (!mic.empty()) {
        conf += R"(    { name = libpipewire-module-pipe-tunnel args = {
        tunnel.mode = source
        pipe.filename = ")" + session_mic_fifo(mic) + R"("
        audio.format = S16LE
        audio.rate = 48000
        audio.channels = 1
        audio.position = [ MONO ]
        stream.props = { node.name = )" + mic + R"( node.description = "Client microphone" }
    } }
)";
      }
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

    class pipewire_t: public server_t {
    public:
      pipewire_t(std::string runtime_dir, std::string mic):
          runtime_dir {std::move(runtime_dir)},
          mic {std::move(mic)} {
      }

      ~pipewire_t() override {
        stop(wireplumber);
        stop(pipewire);
        if (!mic.empty()) {
          std::error_code ec;
          fs::remove(session_mic_fifo(mic), ec);
        }
      }

      std::string pulse_server() const override {
        return "unix:" + runtime_dir + "/pulse/native";
      }

      std::string pipewire_remote() const override {
        return runtime_dir + "/pipewire-0";
      }

      std::string runtime_dir;
      std::string mic;
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

  std::string session_mic_fifo(const std::string &mic) {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    return (fs::path {runtime && *runtime ? runtime : "/tmp"} / "hermes" / (mic + ".fifo")).string();
  }

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
    if (!write_file(conf, pipewire_conf(sink, channels, mic)) ||
        !write_file(dir / "config/wireplumber/wireplumber.conf.d/hermes-session.conf", wireplumber_conf)) {
      BOOST_LOG(warning) << "Couldn't write the private audio server's config to "sv << dir.string();
      return nullptr;
    }
    std::error_code ec;
    if (!mic.empty()) {
      fs::create_directories(fs::path {session_mic_fifo(mic)}.parent_path(), ec);
      fs::remove(session_mic_fifo(mic), ec);
    }

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
    class fifo_mic_writer_t: public mic_writer_t {
    public:
      fifo_mic_writer_t(int fd, std::string mic):
          fd {fd},
          mic {std::move(mic)} {
      }

      ~fifo_mic_writer_t() override {
        close(fd);
      }

      // The caller blocks SIGPIPE: the FIFO loses its reader with its server.
      // While nobody records, the full FIFO drops writes.
      void write(const std::int16_t *samples, int count) override {
        // Writes up to PIPE_BUF are all or nothing, so a full FIFO never
        // splits a sample.
        constexpr int chunk = PIPE_BUF / sizeof(std::int16_t);
        for (int offset = 0; offset < count; offset += chunk) {
          const auto bytes = std::min(chunk, count - offset) * sizeof(std::int16_t);
          if (::write(fd, samples + offset, bytes) < 0 && errno == EPIPE && !broken) {
            broken = true;
            BOOST_LOG(warning) << "Session mic ["sv << mic << "] is gone; dropping the client's microphone"sv;
          }
        }
      }

    private:
      int fd;
      std::string mic;
      bool broken {};
    };
  }  // namespace

  std::unique_ptr<mic_writer_t> open_mic(const std::string &mic) {
    const auto fifo = session_mic_fifo(mic);
    const int fd = open(fifo.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      BOOST_LOG(warning) << "Couldn't open session mic ["sv << fifo << "]: "sv << std::strerror(errno);
      return nullptr;
    }
    return std::make_unique<fifo_mic_writer_t>(fd, mic);
  }
}  // namespace platf::session_audio
