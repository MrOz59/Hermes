/**
 * @file tests/unit/test_hestia_capabilities.cpp
 * @brief Check the served Hestia capabilities document against the schema
 *        this repository ships.
 *
 * `capabilities.schema.json` describes what a Hestia client may receive, and
 * both this repository and the client validate against it - but nothing here
 * read it, so the two drifted: the host began advertising a virtual display
 * backend the schema does not allow, every shipped client rejected the whole
 * document over the unknown value, and the virtual display it should have
 * asked for was never requested. The document is data, so the check is cheap;
 * it just has to exist.
 */
#include "../tests_common.h"

#include <src/config.h>
#include <src/confighttp.h>

#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

  /**
   * The JSON Schema keywords capabilities.schema.json actually uses, plus the
   * annotations that assert nothing. A schema that grows a keyword the
   * validator below does not implement is reported as a failure rather than
   * skipped: a check that quietly stops checking is worse than no check, and
   * that is exactly how the document drifted from the schema in the first
   * place.
   */
  const std::set<std::string> kImplementedKeywords {
    "$id",
    "$schema",
    "additionalProperties",
    "const",
    "description",
    "enum",
    "items",
    "minLength",
    "minimum",
    "properties",
    "required",
    "title",
    "type",
    "uniqueItems",
  };

  bool matches_type(const nlohmann::json &value, const std::string &type) {
    if (type == "object") {
      return value.is_object();
    }
    if (type == "array") {
      return value.is_array();
    }
    if (type == "string") {
      return value.is_string();
    }
    if (type == "boolean") {
      return value.is_boolean();
    }
    if (type == "integer") {
      return value.is_number_integer();
    }
    if (type == "number") {
      return value.is_number();
    }
    return false;
  }

  /**
   * Validate @p value against @p schema, appending one message per violation
   * to @p errors. Paths are JSON-pointer-ish so a failure names the field.
   */
  void validate(
    const nlohmann::json &schema,
    const nlohmann::json &value,
    const std::string &path,
    std::vector<std::string> &errors
  ) {
    const auto at = [&path]() {
      return path.empty() ? std::string {"(root)"} : path;
    };

    for (const auto &[keyword, _] : schema.items()) {
      if (!kImplementedKeywords.contains(keyword)) {
        errors.push_back(at() + ": schema uses '" + keyword + "', which this validator does not implement");
      }
    }

    if (schema.contains("type") && !matches_type(value, schema["type"].get<std::string>())) {
      errors.push_back(at() + ": expected type " + schema["type"].get<std::string>() + ", got " + std::string {value.type_name()});
      return;
    }

    if (schema.contains("const") && value != schema["const"]) {
      errors.push_back(at() + ": expected " + schema["const"].dump() + ", got " + value.dump());
    }

    if (schema.contains("enum")) {
      bool found = false;
      for (const auto &allowed : schema["enum"]) {
        found = found || value == allowed;
      }
      if (!found) {
        errors.push_back(at() + ": " + value.dump() + " is not one of " + schema["enum"].dump());
      }
    }

    if (schema.contains("minimum") && value.is_number() && value.get<double>() < schema["minimum"].get<double>()) {
      errors.push_back(at() + ": " + value.dump() + " is below the minimum " + schema["minimum"].dump());
    }

    if (schema.contains("minLength") && value.is_string() &&
        value.get<std::string>().size() < schema["minLength"].get<std::size_t>()) {
      errors.push_back(at() + ": shorter than the minimum length " + schema["minLength"].dump());
    }

    if (value.is_object()) {
      const auto &properties = schema.contains("properties") ? schema["properties"] : nlohmann::json::object();

      if (schema.contains("required")) {
        for (const auto &name : schema["required"]) {
          if (!value.contains(name.get<std::string>())) {
            errors.push_back(at() + ": missing required field '" + name.get<std::string>() + "'");
          }
        }
      }

      for (const auto &[name, member] : value.items()) {
        if (properties.contains(name)) {
          validate(properties[name], member, path + '/' + name, errors);
        } else if (schema.value("additionalProperties", true) == false) {
          errors.push_back(at() + ": '" + name + "' is not allowed by the schema");
        }
      }
    }

    if (value.is_array()) {
      if (schema.contains("items")) {
        for (std::size_t i = 0; i < value.size(); ++i) {
          validate(schema["items"], value[i], path + '[' + std::to_string(i) + ']', errors);
        }
      }
      if (schema.value("uniqueItems", false)) {
        for (std::size_t i = 0; i < value.size(); ++i) {
          for (std::size_t j = i + 1; j < value.size(); ++j) {
            if (value[i] == value[j]) {
              errors.push_back(at() + ": " + value[i].dump() + " appears more than once");
            }
          }
        }
      }
    }
  }

  nlohmann::json load_schema() {
    const std::string path = std::string {SUNSHINE_SOURCE_DIR} + "/capabilities.schema.json";
    std::ifstream file {path};
    if (!file) {
      return {};
    }
    return nlohmann::json::parse(file, nullptr, false);
  }

}  // namespace

// The document reports on host state - whether gamescope is installed, whether
// a clipboard tool is available - so the platform has to be up.
struct HestiaCapabilitiesTest: PlatformTestSuite {};

TEST_F(HestiaCapabilitiesTest, MatchesTheSchemaThisRepositoryShips) {
  const auto schema = load_schema();
  ASSERT_FALSE(schema.is_null()) << "capabilities.schema.json is missing or unparseable";
  ASSERT_TRUE(schema.is_object());

  std::vector<std::string> errors;
  validate(schema, confighttp::hestia_capabilities_json(), "", errors);

  std::string report;
  for (const auto &error : errors) {
    report += "\n  " + error;
  }
  EXPECT_TRUE(errors.empty())
    << "The served capabilities document does not satisfy capabilities.schema.json."
    << "\nA client that validates strictly rejects the whole document, so it falls back to a"
    << "\nhost without Hestia extensions. Fix the document, or the schema if the field is real:"
    << report;
}

TEST_F(HestiaCapabilitiesTest, AdvertisesOnlyBackendsAClientCanBeServedBy) {
  // The regression this file exists for: "none" is a valid value of the
  // virtual_display_backend *setting*, and was advertised as though it were a
  // backend a client could ask for. It is neither in the schema nor selectable.
  const auto backends = confighttp::hestia_capabilities_json()["features"]["virtual_display_backend"];
  ASSERT_TRUE(backends.is_array());
  for (const auto &backend : backends) {
    EXPECT_TRUE(backend == "evdi" || backend == "hermes_kms")
      << backend.dump() << " is advertised as a virtual display backend but no client can select it";
  }
}

TEST(HestiaCapabilitiesSchema, IsValidatedByEveryKeywordItUses) {
  // Guards the guard: if the schema starts using a keyword the validator does
  // not implement, the document above would pass on a check that silently
  // stopped covering that field.
  const auto schema = load_schema();
  ASSERT_FALSE(schema.is_null());

  std::vector<std::string> unknown;
  const auto walk = [&unknown](const nlohmann::json &node, const auto &self) -> void {
    if (node.is_object()) {
      for (const auto &[keyword, child] : node.items()) {
        if (!kImplementedKeywords.contains(keyword)) {
          unknown.push_back(keyword);
        }
        // Only recurse where a subschema can live; a "required" list or an
        // "enum" of values holds data, not keywords.
        if (keyword == "properties" || keyword == "items" || keyword == "additionalProperties") {
          if (keyword == "properties") {
            for (const auto &[_, subschema] : child.items()) {
              self(subschema, self);
            }
          } else {
            self(child, self);
          }
        }
      }
    }
  };
  walk(schema, walk);

  std::string report;
  for (const auto &keyword : unknown) {
    report += " " + keyword;
  }
  EXPECT_TRUE(unknown.empty())
    << "capabilities.schema.json uses keywords the validator in this file does not implement:"
    << report << "\nImplement them in kImplementedKeywords/validate(), or the schema check is a no-op for them.";
}
