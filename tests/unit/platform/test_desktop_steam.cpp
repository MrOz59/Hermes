/**
 * @file tests/unit/platform/test_desktop_steam.cpp
 * @brief Finding the user's Steam client, native or Flatpak.
 */
#include "../../tests_common.h"

#include <filesystem>
#include <fstream>
#include <src/platform/linux/desktop_steam.h>
#include <string>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using platf::desktop_steam::find;

  /// A /proc of made-up processes, removed afterwards.
  class FakeProc: public ::testing::Test {
  protected:
    fs::path root = fs::temp_directory_path() / ("hermes-desktop-steam-" + std::to_string(::getpid()));
    fs::path proc = root / "proc";

    void SetUp() override {
      fs::remove_all(root);
      fs::create_directories(proc);
    }

    void TearDown() override {
      fs::remove_all(root);
    }

    void process(int pid, const std::string &comm, const std::string &exe, uid_t uid, const std::string &cgroup = "0::/user.slice/app.slice/steam.scope", bool flatpak_info = false) {
      const auto entry = proc / std::to_string(pid);
      fs::create_directories(entry / "root");
      std::ofstream {entry / "comm"} << comm << '\n';
      std::ofstream {entry / "status"} << "Name:\t" << comm << "\nUid:\t" << uid << '\t' << uid << '\t' << uid << '\t' << uid << '\n';
      std::ofstream {entry / "cgroup"} << cgroup << '\n';
      fs::create_symlink(exe, entry / "exe");
      if (flatpak_info) {
        std::ofstream {entry / "root" / ".flatpak-info"} << "[Application]\nname=com.valvesoftware.Steam\n";
      }
    }

    fs::path pid_file(int pid) {
      const auto file = root / "steam.pid";
      std::ofstream {file} << pid;
      return file;
    }
  };
}  // namespace

TEST_F(FakeProc, TheNativePidFileIsUsedWhenItNamesARunningSteam) {
  process(4100, "steam", "/home/u/.local/share/Steam/ubuntu12_32/steam", 1000);
  const auto client = find(proc, 1000, pid_file(4100), "");
  ASSERT_TRUE(client);
  EXPECT_EQ(client->pid, 4100);
  EXPECT_FALSE(client->flatpak);
}

TEST_F(FakeProc, AFlatpakClientIsFoundThoughItsPidFileMeansNothingHere) {
  // The Flatpak's pid file holds the pid in its sandbox: 2 here, which is
  // some other process outside it, or none.
  process(2, "kthreadd", "/", 0);
  process(7300, "steam", "/home/u/.local/share/Steam/ubuntu12_32/steam", 1000, "0::/user.slice/user@1000.service/app.slice/app-flatpak-com.valvesoftware.Steam-123.scope");
  const auto client = find(proc, 1000, pid_file(2), "");
  ASSERT_TRUE(client);
  EXPECT_EQ(client->pid, 7300);
  EXPECT_TRUE(client->flatpak);
}

TEST_F(FakeProc, FlatpakInfoMarksAFlatpakWithoutItsScope) {
  process(7300, "steam", "/home/u/.local/share/Steam/ubuntu12_64/steam", 1000, "0::/user.slice/session-2.scope", true);
  const auto client = find(proc, 1000, "", "");
  ASSERT_TRUE(client);
  EXPECT_TRUE(client->flatpak);
}

TEST_F(FakeProc, TheLauncherScriptAndOtherUsersAreNotTheClient) {
  process(5000, "steam", "/usr/bin/bash", 1000);
  process(5001, "steam", "/home/v/.local/share/Steam/ubuntu12_32/steam", 1001);
  process(5002, "steamwebhelper", "/home/u/.local/share/Steam/ubuntu12_64/steamwebhelper", 1000);
  EXPECT_FALSE(find(proc, 1000, "", ""));
}

TEST_F(FakeProc, AStalePidFileFallsBackToTheScan) {
  process(4200, "bash", "/usr/bin/bash", 1000);
  process(4300, "steam", "/home/u/.local/share/Steam/ubuntu12_32/steam", 1000);
  const auto client = find(proc, 1000, pid_file(4200), "");
  ASSERT_TRUE(client);
  EXPECT_EQ(client->pid, 4300);
}

TEST_F(FakeProc, ASteamUnderHermesOwnServiceIsNotTheDesktops) {
  const std::string own = "0::/user.slice/user@1000.service/app.slice/hermes.service";
  process(4400, "steam", "/home/u/.local/share/Steam/ubuntu12_32/steam", 1000, own + "/session");
  EXPECT_FALSE(find(proc, 1000, pid_file(4400), own));
}

TEST_F(FakeProc, NoSteamNoClient) {
  EXPECT_FALSE(find(proc, 1000, root / "missing.pid", ""));
}
