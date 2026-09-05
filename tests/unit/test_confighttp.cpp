/**
 * @file tests/unit/test_confighttp.cpp
 * @brief Test src/confighttp.* helpers.
 */
#include "../tests_common.h"

#include <src/config.h>
#include <src/confighttp.h>

#include <unordered_map>

TEST(GameModeLocalTokenOriginTest, AllowsOnlyPcAndLanAddresses) {
  EXPECT_TRUE(confighttp::local_api_token_origin_allowed("127.0.0.1"));
  EXPECT_TRUE(confighttp::local_api_token_origin_allowed("::1"));
  EXPECT_TRUE(confighttp::local_api_token_origin_allowed("192.168.1.25"));
  EXPECT_TRUE(confighttp::local_api_token_origin_allowed("10.23.45.67"));
  EXPECT_TRUE(confighttp::local_api_token_origin_allowed("fd12:3456:789a::1"));
  EXPECT_TRUE(confighttp::local_api_token_origin_allowed("fe80::1"));

  EXPECT_FALSE(confighttp::local_api_token_origin_allowed("8.8.8.8"));
  EXPECT_FALSE(confighttp::local_api_token_origin_allowed("2001:4860:4860::8888"));
  EXPECT_FALSE(confighttp::local_api_token_origin_allowed("not-an-address"));
}

// The preflight aggregation reads platform-derived state (encoder probe result,
// virtual-display backend, session environment), so initialize the platform.
struct PreflightTest: PlatformTestSuite {};

TEST_F(PreflightTest, ShapeIsWellFormed) {
  const auto preflight = confighttp::hestia_preflight_json();

  ASSERT_TRUE(preflight.contains("ready"));
  ASSERT_TRUE(preflight["ready"].is_boolean());

  ASSERT_TRUE(preflight.contains("checks"));
  ASSERT_TRUE(preflight["checks"].is_array());
  ASSERT_FALSE(preflight["checks"].empty());

  bool any_fail = false;
  for (const auto &check : preflight["checks"]) {
    ASSERT_TRUE(check.contains("id"));
    ASSERT_TRUE(check["id"].is_string());
    ASSERT_FALSE(check["id"].get<std::string>().empty());

    ASSERT_TRUE(check.contains("status"));
    const auto status = check.value("status", std::string {});
    ASSERT_TRUE(status == "ok" || status == "warn" || status == "fail")
      << "unexpected status: " << status;
    if (status == "fail") {
      any_fail = true;
    }

    ASSERT_TRUE(check.contains("message"));
    ASSERT_TRUE(check["message"].is_string());
    ASSERT_FALSE(check["message"].get<std::string>().empty());
  }

  // `ready` must be the negation of "any check failed".
  ASSERT_EQ(preflight["ready"].get<bool>(), !any_fail);
}

// The config response reports host state (virtual-display readiness, whether
// the streaming ports are free), so the platform has to be up.
struct ConfigResponseTest: PlatformTestSuite {};

TEST_F(ConfigResponseTest, AConfigFileCannotReplaceAComputedField) {
  // The reported bug: saveConfig() persists whatever the Web UI posts back, and
  // the config page posts the whole response - so streamPorts ended up in
  // hermes.conf. The merge then overwrote the array with the string the config
  // parser produces, `"[]"` passed the `|| []` guard because it is truthy, and
  // the home page died calling .filter on it.
  auto response = confighttp::computed_config_json();
  ASSERT_TRUE(response["streamPorts"].is_array());

  const std::unordered_map<std::string, std::string> file_values {
    {"streamPorts", "[]"},
    {"version", "not-a-version"},
    {"virtual_display_backend", "hermes_kms"},
  };
  confighttp::overlay_config_file(response, file_values);

  EXPECT_TRUE(response["streamPorts"].is_array()) << "a config file replaced a structured field with a string";
  EXPECT_EQ(response["version"], PROJECT_VERSION);
  // A real setting still comes from the file, which is the whole point of the merge.
  EXPECT_EQ(response["virtual_display_backend"], "hermes_kms");
}

TEST_F(ConfigResponseTest, EveryComputedFieldIsRegisteredAsOne) {
  // Two places have to agree about which keys are server-computed: the config
  // parser, which would otherwise warn about each as an unrecognized option,
  // and the merge above, which must not let a file overwrite one. A field added
  // to the response and forgotten here is the shape the bug had.
  for (const auto &[name, _] : confighttp::computed_config_json().items()) {
    EXPECT_TRUE(config::server_computed_keys().contains(name))
      << '\'' << name << "' is computed for the config response but is not in "
      << "config::server_computed_keys(), so a hermes.conf key of that name would "
      << "overwrite it and the config parser would report it as unrecognized";
  }
}
