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

#if defined(__linux__) && defined(SUNSHINE_BUILD_SDBUS)

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

  // lib includes
  #include <systemd/sd-bus.h>
  #include <unistd.h>

  // local includes
  #include "src/config.h"
  #include "src/platform/linux/misc.h"

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

      service_thread_ = std::thread([this] {
        while (!stop_.load()) {
          sd_bus_process(service_bus_, nullptr);
          sd_bus_wait(service_bus_, 50'000);
        }
      });
    }

    void TearDown() override {
      config::sunshine.inhibit_screen_lock = false;
      // Release anything a failing test left behind; this also waits for the
      // fake service's reply, so the state below is settled.
      platf::streaming_will_stop();

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

    bool wait_for_inhibit(std::chrono::milliseconds timeout = std::chrono::seconds {5}) {
      std::unique_lock lock {mutex_};
      return inhibited_changed_.wait_for(lock, timeout, [this] {
        return inhibited_;
      });
    }

    std::mutex mutex_;
    std::condition_variable inhibited_changed_;
    bool inhibited_ {false};
    int inhibit_count_ {0};
    std::uint32_t last_cookie_ {0};
    std::uint32_t last_uninhibited_cookie_ {0};
    std::string application_;
    std::string reason_;

  private:
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
        self->last_cookie_ = static_cast<std::uint32_t>(self->inhibit_count_);
        self->application_ = application;
        self->reason_ = reason;
        cookie = self->last_cookie_;
      }
      self->inhibited_changed_.notify_all();
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
      return sd_bus_reply_method_return(message, nullptr);
    }

    sd_bus *service_bus_ {nullptr};
    sd_bus_slot *slot_ {nullptr};
    std::thread service_thread_;
    std::atomic<bool> stop_ {false};
    pid_t daemon_pid_ {0};
    bool had_previous_address_ {false};
    std::string previous_address_;
  };

}  // namespace

TEST_F(ScreenLockInhibitTest, HoldsTheInhibitionWhileAClientIsConnected) {
  config::sunshine.inhibit_screen_lock = true;

  platf::streaming_will_start();
  ASSERT_TRUE(wait_for_inhibit()) << "the fake screensaver never received Inhibit";
  {
    std::lock_guard lock {mutex_};
    EXPECT_EQ(inhibit_count_, 1);
    EXPECT_EQ(application_, "Hermes");
    EXPECT_FALSE(reason_.empty());
  }

  // A second platform start (another client) must reuse the same inhibition
  // rather than dropping the first one.
  platf::streaming_will_start();

  platf::streaming_will_stop();
  {
    std::lock_guard lock {mutex_};
    EXPECT_FALSE(inhibited_);
    EXPECT_EQ(inhibit_count_, 1);
    EXPECT_EQ(last_uninhibited_cookie_, last_cookie_);
  }

  config::sunshine.inhibit_screen_lock = false;
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
  ASSERT_TRUE(wait_for_inhibit());
  platf::streaming_will_stop();

  platf::streaming_will_start();
  ASSERT_TRUE(wait_for_inhibit());
  platf::streaming_will_stop();

  std::lock_guard lock {mutex_};
  EXPECT_EQ(inhibit_count_, 2);
  EXPECT_FALSE(inhibited_);
}

#endif
