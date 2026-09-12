/**
 * @file tests/unit/test_audio.cpp
 * @brief Test src/audio.*.
 */
#include "../tests_common.h"

#include <src/audio.h>
#include <src/config.h>

#include <chrono>
#include <set>
#include <string>

using namespace audio;

namespace {

  /**
   * @brief An audio_control_t whose sinks are whatever the test says they are.
   *
   * Only the presence question is answered for real; everything else exists so
   * the class can be instantiated. Presence is the whole point: a monitor's
   * sink disappears with the monitor and is rebuilt under the same name when it
   * comes back, and that gap is what the host-sink hold has to sit out.
   */
  struct fake_audio_control_t: platf::audio_control_t {
    std::set<std::string> present;
    std::string last_sink_set;

    int set_sink(const std::string &sink) override {
      last_sink_set = sink;
      return present.count(sink) ? 0 : -1;
    }

    std::unique_ptr<platf::mic_t> microphone(const std::uint8_t *, int, std::uint32_t, std::uint32_t) override {
      return nullptr;
    }

    bool is_sink_available(const std::string &sink) override {
      return present.count(sink) > 0;
    }

    std::optional<platf::sink_t> sink_info() override {
      return std::nullopt;
    }
  };

  /**
   * @brief An audio context holding @p host as the sink recorded for the host.
   */
  audio_ctx_t make_ctx(const std::string &host, std::set<std::string> present) {
    audio_ctx_t ctx {};
    auto control = std::make_unique<fake_audio_control_t>();
    control->present = std::move(present);
    ctx.control = std::move(control);
    ctx.sink.host = host;
    return ctx;
  }

  /**
   * @brief Set config::audio.sink for one test and put it back afterwards.
   */
  struct scoped_config_sink_t {
    std::string previous {config::audio.sink};

    explicit scoped_config_sink_t(const std::string &sink) {
      config::audio.sink = sink;
    }

    ~scoped_config_sink_t() {
      config::audio.sink = previous;
    }
  };

}  // namespace

/**
 * The bug these guard, from a KDE/Wayland report: a session using the virtual
 * display exclusively came back to the wrong speakers. Hermes read the host's
 * default sink 1.8 seconds *after* exclusive mode had already blanked the
 * monitors, so the sink it recorded was the fallback the sound server had moved
 * to, and it then restored the fallback 285 milliseconds before the monitors -
 * and their sink - were back.
 */
struct HostSinkHoldTest: testing::Test {};

TEST_F(HostSinkHoldTest, WaitsWhileTheRecordedSinkIsStillGone) {
  // The monitor is off, so its sink is not in the server's list. Restoring now
  // would name a sink that does not exist.
  const auto ctx = make_ctx("alsa_output.pci-0000_01_00.1.hdmi-stereo-extra1", {"raop_sink.shitbox"});
  EXPECT_TRUE(host_sink_restore_pending(ctx));
}

TEST_F(HostSinkHoldTest, StopsWaitingOnceTheSinkIsBack) {
  // The same sink name, rebuilt by the sound server under a new index once the
  // monitor came back. The name is what is restored, so the name is what counts.
  const auto ctx = make_ctx(
    "alsa_output.pci-0000_01_00.1.hdmi-stereo-extra1",
    {"raop_sink.shitbox", "alsa_output.pci-0000_01_00.1.hdmi-stereo-extra1"}
  );
  EXPECT_FALSE(host_sink_restore_pending(ctx));
}

TEST_F(HostSinkHoldTest, DoesNotWaitWhenNothingWasRecorded) {
  // No default sink was found when the context was taken and none is
  // configured: there is nothing to go back to, so the release must not sit
  // out the full timeout for a name it will never restore.
  const scoped_config_sink_t config_sink {""};
  const auto ctx = make_ctx("", {"raop_sink.shitbox"});
  EXPECT_FALSE(host_sink_restore_pending(ctx));
}

TEST_F(HostSinkHoldTest, WaitsForTheConfiguredSinkWhenNoHostSinkWasRecorded) {
  // With no host default recorded the restore falls back to the configured
  // sink, so that is the one whose return has to be waited for.
  const scoped_config_sink_t config_sink {"alsa_output.configured"};
  EXPECT_TRUE(host_sink_restore_pending(make_ctx("", {"raop_sink.shitbox"})));
  EXPECT_FALSE(host_sink_restore_pending(make_ctx("", {"alsa_output.configured"})));
}

TEST_F(HostSinkHoldTest, ReleaseWithoutAHoldIsANoOp) {
  // terminate() calls this for every session, including the ones that never
  // touched the monitors.
  release_host_sink();
  release_host_sink();
}

TEST_F(HostSinkHoldTest, HoldAndReleaseDoNotBlockTheSessionPaths) {
  // hold_host_sink() runs on the launch path and release_host_sink() on
  // teardown; the waiting belongs to the thread the release starts, not to
  // either caller. Without a sound server there is no context to hold, which
  // this still has to survive.
  const auto started = std::chrono::steady_clock::now();
  hold_host_sink();
  hold_host_sink();
  release_host_sink();
  release_host_sink();
  hold_host_sink();
  release_host_sink();
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds {5});
}

struct AudioTest: PlatformTestSuite, testing::WithParamInterface<std::tuple<std::basic_string_view<char>, config_t>> {
  void SetUp() override {
    m_config = std::get<1>(GetParam());
    m_mail = std::make_shared<safe::mail_raw_t>();
  }

  config_t m_config;
  safe::mail_t m_mail;
};

constexpr std::bitset<config_t::MAX_FLAGS> config_flags(const int flag = -1) {
  std::bitset<3> result = std::bitset<config_t::MAX_FLAGS>();
  if (flag >= 0) {
    result.set(flag);
  }
  return result;
}

INSTANTIATE_TEST_SUITE_P(
  Configurations,
  AudioTest,
  testing::Values(
    std::make_tuple("HIGH_STEREO", config_t {5, 2, 0x3, {0}, config_flags(config_t::HIGH_QUALITY)}),
    std::make_tuple("SURROUND51", config_t {5, 6, 0x3F, {0}, config_flags()}),
    std::make_tuple("SURROUND71", config_t {5, 8, 0x63F, {0}, config_flags()}),
    std::make_tuple("SURROUND51_CUSTOM", config_t {5, 6, 0x3F, {6, 4, 2, {0, 1, 4, 5, 2, 3}}, config_flags(config_t::CUSTOM_SURROUND_PARAMS)})
  ),
  [](const auto &info) {
    return std::string(std::get<0>(info.param));
  }
);

TEST_P(AudioTest, TestEncode) {
  std::thread timer([&] {
    // Terminate the audio capture after 100 ms
    std::this_thread::sleep_for(100ms);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    const auto audio_packets = m_mail->queue<packet_t>(mail::audio_packets);
    shutdown_event->raise(true);
    audio_packets->stop();
  });
  std::thread capture([&] {
    const auto packets = m_mail->queue<packet_t>(mail::audio_packets);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    while (const auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }
      if (auto packet_data = packet->second; packet_data.size() == 0) {
        FAIL() << "Empty packet data";
      }
    }
  });
  audio::capture(m_mail, m_config, nullptr);

  timer.join();
  capture.join();
}
