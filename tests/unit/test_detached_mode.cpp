// SPDX-License-Identifier: GPL-3.0-only
#include "src/platform/linux/detached_mode.h"

#include <functional>
#include <gtest/gtest.h>
#include <utility>

namespace {
  using nlohmann::json;
  namespace dm = platf::kwin::detached;

  void require(bool value, const char *message) {
    if (!value) {
      throw std::runtime_error(message);
    }
  }

  json mode(std::string id, int width, int height, double hz) {
    return {{"id", id}, {"size", {{"width", width}, {"height", height}}}, {"refreshRate", hz}};
  }

  json output(std::string current, bool high = false, double hz = 120) {
    auto modes = json::array({mode("1", 1920, 1080, 60)});
    if (high) {
      modes.push_back(mode("2", 1920, 1080, hz));
    }
    return {{"outputs", json::array({{{"id", 1}, {"name", "Virtual-0"}, {"enabled", true}, {"currentModeId", current}, {"modes", modes}}})}};
  }

  struct script_t {
    std::vector<std::pair<std::string, std::string>> steps;
    size_t next = 0;

    std::string run(const std::string &command) {
      require(next < steps.size(), "Unexpected extra command");
      const auto &[expected, reply] = steps[next++];
      require(command == expected, "Unexpected command or mode identifier");
      return reply;
    }

    dm::mode_t apply(dm::mode_t requested = {1920, 1080, 120000}) {
      return dm::configure(requested, [&](const auto &command) {
        return run(command);
      });
    }

    void complete() {
      require(next == steps.size(), "Expected command was not executed");
    }
  };

  void rejects(const std::function<void()> &action, const std::string &message) {
    try {
      action();
    } catch (const std::exception &error) {
      require(std::string(error.what()).find(message) != std::string::npos, "Wrong rejection reason");
      return;
    }
    throw std::runtime_error("Expected rejection");
  }

  const auto low = output("1").dump();
  const auto both = output("1", true).dump();
  const auto high = output("2", true).dump();

  TEST(DetachedMode, Mode60HzRequiresNoMutation) {
    script_t s {{{"-j", low}}};
    require(s.apply({1920, 1080, 60000}).refresh_mhz == 60000, "Wrong result");
    s.complete();
  }

  TEST(DetachedMode, Existing120HzModeIsSelected) {
    script_t s {{{"-j", both}, {"output.1.mode.2", ""}, {"-j", high}}};
    require(s.apply().refresh_mhz == 120000, "Wrong result");
    s.complete();
  }

  TEST(DetachedMode, CustomModeIsAdvertisedThenSelectedAndVerified) {
    script_t s {{{"-j", low}, {"output.1.addCustomMode.1920.1080.120000.full", ""}, {"-j", both}, {"output.1.mode.2", ""}, {"-j", high}}};
    s.apply();
    s.complete();
  }

  TEST(DetachedMode, AlreadyActiveCustomModeNeedsNoSecondMutation) {
    script_t s {{{"-j", low}, {"output.1.addCustomMode.1920.1080.120000.full", ""}, {"-j", high}}};
    s.apply();
    s.complete();
  }

  TEST(DetachedMode, CVTRoundingIsAccepted) {
    script_t s {{{"-j", output("2", true, 119.98).dump()}}};
    require(s.apply().refresh_mhz == 119980, "Actual rounded mode must be returned");
    s.complete();
  }

  TEST(DetachedMode, ResumeCanReturnTo60HzWithoutAddingAMode) {
    script_t s {{{"-j", high}, {"output.1.mode.1", ""}, {"-j", both}}};
    s.apply({1920, 1080, 60000});
    s.complete();
  }

  TEST(DetachedMode, ResolutionChangeUsesTheSameVerifiedPath) {
    auto before = output("1");
    before["outputs"][0]["modes"].push_back(mode("3", 2560, 1440, 120));
    auto after = before;
    after["outputs"][0]["currentModeId"] = "3";
    script_t s {{{"-j", before.dump()}, {"output.1.mode.3", ""}, {"-j", after.dump()}}};
    require(s.apply({2560, 1440, 120000}).width == 2560, "Wrong width");
    s.complete();
  }

  TEST(DetachedMode, SuccessfulCommandWithoutActualActivationIsRejected) {
    script_t s {{{"-j", both}, {"output.1.mode.2", ""}, {"-j", both}}};
    rejects([&] {
      s.apply();
    },
            "did not activate");
    s.complete();
  }

  TEST(DetachedMode, UnsupportedCustomModesProduceASpecificFailure) {
    script_t s {{{"-j", low}, {"output.1.addCustomMode.1920.1080.120000.full", ""}, {"-j", low}}};
    rejects([&] {
      s.apply();
    },
            "did not advertise");
    s.complete();
  }

  TEST(DetachedMode, MultipleOutputsAreRejectedBeforeAMutation) {
    auto data = output("1");
    data["outputs"].push_back(data["outputs"][0]);
    script_t s {{{"-j", data.dump()}}};
    rejects([&] {
      s.apply();
    },
            "exactly one output");
    s.complete();
  }

  TEST(DetachedMode, PhysicalOutputIsRejectedBeforeAMutation) {
    auto data = output("1");
    data["outputs"][0]["name"] = "DP-2";
    script_t s {{{"-j", data.dump()}}};
    rejects([&] {
      s.apply();
    },
            "unexpected private output");
    s.complete();
  }

  TEST(DetachedMode, DisabledOutputIsRejected) {
    auto data = output("1");
    data["outputs"][0]["enabled"] = false;
    rejects([&] {
      dm::parse_output(data.dump());
    },
            "unexpected private output");
  }

  TEST(DetachedMode, OutputReplacementDuringApplicationIsRejected) {
    auto replaced = output("2", true);
    replaced["outputs"][0]["id"] = 2;
    script_t s {{{"-j", both}, {"output.1.mode.2", ""}, {"-j", replaced.dump()}}};
    rejects([&] {
      s.apply();
    },
            "changed during");
    s.complete();
  }

  TEST(DetachedMode, MissingCurrentModeIsRejected) {
    rejects([&] {
      dm::parse_output(output("missing").dump());
    },
            "no advertised current mode");
  }

  TEST(DetachedMode, InvalidModeIdentifiersAreRejected) {
    auto data = output("1");
    data["outputs"][0]["modes"][0]["id"] = "1.mode.2";
    rejects([&] {
      dm::parse_output(data.dump());
    },
            "mode identifier");
  }

  TEST(DetachedMode, InvalidRequestIsRejectedWithoutExecutingCommands) {
    script_t s {};
    rejects([&] {
      s.apply({0, 1080, 120000});
    },
            "Invalid private output mode");
    rejects([&] {
      s.apply({1920, 1080, 120});
    },
            "Invalid private output mode");
    s.complete();
  }

  TEST(DetachedMode, MalformedJSONIsRejected) {
    rejects([&] {
      dm::parse_output("not JSON");
    },
            "parse_error");
  }

  TEST(DetachedMode, CommandFailurePropagatesWithoutRetryingAnotherDisplay) {
    rejects([&] {
      dm::configure({1920, 1080, 120000}, [](const std::string &) -> std::string {
        throw std::runtime_error("command timeout");
      });
    },
            "command timeout");
  }

}  // namespace
