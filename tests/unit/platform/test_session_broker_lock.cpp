// SPDX-License-Identifier: GPL-3.0-only
/** @file test_session_broker_lock.cpp
 *  @brief Exercise broker mutation locking without changing accounts or devices.
 */
#ifdef __linux__
  #include "tools/session-broker-lock.h"

  #include <filesystem>
  #include <fstream>
  #include <gtest/gtest.h>
  #include <string>

namespace {
  class SessionBrokerLockTest: public testing::Test {
  protected:
    void SetUp() override {
      std::string pattern = (std::filesystem::temp_directory_path() / "hermes-broker-lock-XXXXXX").string();
      const auto *directory = ::mkdtemp(pattern.data());
      ASSERT_NE(directory, nullptr);
      root = directory;
      path = (root / "mutation.lock").string();
    }

    void TearDown() override {
      std::filesystem::remove_all(root);
    }

    std::filesystem::path root;
    std::string path;
  };

  TEST_F(SessionBrokerLockTest, SerializesIndependentDescriptorsAndReleasesOnDestruction) {
    {
      hermes::session_broker::mutation_lock first {path.c_str()};
      ASSERT_TRUE(first);
      hermes::session_broker::mutation_lock competing {path.c_str()};
      EXPECT_FALSE(competing);
      EXPECT_TRUE(competing.contended());
    }
    hermes::session_broker::mutation_lock retry {path.c_str()};
    EXPECT_TRUE(retry);
    EXPECT_FALSE(retry.contended());

    struct stat info {};

    ASSERT_EQ(::stat(path.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777U, 0600U);
  }

  TEST_F(SessionBrokerLockTest, RefusesMissingParentAndDirectory) {
    hermes::session_broker::mutation_lock missing {(root / "missing" / "lock").c_str()};
    EXPECT_FALSE(missing);
    EXPECT_FALSE(missing.contended());
    hermes::session_broker::mutation_lock directory {root.c_str()};
    EXPECT_FALSE(directory);
  }

  TEST_F(SessionBrokerLockTest, RefusesSymlinksWithoutChangingTheTarget) {
    const auto target = root / "target";
    std::ofstream {target} << "untouched";
    std::filesystem::create_symlink(target, path);
    hermes::session_broker::mutation_lock lock {path.c_str()};
    EXPECT_FALSE(lock);
    std::ifstream file {target};
    std::string contents;
    std::getline(file, contents);
    EXPECT_EQ(contents, "untouched");
  }

  TEST_F(SessionBrokerLockTest, RefusesHardLinksAndWritableGroupOrOther) {
    const auto target = root / "target";
    std::ofstream {target};
    std::filesystem::create_hard_link(target, path);
    {
      hermes::session_broker::mutation_lock hardlink {path.c_str()};
      EXPECT_FALSE(hardlink);
    }
    std::filesystem::remove(path);
    std::filesystem::rename(target, path);
    for (const mode_t mode : {0620, 0602, 0666}) {
      ASSERT_EQ(::chmod(path.c_str(), mode), 0);
      hermes::session_broker::mutation_lock unsafe {path.c_str()};
      EXPECT_FALSE(unsafe);
      EXPECT_FALSE(unsafe.contended());
    }
  }

  TEST_F(SessionBrokerLockTest, RefusesFifoWithoutBlocking) {
    ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);
    hermes::session_broker::mutation_lock fifo {path.c_str()};
    EXPECT_FALSE(fifo);
  }
}  // namespace
#endif
