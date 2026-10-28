/**
 * @file test_session_audio.cpp
 * @brief Real private PipeWire graphs, independent of a GPU or desktop server.
 */
#ifdef __linux__
  #include <algorithm>
  #include <chrono>
  #include <filesystem>
  #include <fstream>
  #include <future>
  #include <gtest/gtest.h>
  #include <map>
  #include <optional>
  #include <src/platform/linux/clipboard_process.h>
  #include <src/platform/linux/session_audio.h>
  #include <sys/stat.h>

namespace {
  namespace fs = std::filesystem;
  namespace audio = platf::session_audio;
  using namespace std::chrono_literals;

  class SessionAudioLiveTest: public testing::Test {
  protected:
    void SetUp() override {
      if (!std::getenv("HERMES_TEST_REQUIRE_PRIVATE_AUDIO")) {
        GTEST_SKIP() << "Private audio integration runs in the isolated-session CI job";
      }
      for (const char *name : {"XDG_RUNTIME_DIR", "PULSE_SERVER", "PULSE_SINK", "PULSE_SOURCE", "PIPEWIRE_REMOTE", "PATH"}) {
        const auto *value = std::getenv(name);
        environment[name] = value ? std::optional<std::string> {value} : std::nullopt;
      }
      std::string pattern = (fs::temp_directory_path() / "hermes-audio-XXXXXX").string();
      const char *directory = mkdtemp(pattern.data());
      ASSERT_NE(directory, nullptr);
      root = directory;
      setenv("XDG_RUNTIME_DIR", root.c_str(), 1);
    }

    void TearDown() override {
      for (const auto &[name, value] : environment) {
        if (value) {
          setenv(name.c_str(), value->c_str(), 1);
        } else {
          unsetenv(name.c_str());
        }
      }
      if (!root.empty()) {
        fs::remove_all(root);
      }
    }

    fs::path directory(const char *name) {
      const auto path = root / name;
      fs::create_directories(path);
      chmod(path.c_str(), 0700);
      return path;
    }

    nlohmann::json query(const audio::server_t &server, const char *command, const char *kind = nullptr) {
      const auto address = server.pulse_server();
      const char *arguments[] = {"pactl", "--server", address.c_str(), "--format=json", command, kind, nullptr};
      platf::clipboard::pipe_t pipe("/usr/bin/pactl", arguments, false);
      EXPECT_NE(pipe.get(), nullptr);
      if (!pipe.get()) {
        return nullptr;
      }
      std::string text;
      char buffer[4096];
      while (const auto bytes = fread(buffer, 1, sizeof(buffer), pipe.get())) {
        text.append(buffer, bytes);
      }
      EXPECT_TRUE(pipe.finish()) << text;
      const auto result = nlohmann::json::parse(text, nullptr, false);
      EXPECT_FALSE(result.is_discarded()) << text;
      return result;
    }

    void record_silence(std::unique_ptr<audio::server_t> &server, const std::string &sink) {
      ASSERT_TRUE(server);
      auto capture = audio::microphone(server->pulse_server(), sink, platf::speaker::map_stereo, 2, 48000, 480);
      ASSERT_TRUE(capture);
      std::vector<float> samples(960);
      auto result = std::async(std::launch::async, [&]() {
        return capture->sample(samples);
      });
      // The fixture's job has a hard timeout too; a healthy null sink supplies
      // silence immediately and must not require host audio playback.
      const auto status = result.wait_for(3s);
      if (status != std::future_status::ready) {
        server.reset();  // Disconnect the reader before joining its worker.
      }
      EXPECT_EQ(status, std::future_status::ready);
      EXPECT_EQ(result.get(), platf::capture_e::ok);
      EXPECT_TRUE(std::ranges::all_of(samples, [](float value) {
        return value == 0.0f;
      }));
    }

    fs::path root;
    std::map<std::string, std::optional<std::string>> environment;
  };

  TEST_F(SessionAudioLiveTest, ConcurrentGraphsKeepHostDefaultsAndDevicesSeparate) {
    auto host = audio::start(directory("host").string(), "sink-host-test", 2, "");
    ASSERT_TRUE(host);
    const auto before = query(*host, "info");
    ASSERT_TRUE(before.is_object());
    setenv("PULSE_SERVER", host->pulse_server().c_str(), 1);
    setenv("PULSE_SINK", "sink-host-test", 1);
    setenv("PULSE_SOURCE", "source-that-only-the-host-has", 1);
    setenv("PIPEWIRE_REMOTE", host->pipewire_remote().c_str(), 1);

    auto first = audio::start(directory("one").string(), "sink-session-one", 2, "source-session-one");
    auto second = audio::start(directory("two").string(), "sink-session-two", 6, "");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    const auto first_sinks = query(*first, "list", "sinks");
    const auto second_sinks = query(*second, "list", "sinks");
    ASSERT_TRUE(first_sinks.is_array());
    ASSERT_TRUE(second_sinks.is_array());
    ASSERT_EQ(first_sinks.size(), 1);
    ASSERT_EQ(second_sinks.size(), 1);
    EXPECT_EQ(first_sinks.at(0).at("name"), "sink-session-one");
    EXPECT_EQ(second_sinks.at(0).at("name"), "sink-session-two");
    ASSERT_TRUE(second_sinks.at(0).contains("sample_specification")) << second_sinks.dump();
    EXPECT_NE(second_sinks.at(0).at("sample_specification").get<std::string>().find("6ch"), std::string::npos);
    const auto first_info = query(*first, "info");
    EXPECT_EQ(first_info.at("default_sink_name"), "sink-session-one");
    EXPECT_EQ(first_info.at("default_source_name"), "source-session-one");
    const auto sources = query(*first, "list", "sources");
    ASSERT_TRUE(sources.is_array());
    for (const auto &source : sources) {
      const auto name = source.at("name").get<std::string>();
      EXPECT_TRUE(name == "source-session-one" || name == "sink-session-one.monitor") << name;
    }
    first.reset();
    EXPECT_EQ(query(*second, "info").at("default_sink_name"), "sink-session-two");
    const auto after = query(*host, "info");
    EXPECT_EQ(after.at("default_sink_name"), before.at("default_sink_name"));
    EXPECT_EQ(after.at("default_source_name"), before.at("default_source_name"));
    EXPECT_EQ(query(*host, "list", "sinks").size(), 1);
  }

  TEST_F(SessionAudioLiveTest, CaptureCanDisconnectAndResumeWithoutRestartingTheServer) {
    auto server = audio::start(directory("resume").string(), "sink-session-resume", 2, "");
    ASSERT_TRUE(server);
    record_silence(server, "sink-session-resume");
    ASSERT_TRUE(server);
    record_silence(server, "sink-session-resume");
    ASSERT_TRUE(server);
    EXPECT_EQ(query(*server, "info").at("default_sink_name"), "sink-session-resume");
  }

  TEST_F(SessionAudioLiveTest, TeardownRemovesTheMicrophoneFifoAndAllowsRestart) {
    const auto path = directory("restart");
    const auto fifo = audio::session_mic_fifo("source-session-restart");
    auto server = audio::start(path.string(), "sink-session-restart", 2, "source-session-restart");
    ASSERT_TRUE(server);
    EXPECT_TRUE(fs::is_fifo(fifo));
    EXPECT_TRUE(audio::open_mic("source-session-restart"));
    server.reset();
    EXPECT_FALSE(fs::exists(fifo));
    server = audio::start(path.string(), "sink-session-restart", 2, "source-session-restart");
    ASSERT_TRUE(server);
    EXPECT_EQ(query(*server, "info").at("default_source_name"), "source-session-restart");
  }

  TEST_F(SessionAudioLiveTest, ASessionManagerThatExitsDuringStartupIsRejected) {
    const auto fake_bin = directory("fake-bin");
    const auto wireplumber = fake_bin / "wireplumber";
    std::ofstream {wireplumber} << "#!/bin/sh\nexit 0\n";
    ASSERT_EQ(chmod(wireplumber.c_str(), 0700), 0);
    const auto *path = std::getenv("PATH");
    const auto search_path = fake_bin.string() + ':' + (path ? path : "/usr/bin:/bin");
    setenv("PATH", search_path.c_str(), 1);
    const auto session_path = directory("failed-manager");
    auto server = audio::start(session_path.string(), "sink-session-failed", 2, "source-session-failed");
    EXPECT_FALSE(server);
    EXPECT_FALSE(fs::exists(audio::session_mic_fifo("source-session-failed")));
  }
}  // namespace
#endif
