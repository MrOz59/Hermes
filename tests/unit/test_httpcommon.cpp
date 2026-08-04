/**
 * @file tests/unit/test_httpcommon.cpp
 * @brief Test src/httpcommon.*.
 */
// test imports
#include "../tests_common.h"

// standard imports
#include <filesystem>
#include <fstream>

// lib imports
#include <curl/curl.h>
#include <nlohmann/json.hpp>

// local imports
#include <src/httpcommon.h>

struct UrlEscapeTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlEscapeTest, Run) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::url_escape(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlEscapeTests,
  UrlEscapeTest,
  testing::Values(
    std::make_tuple("igdb_0123456789", "igdb_0123456789"),
    std::make_tuple("../../../", "..%2F..%2F..%2F"),
    std::make_tuple("..*\\", "..%2A%5C")
  )
);

struct UrlGetHostTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlGetHostTest, Run) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::url_get_host(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlGetHostTests,
  UrlGetHostTest,
  testing::Values(
    std::make_tuple("https://images.igdb.com/example.txt", "images.igdb.com"),
    std::make_tuple("http://localhost:8080", "localhost"),
    std::make_tuple("nonsense!!}{::", "")
  )
);

struct DownloadFileTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(DownloadFileTest, Run) {
  const auto &[url, filename] = GetParam();
  const std::string test_dir = platf::appdata().string() + "/tests/";
  std::string path = test_dir + filename;
  ASSERT_TRUE(http::download_file(url, path, CURL_SSLVERSION_TLSv1_0));
}

#ifdef SUNSHINE_BUILD_FLATPAK
// requires running `npm run serve` prior to running the tests
constexpr const char *URL_1 = "http://0.0.0.0:3000/hello.txt";
constexpr const char *URL_2 = "http://0.0.0.0:3000/hello-redirect.txt";
#else
constexpr const char *URL_1 = "https://httpbin.org/base64/aGVsbG8h";
constexpr const char *URL_2 = "https://httpbin.org/redirect-to?url=/base64/aGVsbG8h";
#endif

INSTANTIATE_TEST_SUITE_P(
  DownloadFileTests,
  DownloadFileTest,
  testing::Values(
    std::make_tuple(URL_1, "hello.txt"),
    std::make_tuple(URL_2, "hello-redirect.txt")
  )
);

// --- credentials ------------------------------------------------------------
// Setting credentials is the documented way out of a lost or broken Web UI
// password, so it has to work even when the state file it writes into is the
// thing that is broken.
struct UserCredsTest: testing::Test {
  void SetUp() override {
    dir = std::filesystem::temp_directory_path() / "hermes-user-creds-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    file = (dir / "hermes_state.json").string();
  }

  void TearDown() override {
    std::filesystem::remove_all(dir);
  }

  void write(const std::string &contents) const {
    std::ofstream out(file);
    out << contents;
  }

  std::string read(const std::string &path) const {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char> {in}, std::istreambuf_iterator<char> {}};
  }

  std::filesystem::path dir;
  std::string file;
};

TEST_F(UserCredsTest, WritesCredentialsWhenNoStateFileExists) {
  ASSERT_EQ(http::save_user_creds(file, "ozzy", "hunter2"), 0);
  ASSERT_TRUE(http::user_creds_exist(file));

  const auto tree = nlohmann::json::parse(read(file));
  EXPECT_EQ(tree["username"], "ozzy");
  // The password is stored as a salted hash, never in the clear.
  EXPECT_NE(tree["password"], "hunter2");
  EXPECT_EQ(tree["salt"].get<std::string>().size(), 16u);
}

TEST_F(UserCredsTest, KeepsPairedClientsWhenChangingCredentials) {
  write(R"({"root":{"uniqueid":"abc","named_devices":["a-paired-client"]}})");
  ASSERT_EQ(http::save_user_creds(file, "ozzy", "hunter2"), 0);

  const auto tree = nlohmann::json::parse(read(file));
  EXPECT_EQ(tree["root"]["named_devices"][0], "a-paired-client");
  EXPECT_EQ(tree["username"], "ozzy");
}

TEST_F(UserCredsTest, RecoversWhenTheStateFileIsUnreadable) {
  // What a hand-edited state file typically looks like: no longer valid JSON.
  write(R"({"username": "ozzy", "password": my-new-password})");

  // The whole point: this must not fail just because the file is broken.
  ASSERT_EQ(http::save_user_creds(file, "ozzy", "hunter2"), 0);
  ASSERT_TRUE(http::user_creds_exist(file));
  EXPECT_EQ(http::reload_user_creds(file), 0);

  // The unreadable file is preserved, not destroyed — it is the user's only
  // route back to whatever paired clients it held.
  EXPECT_TRUE(std::filesystem::exists(file + ".unreadable"));
  EXPECT_NE(read(file + ".unreadable").find("my-new-password"), std::string::npos);
}
