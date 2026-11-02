// SPDX-License-Identifier: GPL-3.0-only
/** @file test_session_broker_client.cpp
 *  @brief Unix transport deadlines and session liveness without a real broker.
 */
#ifdef __linux__
  #include "src/platform/linux/session_broker.h"
  #include "src/platform/linux/session_broker_transport.h"

  #include <chrono>
  #include <cstring>
  #include <filesystem>
  #include <functional>
  #include <gtest/gtest.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <thread>
  #include <unistd.h>

namespace {
  using namespace std::chrono_literals;
  namespace broker = platf::session_broker;

  class SessionBrokerClientTest: public testing::Test {
  protected:
    void SetUp() override {
      std::string pattern = (std::filesystem::temp_directory_path() / "hermes-rpc-XXXXXX").string();
      const auto *directory = ::mkdtemp(pattern.data());
      ASSERT_NE(directory, nullptr);
      root = directory;
      path = (root / "socket").string();
      if (const auto *value = ::getenv("HERMES_SESSION_BROKER_SOCKET")) {
        old_path = value;
      }
      ::setenv("HERMES_SESSION_BROKER_SOCKET", path.c_str(), 1);
      listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (listener < 0 && errno == EPERM && !::getenv("HERMES_TEST_REQUIRE_UNIX_SOCKETS")) {
        GTEST_SKIP() << "This namespace denies Unix sockets; IPC tests run in the broker CI job";
      }
      ASSERT_GE(listener, 0);
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      ASSERT_LT(path.size(), sizeof(address.sun_path));
      std::memcpy(address.sun_path, path.data(), path.size());
      ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0);
      ASSERT_EQ(::listen(listener, 0), 0);
    }

    void TearDown() override {
      if (server.joinable()) {
        server.join();
      }
      if (listener >= 0) {
        ::close(listener);
      }
      if (old_path) {
        ::setenv("HERMES_SESSION_BROKER_SOCKET", old_path->c_str(), 1);
      } else {
        ::unsetenv("HERMES_SESSION_BROKER_SOCKET");
      }
      std::filesystem::remove_all(root);
    }

    void serve(const std::function<void(int)> &job) {
      if (server.joinable()) {
        server.join();
      }
      server = std::thread {[this, job]() {
        pollfd ready {listener, POLLIN, 0};
        if (::poll(&ready, 1, 2000) != 1) {
          return;
        }
        const int peer = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (peer < 0) {
          return;
        }
        job(peer);
        ::close(peer);
      }};
    }

    void answer(std::string reply) {
      serve([this, reply = std::move(reply)](int peer) {
        received.clear();
        char buffer[1024];
        ssize_t count;
        while ((count = ::read(peer, buffer, sizeof(buffer))) > 0) {
          received.append(buffer, count);
        }
        ::send(peer, reply.data(), reply.size(), MSG_NOSIGNAL);
      });
    }

    void join() {
      if (server.joinable()) {
        server.join();
      }
    }

    std::filesystem::path root;
    std::string path, received;
    std::optional<std::string> old_path;
    int listener = -1;
    std::thread server;
  };

  TEST_F(SessionBrokerClientTest, AcceptsOnlyACompleteReplyAndPreservesTheRequest) {
    answer("OK active hermes-session-01.service\n");
    const auto reply = broker::detail::exchange(path, "STATUS abcd-1234\n", 1s);
    ASSERT_TRUE(reply.reply) << reply.error;
    EXPECT_EQ(*reply.reply, "OK active hermes-session-01.service");
    join();
    EXPECT_EQ(received, "STATUS abcd-1234\n");
  }

  TEST_F(SessionBrokerClientTest, AssemblesAFragmentedReply) {
    serve([](int peer) {
      ::send(peer, "OK act", 6, MSG_NOSIGNAL);
      std::this_thread::sleep_for(10ms);
      constexpr std::string_view end {"ive hermes-session-01.service\n"};
      ::send(peer, end.data(), end.size(), MSG_NOSIGNAL);
    });
    const auto reply = broker::detail::exchange(path, "STATUS abcd\n", 1s);
    ASSERT_TRUE(reply.reply) << reply.error;
    EXPECT_EQ(*reply.reply, "OK active hermes-session-01.service");
  }

  TEST_F(SessionBrokerClientTest, RejectsPartialEmptyEmbeddedNulAndOversizedReplies) {
    for (const auto &text : {std::string {"OK active"}, std::string {"\n"}, std::string {"OK\0active\n", 10}, std::string(5000, 'x') + '\n'}) {
      answer(text);
      const auto reply = broker::detail::exchange(path, "STATUS abcd\n", 1s);
      EXPECT_FALSE(reply.reply);
      EXPECT_FALSE(reply.error.empty());
      join();
    }
  }

  TEST_F(SessionBrokerClientTest, APeerThatDoesNotReplyCannotExtendTheDeadline) {
    serve([](int) {
      std::this_thread::sleep_for(500ms);
    });
    const auto started = std::chrono::steady_clock::now();
    const auto reply = broker::detail::exchange(path, "STATUS abcd\n", 50ms);
    EXPECT_FALSE(reply.reply);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 350ms);
    EXPECT_TRUE(reply.error.starts_with("read reply:")) << reply.error;
  }

  TEST_F(SessionBrokerClientTest, BackpressuredWritesCannotExtendTheDeadline) {
    serve([](int) {
      std::this_thread::sleep_for(500ms);
    });
    const auto started = std::chrono::steady_clock::now();
    const auto reply = broker::detail::exchange(path, "START abcd\nARG " + std::string(60000, 'x') + "\nEND\n", 50ms);
    EXPECT_FALSE(reply.reply);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 350ms);
    EXPECT_TRUE(reply.error.starts_with("send request:")) << reply.error;
  }

  TEST_F(SessionBrokerClientTest, AFullListenQueueIsRetriedUntilTheConnectDeadline) {
    const int queued = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(queued, 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    ASSERT_EQ(::connect(queued, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0);
    const auto started = std::chrono::steady_clock::now();
    const auto reply = broker::detail::exchange(path, "STATUS abcd\n", 50ms);
    ::close(queued);
    EXPECT_FALSE(reply.reply);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 350ms);
    EXPECT_TRUE(reply.error.starts_with("connect:")) << reply.error;
  }

  TEST_F(SessionBrokerClientTest, PeerDisconnectIsReportedWithoutSigpipe) {
    serve([](int peer) {
      ::shutdown(peer, SHUT_RDWR);
    });
    const auto reply = broker::detail::exchange(path, "START abcd\nARG " + std::string(60000, 'x') + "\nEND\n", 1s);
    EXPECT_FALSE(reply.reply);
  }

  TEST(SessionBrokerPolicyTest, MutationsHaveALongerBudgetThanLivenessPolling) {
    EXPECT_EQ(broker::detail::request_timeout("STATUS abcd\n"), 15s);
    EXPECT_EQ(broker::detail::request_timeout("SOCKET abcd\n"), 15s);
    for (const auto *verb : {"ENSURE", "START", "STOP", "PURGE"}) {
      EXPECT_EQ(broker::detail::request_timeout(std::string {verb} + " abcd\n"), 180s);
    }
  }

  TEST_F(SessionBrokerClientTest, LivenessSeparatesStoppedFromUnavailableAndUnknown) {
    for (const auto *state : {"active", "activating", "reloading"}) {
      answer(std::string {"OK "} + state + " hermes-session-01.service\n");
      EXPECT_EQ(broker::query_active("abcd-1234"), std::optional {true});
      join();
    }
    for (const auto *reply : {"OK inactive hermes-session-01.service\n", "OK failed hermes-session-01.service\n", "OK deactivating hermes-session-01.service\n", "ERR notfound no account is mapped\n"}) {
      answer(reply);
      EXPECT_EQ(broker::query_active("abcd-1234"), std::optional {false});
      join();
    }
    for (const auto *reply : {"ERR busy try later\n", "ERR state unavailable\n", "OK maintenance hermes-session-01.service\n", "OK active\n", "OK active wrong.service\n"}) {
      answer(reply);
      EXPECT_EQ(broker::query_active("abcd-1234"), std::nullopt);
      join();
    }
    ::close(listener);
    listener = -1;
    EXPECT_EQ(broker::query_active("abcd-1234"), std::nullopt);
  }

  TEST_F(SessionBrokerClientTest, AccountParsingRejectsRootSentinelAndPartialNumericUids) {
    for (const auto *uid : {"0", "-1", "+12", "123garbage", "4294967295", "4294967296"}) {
      answer(std::string {"OK hermes-s01 "} + uid + " /var/lib/hermes/sessions/hermes-s01\n");
      EXPECT_FALSE(broker::ensure("abcd-1234"));
      join();
    }
    answer("OK hermes-s01 12001 /var/lib/hermes/session homes/hermes-s01\n");
    const auto account = broker::ensure("abcd-1234");
    ASSERT_TRUE(account);
    EXPECT_EQ(account->uid, 12001U);
    EXPECT_EQ(account->home, "/var/lib/hermes/session homes/hermes-s01");
  }

  TEST_F(SessionBrokerClientTest, RejectsProtocolInjectionAndInvalidRequestsBeforeConnecting) {
    EXPECT_FALSE(broker::ensure("abcd\nSTOP dcba"));
    EXPECT_FALSE(broker::lookup("../root"));
    EXPECT_FALSE(broker::start("abcd", {"/usr/bin/true"}, {}, "app\nEND"));
    EXPECT_FALSE(broker::detail::exchange(path, "STATUS abcd", 10ms).reply);
    EXPECT_FALSE(broker::detail::exchange(path, std::string(65537, 'x') + '\n', 10ms).reply);
    EXPECT_FALSE(broker::detail::exchange(std::string(200, 'x'), "STATUS abcd\n", 10ms).reply);
  }
}  // namespace
#endif
