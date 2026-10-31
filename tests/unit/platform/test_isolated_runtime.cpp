/**
 * @file test_isolated_runtime.cpp
 * @brief A slow account broker must not lock every other session's registry.
 */
#ifdef __linux__
  #include <chrono>
  #include <cstring>
  #include <filesystem>
  #include <future>
  #include <gtest/gtest.h>
  #include <optional>
  #include <poll.h>
  #include <src/process.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>

namespace proc {
  void seed_isolated_runtime_for_test(uint32_t id, const std::string &client_uuid);
  void erase_isolated_runtime_for_test(uint32_t id);
}  // namespace proc

namespace {
  using namespace std::chrono_literals;

  class IsolatedRuntimeConcurrencyTest: public testing::Test {
  protected:
    void SetUp() override {
      std::string pattern = (std::filesystem::temp_directory_path() / "hermes-registry-XXXXXX").string();
      const char *root = mkdtemp(pattern.data());
      ASSERT_NE(root, nullptr);
      directory = root;
      const auto path = (directory / "broker.sock").string();
      listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (listener < 0 && errno == EPERM && !std::getenv("HERMES_TEST_REQUIRE_UNIX_SOCKETS")) {
        GTEST_SKIP() << "Unix sockets are required by the isolated integration CI";
      }
      ASSERT_GE(listener, 0);
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      ASSERT_LT(path.size(), sizeof(address.sun_path));
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
      ASSERT_EQ(listen(listener, 1), 0);
      if (const char *value = std::getenv("HERMES_SESSION_BROKER_SOCKET")) {
        previous_socket = value;
      }
      setenv("HERMES_SESSION_BROKER_SOCKET", path.c_str(), 1);
      changed_environment = true;
      proc::seed_isolated_runtime_for_test(id, "abcd-1234");
      seeded = true;
    }

    void TearDown() override {
      if (seeded) {
        proc::erase_isolated_runtime_for_test(id);
      }
      if (listener >= 0) {
        close(listener);
      }
      if (changed_environment) {
        if (previous_socket) {
          setenv("HERMES_SESSION_BROKER_SOCKET", previous_socket->c_str(), 1);
        } else {
          unsetenv("HERMES_SESSION_BROKER_SOCKET");
        }
      }
      if (!directory.empty()) {
        std::filesystem::remove_all(directory);
      }
    }

    template<typename Query>
    void check(Query query, bool remove_pending_runtime = false) {
      std::promise<void> accepted;
      auto accepted_future = accepted.get_future();
      std::promise<void> release;
      auto release_future = release.get_future();
      auto broker = std::async(std::launch::async, [&]() {
        pollfd ready {listener, POLLIN, 0};
        if (poll(&ready, 1, 2000) != 1) {
          return;
        }
        const int peer = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (peer < 0) {
          return;
        }
        char buffer[1024];
        while (read(peer, buffer, sizeof(buffer)) > 0) {}
        accepted.set_value();
        release_future.wait();
        constexpr char reply[] = "OK active hermes-session-01.service\n";
        send(peer, reply, sizeof(reply) - 1, MSG_NOSIGNAL);
        close(peer);
      });
      auto request = std::async(std::launch::async, query);
      const auto connected = accepted_future.wait_for(2s);
      if (connected != std::future_status::ready) {
        release.set_value();
        request.wait();
        broker.wait();
        FAIL() << "The query did not reach the test broker";
      }
      auto lookup = std::async(std::launch::async, []() {
        return proc::proc.get_session_env(id)["HERMES_REGISTRY_TEST"].to_string();
      });
      const auto status = lookup.wait_for(250ms);
      if (remove_pending_runtime && status == std::future_status::ready) {
        proc::erase_isolated_runtime_for_test(id);
        seeded = false;
      }
      release.set_value();
      EXPECT_EQ(status, std::future_status::ready) << "A broker RPC held the global session registry lock";
      EXPECT_EQ(lookup.get(), "ready");
      EXPECT_TRUE(request.get());
      broker.get();
    }

    static constexpr uint32_t id = 0xfffffff0;
    int listener = -1;
    std::filesystem::path directory;
    std::optional<std::string> previous_socket;
    bool changed_environment = false;
    bool seeded = false;
  };

  TEST_F(IsolatedRuntimeConcurrencyTest, AppIdLookupDoesNotBlockOtherSessionMetadata) {
    check([]() {
      return proc::proc.running_for_client("abcd-1234") == 123;
    });
  }

  TEST_F(IsolatedRuntimeConcurrencyTest, AppUuidLookupDoesNotBlockOtherSessionMetadata) {
    check([]() {
      return proc::proc.running_app_uuid_for_client("abcd-1234") == "runtime-test-app";
    });
  }

  TEST_F(IsolatedRuntimeConcurrencyTest, ResumeDoesNotBlockOtherSessionMetadata) {
    check([]() {
      auto session = std::make_shared<rtsp_stream::launch_session_t>();
      session->unique_id = "abcd-1234";
      return proc::proc.prepare_isolated_resume(session) && session->isolated_runtime_owner_id == id;
    });
  }

  TEST_F(IsolatedRuntimeConcurrencyTest, ResumeRejectsARuntimeRemovedWhileTheBrokerReplies) {
    check([]() {
      auto session = std::make_shared<rtsp_stream::launch_session_t>();
      session->unique_id = "abcd-1234";
      return !proc::proc.prepare_isolated_resume(session) && !session->isolated_session;
    },
          true);
  }

}  // namespace
#endif
