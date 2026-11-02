/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
// standard includes
#include <algorithm>
#include <array>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

// lib includes
#include <opus/opus_multistream.h>

// local includes
#include "audio.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#ifndef _WIN32
  #include "platform/linux/session_audio.h"
#endif
#include "thread_safe.h"
#include "utility.h"

namespace audio {
  using namespace std::literals;
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;

  static int start_audio_control(audio_ctx_t &ctx);
  static void stop_audio_control(audio_ctx_t &);
  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);

  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // corresponding bitrate adjustment logic in rtsp_stream::cmd_announce()
  opus_stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51,
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51,
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71,
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71,
      2048000,
    },
  };

  void encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    // Encoding takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    opus_t opus {opus_multistream_encoder_create(
      stream.sampleRate,
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      stream.mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr
    )};

    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));

    BOOST_LOG(info) << "Opus initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};

      int bytes = opus_multistream_encode_float(opus.get(), sample->data(), frame_size, std::begin(packet), packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));
    }
  }

  void capture(safe::mail_t mail, config_t config, void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    if (!config::audio.stream || config.input_only) {
      shutdown_event->view();
      return;
    }
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    std::function<std::unique_ptr<platf::mic_t>()> open_microphone;
    audio_ctx_ref_t ref;

    auto init_failure_fg = util::fail_guard([&shutdown_event]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;

      // Wait for shutdown to be signalled if we fail init.
      // This allows streaming to continue without audio.
      shutdown_event->view();
      return;
    });

#ifndef _WIN32
    // A session with an audio server of its own is recorded there, without the
    // host's audio control and the virtual sinks it adds to the host's server.
    if (!config.session_server.empty()) {
      open_microphone = [&]() {
        return platf::session_audio::microphone(config.session_server, config.session_sink, stream.mapping, stream.channelCount, stream.sampleRate, frame_size);
      };
    }
#endif

    if (!open_microphone) {
      ref = get_audio_ctx_ref();
      if (!ref || !ref->control) {
        return;
      }
      auto &control = ref->control;

      // A session that owns a sink takes it and considers nothing else. Every
      // priority below is a sink the whole machine shares, which is the one thing
      // an isolated session must not record.
      std::string session_sink = config.session_sink;

      // Order of priority:
      // 1. Virtual sink
      // 2. Audio sink
      // 3. Host
      std::string *sink = &ref->sink.host;
      if (!session_sink.empty()) {
        sink = &session_sink;
      } else if (!config::audio.sink.empty()) {
        sink = &config::audio.sink;
      }

      // Prefer the virtual sink if host playback is disabled or there's no other sink
      if (session_sink.empty() && ref->sink.null &&
          (!config.flags[config_t::HOST_AUDIO] || sink->empty())) {
        auto &null = *ref->sink.null;
        switch (stream.channelCount) {
          case 2:
            sink = &null.stereo;
            break;
          case 6:
            sink = &null.surround51;
            break;
          case 8:
            sink = &null.surround71;
            break;
        }
      }

      BOOST_LOG(info) << "Selected audio sink: "sv << *sink;

      // Only the first to start a session may change the default sink, and an
      // isolated session is never that one. Moving the host's default is how the
      // host's own audio is made to reach a stream; doing it for a session that
      // is somebody else's would put the host - and every other session - into
      // that client's ears, and take the audio away from whoever was streaming
      // before.
      if (session_sink.empty() && !ref->sink_flag->exchange(true, std::memory_order_acquire)) {
        // If the selected sink is different than the current one, change sinks.
        ref->restore_sink = ref->sink.host != *sink;
        if (ref->restore_sink) {
          if (control->set_sink(*sink)) {
            return;
          }
        }
      }

      open_microphone = [&]() {
        return config.session_sink.empty() ?
                 ref->control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size) :
                 ref->control->microphone_from(config.session_sink, stream.mapping, stream.channelCount, stream.sampleRate, frame_size);
      };
    }

    auto mic = open_microphone();
    if (!mic) {
      return;
    }

    // Audio is initialized, so we don't want to print the failure message
    init_failure_fg.disable();

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    auto samples = std::make_shared<sample_queue_t::element_type>(30);
    std::thread thread {encodeThread, samples, config, channel_data};

    auto fg = util::fail_guard([&]() {
      samples->stop();
      thread.join();

      shutdown_event->view();
    });

    int samples_per_frame = frame_size * stream.channelCount;

    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);
      switch (status) {
        case platf::capture_e::ok:
          break;
        case platf::capture_e::timeout:
          continue;
        case platf::capture_e::reinit:
          if (config::audio.auto_capture) {
            BOOST_LOG(info) << "Reinitializing audio capture"sv;
            mic.reset();
            do {
              mic = open_microphone();
              if (!mic) {
                BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
              }
            } while (!mic && !shutdown_event->view(5s));
          }

          continue;
        default:
          return;
      }

      samples->raise(std::move(sample_buffer));
    }
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    static auto control_shared {safe::make_shared<audio_ctx_t>(start_audio_control, stop_audio_control)};
    return control_shared.ref();
  }

  bool host_sink_restore_pending(const audio_ctx_t &ctx) {
    if (ctx.sink.host.empty() && config::audio.sink.empty()) {
      return false;
    }

    return !is_audio_ctx_sink_available(ctx);
  }

  namespace {
    /**
     * How long release_host_sink() waits for the recorded sink to come back.
     *
     * The wait is not free: until it ends the host's default is still the
     * virtual sink the stream was recorded from, so the host plays into
     * silence. That is what bounds both numbers. A monitor coming out of an
     * exclusive session re-registers its audio device in a second or two, so
     * ten seconds is a generous ceiling - and a device that is not coming back
     * at all, a headset unplugged mid-session, costs ten seconds of quiet
     * rather than the half-minute a display-sized timeout would.
     *
     * Polled often rather than slowly for the same reason: the common case
     * ends when the sink reappears, not when the timeout expires, and every
     * poll it is late by is silence.
     */
    constexpr auto HOST_SINK_WAIT_INTERVAL = 500ms;
    constexpr int HOST_SINK_WAIT_ATTEMPTS = 20;

    /**
     * @brief The single hold behind hold_host_sink()/release_host_sink().
     */
    struct host_sink_hold_t {
      std::mutex mutex;

      /// Woken when a hold is taken back, to cut a pending wait short.
      std::condition_variable cancelled;

      /// The held context. Its presence is what "a hold is taken" means.
      audio_ctx_ref_t ref;

      /// The thread waiting for `ref`'s sink to come back, if one is running.
      std::thread waiter;

      /// Whether that thread is still meant to drop the hold when it is done.
      bool releasing {false};

      ~host_sink_hold_t() {
        {
          std::lock_guard lock {mutex};
          releasing = false;
        }
        cancelled.notify_all();
        if (waiter.joinable()) {
          waiter.join();
        }
      }
    };

    /**
     * @note Deliberately not a namespace-scope object: it has to be constructed
     *       after the audio context's own static, so that it is destroyed
     *       before it and the waiter is joined while the context it holds is
     *       still alive. hold_host_sink() is what orders the two.
     */
    host_sink_hold_t &host_sink_hold() {
      static host_sink_hold_t hold;
      return hold;
    }

    /**
     * @brief Wait for the held context's sink, then drop the hold.
     */
    void wait_for_host_sink() {
      auto &hold = host_sink_hold();
      std::unique_lock lock {hold.mutex};

      bool waited_out = false;
      std::string waited_for;
      for (int attempt = 0; attempt < HOST_SINK_WAIT_ATTEMPTS && hold.releasing; ++attempt) {
        const auto *ctx = hold.ref ? hold.ref.get() : nullptr;
        if (!ctx || !host_sink_restore_pending(*ctx)) {
          break;
        }
        waited_out = attempt + 1 == HOST_SINK_WAIT_ATTEMPTS;
        waited_for = ctx->sink.host.empty() ? config::audio.sink : ctx->sink.host;
        hold.cancelled.wait_for(lock, HOST_SINK_WAIT_INTERVAL, [&hold]() {
          return !hold.releasing;
        });
      }

      if (waited_out && hold.releasing) {
        // Unplugged for good, most likely, rather than slow to come back. The
        // restore below will fail on the name, and dropping the context takes
        // the virtual sinks with it - at which point the sound server picks a
        // default of its own, which is the right answer once the device the
        // user chose is gone.
        BOOST_LOG(warning) << "Gave up waiting for audio sink ["sv << waited_for
                           << "] to come back; leaving the default where the sound server puts it"sv;
      }

      if (!hold.releasing) {
        // A new session took the hold back while we were waiting.
        return;
      }

      hold.releasing = false;
      auto ref = std::move(hold.ref);
      hold.ref = {};
      lock.unlock();

      // Dropped outside the lock: the last reference is the one that talks to
      // the sound server to put the default sink back.
      ref = {};
    }
  }  // namespace

  void hold_host_sink() {
    // Take the context first. That constructs its static before the hold's, so
    // the hold is destroyed first at exit and joins its waiter while the
    // context is still there to be dropped.
    auto ref = get_audio_ctx_ref();
    auto &hold = host_sink_hold();

    std::unique_lock lock {hold.mutex};
    if (hold.releasing) {
      // The previous session's release is still waiting for its sink. That
      // context holds the default recorded before any display went dark, which
      // is the one this session wants as well - keep it rather than let it go
      // and read a default that is about to be moved again.
      hold.releasing = false;
      lock.unlock();
      hold.cancelled.notify_all();
      return;
    }

    if (!hold.ref) {
      hold.ref = std::move(ref);
      lock.unlock();
      BOOST_LOG(debug) << "Holding the host audio sink across the display change"sv;
    }
  }

  void release_host_sink() {
    auto &hold = host_sink_hold();

    std::thread finished;
    {
      std::lock_guard lock {hold.mutex};
      if (!hold.ref || hold.releasing) {
        return;
      }

      hold.releasing = true;
      // Either never started, or already past its own `releasing = false`, so
      // joining it below costs nothing.
      finished = std::move(hold.waiter);
      hold.waiter = std::thread {wait_for_host_sink};
    }

    if (finished.joinable()) {
      finished.join();
    }
  }

  bool create_session_sink(const std::string &name, int channels) {
    auto ref = get_audio_ctx_ref();
    if (!ref || !ref->control) {
      return false;
    }

    // The layout comes from the same table capture uses, so the sink is made
    // with the channel map that will later be recorded from it. Quality picks
    // the bitrate and nothing about the layout, so either row will do.
    const auto &stream = stream_configs[map_stream(channels, false)];
    return ref->control->create_session_sink(name, stream.mapping, stream.channelCount);
  }

  void remove_session_sink(const std::string &name) {
    auto ref = get_audio_ctx_ref();
    if (!ref || !ref->control) {
      return;
    }
    ref->control->remove_session_sink(name);
  }

  client_mic_t::client_mic_t(std::unique_ptr<platf::mic_writer_t> writer):
      writer {std::move(writer)} {
    int status = OPUS_OK;
    decoder.reset(opus_decoder_create(SAMPLE_RATE, 1, &status));
    if (status != OPUS_OK) {
      decoder.reset();
      BOOST_LOG(error) << "Couldn't create the client microphone decoder: "sv << opus_strerror(status);
    }
  }

  void client_mic_t::write(std::uint16_t sequence, const std::uint8_t *data, std::size_t size) {
    const int frame = opus_packet_get_nb_samples(data, size, SAMPLE_RATE);
    if (!decoder || frame <= 0) {
      return;
    }

    int lost = 0;
    if (last_sequence) {
      const auto gap = static_cast<std::int16_t>(sequence - *last_sequence);
      if (gap <= 0) {
        return;
      }
      lost = std::min(gap - 1, 5);
    }
    last_sequence = sequence;

    // 120 ms, Opus' longest frame.
    std::array<opus_int16, 5760> pcm;
    // Conceal all but the last lost frame, which this packet's FEC carries.
    for (; lost > 0; --lost) {
      const bool fec = lost == 1;
      const int samples = opus_decode(decoder.get(), fec ? data : nullptr, fec ? size : 0, pcm.data(), frame, fec);
      if (samples > 0) {
        writer->write(pcm.data(), samples);
      }
    }

    const int samples = opus_decode(decoder.get(), data, size, pcm.data(), pcm.size(), 0);
    if (samples > 0) {
      writer->write(pcm.data(), samples);
    }
  }

  std::unique_ptr<client_mic_t> open_client_mic(const std::string &source) {
#ifndef _WIN32
    if (auto writer = platf::session_audio::open_mic(source)) {
      return std::make_unique<client_mic_t>(std::move(writer));
    }
#endif
    return nullptr;
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  int map_stream(int channels, bool quality) {
    int shift = quality ? 1 : 0;
    switch (channels) {
      case 2:
        return STEREO + shift;
      case 6:
        return SURROUND51 + shift;
      case 8:
        return SURROUND71 + shift;
    }
    return STEREO;
  }

  int start_audio_control(audio_ctx_t &ctx) {
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);

    // The default sink has not been replaced yet.
    ctx.restore_sink = false;

    if (!(ctx.control = platf::audio_control())) {
      return 0;
    }

    auto sink = ctx.control->sink_info();
    if (!sink) {
      // Let the calling code know it failed
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);

    fg.disable();
    return 0;
  }

  void stop_audio_control(audio_ctx_t &ctx) {
    // restore audio-sink if applicable
    if (!ctx.restore_sink) {
      return;
    }

    // Change back to the host sink, unless there was none
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (!sink.empty()) {
      // Best effort, it's allowed to fail
      ctx.control->set_sink(sink);
    }
  }

  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    stream.channelCount = params.channelCount;
    stream.streams = params.streams;
    stream.coupledStreams = params.coupledStreams;
    stream.mapping = params.mapping;
  }
}  // namespace audio
