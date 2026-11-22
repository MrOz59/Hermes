/**
 * @file tests/unit/platform/test_screen_lock_inhibit.cpp
 * @brief Test that a connected client can hold off the screen saver.
 *
 * The platform hook is exercised against a private D-Bus session bus and a
 * fake org.freedesktop.ScreenSaver service, so the test covers the two things
 * a one-shot helper cannot: that Inhibit is called with the documented
 * signature, and that the caller keeps the connection open until UnInhibit.
 */
#include "../../tests_common.h"

#if defined(SUNSHINE_BUILD_SDBUS)

  // standard includes
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <csignal>
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <mutex>
  #include <string>
  #include <thread>
  #include <vector>

  // lib includes
  #include <systemd/sd-bus.h>
  #include <unistd.h>

  // local includes
  #include "src/config.h"
  #include "src/platform/linux/misc.h"
  #include "src/session_activity_counter.h"

namespace {

  constexpr auto screen_saver_service = "org.freedesktop.ScreenSaver";
  constexpr auto screen_saver_path = "/org/freedesktop/ScreenSaver";
  constexpr auto screen_saver_interface = "org.freedesktop.ScreenSaver";

  class ScreenLockInhibitTest: public ::testing::Test {
  protected:
    void SetUp() override {
      if (!start_private_session_bus()) {
        GTEST_SKIP() << "dbus-daemon is not available for a private session bus";
      }

      if (const int status = sd_bus_open_user(&service_bus_); status < 0) {
        GTEST_SKIP() << "could not connect to the private session bus: " << std::strerror(-status);
      }
      if (const int status = sd_bus_request_name(service_bus_, screen_saver_service, 0); status < 0) {
        GTEST_SKIP() << "could not own " << screen_saver_service << ": " << std::strerror(-status);
      }
      if (const int status = sd_bus_add_object_vtable(service_bus_, &slot_, screen_saver_path, screen_saver_interface, screen_saver_vtable(), this); status < 0) {
        GTEST_SKIP() << "could not publish " << screen_saver_path << ": " << std::strerror(-status);
      }
      service_online_.store(true);

      service_thread_ = std::thread([this] {
        while (!stop_.load()) {
          switch (service_command_.exchange(service_command_e::none)) {
            case service_command_e::none:
              break;
            case service_command_e::restart:
              // Simulate a real process restart: disconnect the old D-Bus
              // connection and register a fresh unique bus owner. Releasing
              // and re-requesting the name on one connection does not model a
              // new service instance and can reuse a stale owner identity.
              disconnect_service();
              if (!connect_service()) {
                restart_failed_.store(true);
              }
              break;
            case service_command_e::down:
              disconnect_service();
              break;
            case service_command_e::up:
              if (!service_bus_ && !connect_service()) {
                restart_failed_.store(true);
              }
              break;
          }
          if (!service_bus_) {
            std::this_thread::sleep_for(std::chrono::milliseconds {10});
            continue;
          }
          sd_bus_process(service_bus_, nullptr);
          sd_bus_wait(service_bus_, 50'000);
        }
      });
    }

    void TearDown() override {
      config::sunshine.inhibit_screen_lock = false;
      // The platform hook is asynchronous. The fake service must remain alive
      // until Hermes has observed the stop and released its inhibition.
      platf::streaming_will_stop();
      wait_for_inhibition(false);

      stop_.store(true);
      if (service_thread_.joinable()) {
        service_thread_.join();
      }
      if (slot_) {
        sd_bus_slot_unref(slot_);
        slot_ = nullptr;
      }
      if (service_bus_) {
        sd_bus_unref(service_bus_);
        service_bus_ = nullptr;
      }

      if (daemon_pid_ > 0) {
        ::kill(daemon_pid_, SIGTERM);
        daemon_pid_ = 0;
      }
      if (had_previous_address_) {
        ::setenv("DBUS_SESSION_BUS_ADDRESS", previous_address_.c_str(), 1);
      } else {
        ::unsetenv("DBUS_SESSION_BUS_ADDRESS");
      }
    }

    bool wait_for_inhibition(bool expected, std::chrono::milliseconds timeout = std::chrono::seconds {5}) {
      std::unique_lock lock {mutex_};
      return inhibited_changed_.wait_for(lock, timeout, [this, expected] {
        return inhibited_ == expected;
      });
    }

    bool wait_for_count(int count, std::chrono::milliseconds timeout = std::chrono::seconds {5}) {
      std::unique_lock lock {mutex_};
      return inhibited_changed_.wait_for(lock, timeout, [this, count] {
        return inhibit_count_ >= count;
      });
    }

    void restart_fake_service() {
      service_command_.store(service_command_e::restart);
    }

    void stop_fake_service() {
      service_command_.store(service_command_e::down);
    }

    void start_fake_service() {
      service_command_.store(service_command_e::up);
    }

    // The commands above run on the service thread; wait until it applied one.
    bool wait_for_service_online(bool expected) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {5};
      while (service_online_.load() != expected) {
        if (std::chrono::steady_clock::now() >= deadline) {
          return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
      }
      return true;
    }

    bool fake_service_restart_failed() const {
      return restart_failed_.load();
    }

    void delay_inhibit_reply(std::chrono::milliseconds duration) {
      inhibit_reply_delay_ms_.store(static_cast<int>(duration.count()));
    }

    std::mutex mutex_;
    std::condition_variable inhibited_changed_;
    bool inhibited_ {false};
    int inhibit_count_ {0};
    // Not reset when the fake restarts, so every instance issues distinct
    // cookies and a stale cookie sent to a new instance is detectable.
    std::uint32_t service_cookie_counter_ {0};
    std::uint32_t last_cookie_ {0};
    std::uint32_t last_uninhibited_cookie_ {0};
    std::string application_;
    std::string reason_;

  private:
    enum class service_command_e {
      none,
      restart,
      down,
      up,
    };

    // Service-thread only. A fresh connection gets a fresh unique bus name.
    bool connect_service() {
      const bool online = sd_bus_open_user(&service_bus_) >= 0 &&
                          sd_bus_add_object_vtable(service_bus_, &slot_, screen_saver_path, screen_saver_interface, screen_saver_vtable(), this) >= 0 &&
                          sd_bus_request_name(service_bus_, screen_saver_service, 0) >= 0;
      service_online_.store(online);
      return online;
    }

    // Service-thread only. Dropping the connection drops every inhibition the
    // old instance held, like a crashing or restarting screensaver would.
    void disconnect_service() {
      sd_bus_slot_unref(slot_);
      slot_ = nullptr;
      sd_bus_unref(service_bus_);
      service_bus_ = nullptr;
      service_online_.store(false);
      {
        std::lock_guard lock {mutex_};
        inhibited_ = false;
      }
      inhibited_changed_.notify_all();
    }

    bool start_private_session_bus() {
      FILE *pipe = ::popen("dbus-daemon --session --fork --print-address=1 --print-pid=1 2>/dev/null", "r");
      if (!pipe) {
        return false;
      }

      std::array<char, 512> line {};
      std::string address;
      std::string pid_text;
      if (std::fgets(line.data(), line.size(), pipe)) {
        address = line.data();
      }
      if (std::fgets(line.data(), line.size(), pipe)) {
        pid_text = line.data();
      }
      if (::pclose(pipe) != 0) {
        return false;
      }

      const auto trim = [](std::string &value) {
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
          value.pop_back();
        }
      };
      trim(address);
      trim(pid_text);
      if (address.empty() || pid_text.empty()) {
        return false;
      }
      try {
        daemon_pid_ = std::stoi(pid_text);
      } catch (const std::exception &) {
        return false;
      }

      if (const char *previous = ::getenv("DBUS_SESSION_BUS_ADDRESS")) {
        had_previous_address_ = true;
        previous_address_ = previous;
      }
      if (::setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), 1) != 0) {
        return false;
      }
      return true;
    }

    static const sd_bus_vtable *screen_saver_vtable() {
      static const sd_bus_vtable table[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Inhibit", "ss", "u", on_inhibit, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("UnInhibit", "u", "", on_uninhibit, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END
      };
      return table;
    }

    static int on_inhibit(sd_bus_message *message, void *userdata, sd_bus_error *error) {
      auto *self = static_cast<ScreenLockInhibitTest *>(userdata);
      const char *application = nullptr;
      const char *reason = nullptr;
      if (sd_bus_message_read(message, "ss", &application, &reason) < 0) {
        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "an application and reason are required");
      }

      std::uint32_t cookie = 0;
      {
        std::lock_guard lock {self->mutex_};
        ++self->inhibit_count_;
        self->inhibited_ = true;
        self->last_cookie_ = ++self->service_cookie_counter_;
        self->application_ = application;
        self->reason_ = reason;
        cookie = self->last_cookie_;
      }
      self->inhibited_changed_.notify_all();
      const int delay_ms = self->inhibit_reply_delay_ms_.load();
      if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds {delay_ms});
      }
      return sd_bus_reply_method_return(message, "u", cookie);
    }

    static int on_uninhibit(sd_bus_message *message, void *userdata, sd_bus_error *error) {
      auto *self = static_cast<ScreenLockInhibitTest *>(userdata);
      std::uint32_t cookie = 0;
      if (sd_bus_message_read(message, "u", &cookie) < 0) {
        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "a cookie is required");
      }
      {
        std::lock_guard lock {self->mutex_};
        self->inhibited_ = false;
        self->last_uninhibited_cookie_ = cookie;
      }
      const int result = sd_bus_reply_method_return(message, nullptr);
      self->inhibited_changed_.notify_all();
      return result;
    }

    sd_bus *service_bus_ {nullptr};
    sd_bus_slot *slot_ {nullptr};
    std::thread service_thread_;
    std::atomic<bool> stop_ {false};
    std::atomic<service_command_e> service_command_ {service_command_e::none};
    std::atomic<bool> restart_failed_ {false};
    std::atomic<bool> service_online_ {false};
    std::atomic<int> inhibit_reply_delay_ms_ {0};
    pid_t daemon_pid_ {0};
    bool had_previous_address_ {false};
    std::string previous_address_;
  };

}  // namespace

TEST_F(ScreenLockInhibitTest, HoldsTheInhibitionWhileAClientIsConnected) {
  config::sunshine.inhibit_screen_lock = true;

  platf::streaming_will_start();
  ASSERT_TRUE(wait_for_inhibition(true)) << "the fake screensaver never received Inhibit";
  {
    std::lock_guard lock {mutex_};
    EXPECT_EQ(inhibit_count_, 1);
    EXPECT_EQ(application_, "Hermes");
    EXPECT_FALSE(reason_.empty());
  }

  platf::streaming_will_stop();
  ASSERT_TRUE(wait_for_inhibition(false)) << "the fake screensaver never received UnInhibit";
  {
    std::lock_guard lock {mutex_};
    EXPECT_EQ(inhibit_count_, 1);
    EXPECT_EQ(last_uninhibited_cookie_, last_cookie_);
  }
}

TEST_F(ScreenLockInhibitTest, LeavesTheScreensaverAloneWhenDisabled) {
  config::sunshine.inhibit_screen_lock = false;

  platf::streaming_will_start();
  platf::streaming_will_stop();

  std::lock_guard lock {mutex_};
  EXPECT_EQ(inhibit_count_, 0);
}

TEST_F(ScreenLockInhibitTest, CanInhibitAgainForANewClient) {
  config::sunshine.inhibit_screen_lock = true;

  platf::streaming_will_start();
  ASSERT_TRUE(wait_for_count(1));
  platf::streaming_will_stop();
  ASSERT_TRUE(wait_for_inhibition(false));

  platf::streaming_will_start();
  ASSERT_TRUE(wait_for_count(2));
  platf::streaming_will_stop();
  ASSERT_TRUE(wait_for_inhibition(false));

  std::lock_guard lock {mutex_};
  EXPECT_EQ(inhibit_count_, 2);
  EXPECT_FALSE(inhibited_);
}

TEST_F(ScreenLockInhibitTest, RestoresInhibitionAfterScreensaverRestart) {
  config::sunshine.inhibit_screen_lock = true;
  platf::streaming_will_start();
  ASSERT_TRUE(wait_for_count(1));

  // Replacing the fake service must yield another *unique* D-Bus sender.
  // Repeat to cover queued NameOwnerChanged signals that can arrive after
  // the replacement service has already answered the next Inhibit call.
  for (int expected = 2; expected <= 4; ++expected) {
    restart_fake_service();
    ASSERT_TRUE(wait_for_count(expected)) << "Hermes did not reinhibit after screensaver restart " << expected;
    ASSERT_FALSE(fake_service_restart_failed());
    EXPECT_TRUE(wait_for_inhibition(true));
  }

  platf::streaming_will_stop();
  ASSERT_TRUE(wait_for_inhibition(false)) << "the latest service did not receive UnInhibit";
  std::lock_guard lock {mutex_};
  EXPECT_EQ(inhibit_count_, 4) << "duplicate or missing inhibition after service replacement";
  EXPECT_EQ(last_uninhibited_cookie_, last_cookie_);
}

TEST_F(ScreenLockInhibitTest, InhibitsWhenTheScreensaverAppearsMidStream) {
  config::sunshine.inhibit_screen_lock = true;
  stop_fake_service();
  ASSERT_TRUE(wait_for_service_online(false));

  // Without an owner, Inhibit fails with ServiceUnknown. The worker must then
  // wait for NameOwnerChanged rather than give up for the rest of the stream.
  platf::streaming_will_start();
  std::this_thread::sleep_for(std::chrono::milliseconds {300});
  {
    std::lock_guard lock {mutex_};
    EXPECT_EQ(inhibit_count_, 0);
  }

  start_fake_service();
  ASSERT_TRUE(wait_for_service_online(true));
  ASSERT_TRUE(wait_for_count(1)) << "Hermes did not inhibit once the screensaver appeared";

  platf::streaming_will_stop();
  ASSERT_TRUE(wait_for_inhibition(false));
  std::lock_guard lock {mutex_};
  EXPECT_EQ(inhibit_count_, 1);
  EXPECT_EQ(last_uninhibited_cookie_, last_cookie_);
}

TEST_F(ScreenLockInhibitTest, StopDoesNotWaitForSlowInhibitCall) {
  config::sunshine.inhibit_screen_lock = true;
  delay_inhibit_reply(std::chrono::milliseconds {900});

  const auto start_time = std::chrono::steady_clock::now();
  platf::streaming_will_start();
  const auto start_elapsed = std::chrono::steady_clock::now() - start_time;
  EXPECT_LT(start_elapsed, std::chrono::milliseconds {600});

  ASSERT_TRUE(wait_for_count(1));
  const auto stop_time = std::chrono::steady_clock::now();
  platf::streaming_will_stop();
  const auto stop_elapsed = std::chrono::steady_clock::now() - stop_time;
  EXPECT_LT(stop_elapsed, std::chrono::milliseconds {600});

  // The in-flight Inhibit may finish after the stop was requested. The worker
  // must then UnInhibit with the returned cookie and not leave a stale lease.
  ASSERT_TRUE(wait_for_inhibition(false));
  std::lock_guard lock {mutex_};
  EXPECT_EQ(inhibit_count_, 1);
  EXPECT_EQ(last_uninhibited_cookie_, last_cookie_);
}


TEST_F(ScreenLockInhibitTest, TwoClientsKeepTheLeaseUntilBothDisconnect) {
  config::sunshine.inhibit_screen_lock = true;
  stream::session_activity_counter_t sessions;

  sessions.started([] { platf::streaming_will_start(); });
  sessions.started([] { platf::streaming_will_start(); });
  ASSERT_TRUE(wait_for_count(1));

  sessions.stopped([] { platf::streaming_will_stop(); });
  EXPECT_EQ(sessions.count(), 1u);
  {
    std::lock_guard lock {mutex_};
    EXPECT_EQ(inhibit_count_, 1);
    EXPECT_TRUE(inhibited_) << "the first disconnect released the remaining client's lease";
  }

  sessions.stopped([] { platf::streaming_will_stop(); });
  EXPECT_EQ(sessions.count(), 0u);
  ASSERT_TRUE(wait_for_inhibition(false));
  std::lock_guard lock {mutex_};
  EXPECT_EQ(inhibit_count_, 1);
  EXPECT_EQ(last_uninhibited_cookie_, last_cookie_);
}

TEST(SessionActivityCounterTest, ConcurrentClientHandoffsKeepCallbacksBalanced) {
  stream::session_activity_counter_t sessions;
  int first_starts = 0;
  int last_stops = 0;
  constexpr int thread_count = 8;
  constexpr int iterations = 200;

  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (int i = 0; i < thread_count; ++i) {
    workers.emplace_back([&] {
      for (int iteration = 0; iteration < iterations; ++iteration) {
        sessions.started([&] { ++first_starts; });
        std::this_thread::yield();
        sessions.stopped([&] { ++last_stops; });
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  EXPECT_EQ(sessions.count(), 0u);
  EXPECT_GT(first_starts, 0);
  EXPECT_EQ(first_starts, last_stops);
}

#endif
