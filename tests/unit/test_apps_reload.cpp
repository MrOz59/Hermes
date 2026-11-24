/**
 * @file tests/unit/test_apps_reload.cpp
 * @brief A failed apps.json load keeps the current list and leaves the file alone.
 */
#include "../tests_common.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <src/process.h>
#include <string>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;

  class AppsReload: public ::testing::Test {
  protected:
    fs::path file = fs::temp_directory_path() / ("hermes-apps-reload-" + std::to_string(::getpid()) + ".json");

    void TearDown() override {
      fs::remove(file);
    }

    void write(const std::string &content) {
      std::ofstream {file} << content;
    }

    std::string read() {
      std::ifstream in {file};
      return {std::istreambuf_iterator<char> {in}, {}};
    }
  };

  // Current format, but "uuid" must be a string: loading it throws, which
  // used to send parse() into a from-scratch re-migration that rewrote the file.
  const std::string bad_entry = R"({"version": 7, "env": {}, "apps": [{"name": "Broken", "uuid": 42}]})";
}  // namespace

TEST_F(AppsReload, AReloadThatFailsReturnsNothingToReplaceTheList) {
  write(bad_entry);
  EXPECT_FALSE(proc::parse(file.string(), false));
}

TEST_F(AppsReload, AFailedLoadDoesNotRewriteTheFile) {
  write(bad_entry);
  proc::parse(file.string(), false);
  EXPECT_EQ(read(), bad_entry);
}

TEST_F(AppsReload, UnreadableJsonOnReloadReturnsNothing) {
  write("{ not json");
  EXPECT_FALSE(proc::parse(file.string(), false));
}

TEST_F(AppsReload, AtStartupAFailedLoadStillGivesTheFallbackDesktop) {
  write(bad_entry);
  const auto loaded = proc::parse(file.string(), true);
  ASSERT_TRUE(loaded);
  bool has_fallback = false;
  for (const auto &app : loaded->get_apps()) {
    has_fallback |= app.name == "Desktop (fallback)";
  }
  EXPECT_TRUE(has_fallback);
}

TEST_F(AppsReload, AnEmptyListIsALoadNotAFailure) {
  write(R"({"version": 7, "env": {}, "apps": []})");
  EXPECT_TRUE(proc::parse(file.string(), false));
}
