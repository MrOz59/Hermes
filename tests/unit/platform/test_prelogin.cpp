/**
 * @file tests/unit/platform/test_prelogin.cpp
 * @brief Test the setup and the checks for reaching a host before login.
 *
 * `hermes --prelogin enable` rewrites the unit Hermes starts from, and the
 * checks decide whether a host is told it will be reachable after a reboot
 * with nobody at it. A wrong unit keeps Hermes from starting; a wrong "ready"
 * is found out with the machine already out of reach.
 */
#include "../../tests_common.h"

#include <src/platform/linux/prelogin.h>

#include <algorithm>
#include <string>

using namespace platf::prelogin;

namespace {

  /** The unit the Arch package installs, comments and all. */
  constexpr const char *packaged_unit = R"([Unit]
Description=Self-hosted game stream host for Hestia and Artemis
StartLimitIntervalSec=500
StartLimitBurst=5
# Start only once the graphical session is up, and stop when it goes away.
After=graphical-session.target
PartOf=graphical-session.target
Wants=graphical-session.target

[Service]
# Give the compositor a moment to finish coming up before we launch.
ExecStartPre=/bin/sleep 5
ExecStart=/usr/bin/hermes

ExecStopPost=-/usr/bin/hermes-monitor-recovery
Restart=on-failure
RestartSec=5s

[Install]
# Two targets, because no single one is reached by every session type.
WantedBy=graphical-session.target xdg-desktop-autostart.target
)";

  bool has_line(const std::string &text, const std::string &line) {
    return ("\n" + text + "\n").find("\n" + line + "\n") != std::string::npos;
  }

  /** A host with everything in place. */
  facts_t ready_host() {
    facts_t facts;
    facts.user = "ana";
    facts.unit_name = "hermes.service";
    facts.unit_from_boot = true;
    facts.unit_generated = true;
    facts.unit_enabled = true;
    facts.linger = true;
    facts.display_manager = "sddm";
    facts.sddm = sddm_settings_t {"wayland", ""};
    facts.input_without_session = true;
    facts.virtual_display_backend = "hermes_kms";
    facts.hermes_kms_without_session = true;
    return facts;
  }

  const check_t &check(const report_t &report, const std::string &id) {
    const auto it = std::find_if(report.checks.begin(), report.checks.end(), [&id](const check_t &candidate) {
      return candidate.id == id;
    });
    EXPECT_NE(it, report.checks.end()) << "no check named " << id;
    return *it;
  }

}  // namespace

TEST(PreloginUnit, TheServiceIsKeptAsPackaged) {
  const auto unit = from_boot_unit(packaged_unit, "/usr/lib/systemd/user/hermes.service");

  EXPECT_TRUE(has_line(unit, "ExecStartPre=/bin/sleep 5"));
  EXPECT_TRUE(has_line(unit, "ExecStart=/usr/bin/hermes"));
  EXPECT_TRUE(has_line(unit, "ExecStopPost=-/usr/bin/hermes-monitor-recovery"));
  EXPECT_TRUE(has_line(unit, "Restart=on-failure"));
  EXPECT_TRUE(has_line(unit, "StartLimitBurst=5"));
}

TEST(PreloginUnit, NothingTiesItToTheGraphicalSession) {
  // Wants=graphical-session.target pulled in at boot would mark a graphical
  // session as running with none behind it.
  const auto unit = from_boot_unit(packaged_unit, "/usr/lib/systemd/user/hermes.service");

  EXPECT_EQ(unit.find("graphical-session"), std::string::npos);
  EXPECT_EQ(unit.find("xdg-desktop-autostart"), std::string::npos);
  EXPECT_EQ(unit.find("PartOf="), std::string::npos);
}

TEST(PreloginUnit, ItStartsWithTheUserManager) {
  const auto unit = from_boot_unit(packaged_unit, "/usr/lib/systemd/user/hermes.service");

  EXPECT_TRUE(has_line(unit, "[Install]"));
  EXPECT_TRUE(has_line(unit, "WantedBy=default.target"));
  EXPECT_EQ(unit.find("WantedBy=", unit.find("WantedBy=default.target") + 1), std::string::npos);
}

TEST(PreloginUnit, OtherDependenciesOnTheSameLineSurvive) {
  const auto unit = from_boot_unit(
    "[Unit]\nAfter=network-online.target graphical-session.target pipewire.service\nWants=graphical-session.target\n"
    "[Service]\nExecStart=/usr/bin/hermes\n",
    "/x"
  );

  EXPECT_TRUE(has_line(unit, "After=network-online.target pipewire.service"));
  EXPECT_EQ(unit.find("Wants="), std::string::npos);
}

TEST(PreloginUnit, AUnitWithNoRestartPolicyGetsOne) {
  // Hermes leaves with the compositor at logout and has to come back for the
  // login screen.
  const auto unit = from_boot_unit("[Unit]\nDescription=x\n[Service]\nExecStart=/usr/bin/hermes\n", "/x");

  EXPECT_TRUE(has_line(unit, "Restart=on-failure"));
  EXPECT_TRUE(has_line(unit, "WantedBy=default.target"));
  EXPECT_LT(unit.find("Restart=on-failure"), unit.find("[Install]"));
}

TEST(PreloginUnit, TheFlatpakStartAndStopCommandsCarryOver) {
  const auto unit = from_boot_unit(
    "[Unit]\nPartOf=graphical-session.target\n[Service]\n"
    "ExecStart=flatpak run --command=sunshine dev.lizardbyte.app.Sunshine\n"
    "ExecStop=flatpak kill dev.lizardbyte.app.Sunshine\nRestart=on-failure\n"
    "[Install]\nWantedBy=xdg-desktop-autostart.target\n",
    "/x"
  );

  EXPECT_TRUE(has_line(unit, "ExecStart=flatpak run --command=sunshine dev.lizardbyte.app.Sunshine"));
  EXPECT_TRUE(has_line(unit, "ExecStop=flatpak kill dev.lizardbyte.app.Sunshine"));
}

TEST(PreloginUnit, WhatItWroteIsRecognisedAndWhatSomeoneElseWroteIsNot) {
  EXPECT_TRUE(is_generated_unit(from_boot_unit(packaged_unit, "/usr/lib/systemd/user/hermes.service")));
  EXPECT_FALSE(is_generated_unit(packaged_unit));
  EXPECT_FALSE(is_generated_unit("# my own override\n[Service]\nExecStart=/home/me/hermes\n"));
}

TEST(PreloginUnit, GeneratingFromItsOwnOutputChangesNothingButTheHeader) {
  const auto once = from_boot_unit(packaged_unit, "/a");
  const auto twice = from_boot_unit(once, "/a");

  EXPECT_EQ(once, twice);
}

TEST(PreloginUnit, AUnitIsJudgedByWhatItWaitsForNotByWhoWroteIt) {
  EXPECT_TRUE(unit_waits_for_session(packaged_unit));
  EXPECT_FALSE(unit_waits_for_session(from_boot_unit(packaged_unit, "/x")));
  // Someone's own override that starts at boot is as good as the generated one.
  EXPECT_FALSE(unit_waits_for_session("[Service]\nExecStart=/home/me/hermes\n[Install]\nWantedBy=default.target\n"));
  // A target named only in a comment is not a dependency.
  EXPECT_FALSE(unit_waits_for_session("# no longer After=graphical-session.target\n[Service]\nExecStart=/usr/bin/hermes\n"));
  EXPECT_TRUE(unit_waits_for_session("[Install]\nWantedBy=xdg-desktop-autostart.target\n"));
}

TEST(PreloginChecks, AUnitOfTheUsersOwnThatStartsAtBootCounts) {
  auto facts = ready_host();
  facts.unit_generated = false;

  const auto report = assess(facts);

  EXPECT_TRUE(report.enabled);
  EXPECT_TRUE(report.ready);
  EXPECT_NE(check(report, "unit").detail.find("of your own"), std::string::npos);
}

TEST(PreloginChecks, AnOverrideThatStillWaitsForTheSessionDoesNot) {
  auto facts = ready_host();
  facts.unit_from_boot = false;
  facts.unit_generated = false;

  EXPECT_FALSE(assess(facts).enabled);
  EXPECT_EQ(check(assess(facts), "unit").state, state_e::missing);
}

TEST(SddmSettings, TheGreeterIsX11UnlessSomethingSaysOtherwise) {
  EXPECT_EQ(sddm_settings({"[Theme]\nCurrent=breeze\n"}).display_server, "");
}

TEST(SddmSettings, ALaterFileHasTheLastWord) {
  // This is the machine the feature was developed on: the directory says
  // wayland, and /etc/sddm.conf, read last, says x11.
  const auto settings = sddm_settings({
    "[General]\nDisplayServer=wayland\n",
    "[General]\nDisplayServer=x11\nNumlock=on\n",
  });

  EXPECT_EQ(settings.display_server, "x11");
}

TEST(SddmSettings, SectionsAndKeysAreReadWhateverTheirCase) {
  const auto settings = sddm_settings({"# a comment\n[general]\n displayserver = Wayland \n"});

  EXPECT_EQ(settings.display_server, "wayland");
}

TEST(SddmSettings, AKeyOnlyCountsInItsOwnSection) {
  const auto settings = sddm_settings({"[X11]\nDisplayServer=wayland\n[Users]\nUser=ana\n"});

  EXPECT_EQ(settings.display_server, "");
  EXPECT_EQ(settings.autologin_user, "");
}

TEST(SddmSettings, AutologinIsOnWheneverAUserIsNamed) {
  // SDDM has no Enable= key: clearing User= is what turns autologin off.
  EXPECT_EQ(sddm_settings({"[Autologin]\nEnable=false\nUser=ana\nSession=plasma\n"}).autologin_user, "ana");
  EXPECT_EQ(sddm_settings({"[Autologin]\nUser=ana\n", "[Autologin]\nUser=\n"}).autologin_user, "");
}

TEST(PreloginChecks, AHostWithEverythingInPlaceIsReady) {
  const auto report = assess(ready_host());

  EXPECT_TRUE(report.enabled);
  EXPECT_TRUE(report.ready);
  for (const auto &entry : report.checks) {
    EXPECT_EQ(entry.state, state_e::ready) << entry.id;
    EXPECT_TRUE(entry.fix.empty()) << entry.id;
  }
}

TEST(PreloginChecks, AFreshInstallIsNotEnabledAndSaysHowToBe) {
  auto facts = ready_host();
  facts.unit_from_boot = false;
  facts.unit_generated = false;
  facts.unit_enabled = false;
  facts.linger = false;

  const auto report = assess(facts);

  EXPECT_FALSE(report.enabled);
  EXPECT_FALSE(report.ready);
  EXPECT_EQ(check(report, "unit").state, state_e::missing);
  EXPECT_NE(check(report, "unit").fix.find("hermes --prelogin enable"), std::string::npos);
  EXPECT_EQ(check(report, "linger").state, state_e::missing);
}

TEST(PreloginChecks, TheX11GreeterIsNamedAsTheObstacle) {
  auto facts = ready_host();
  facts.sddm = sddm_settings_t {"x11", ""};
  EXPECT_FALSE(assess(facts).ready);
  EXPECT_EQ(check(assess(facts), "greeter").state, state_e::missing);

  // Unset is X11 too: that is SDDM's default.
  facts.sddm = sddm_settings_t {"", ""};
  EXPECT_EQ(check(assess(facts), "greeter").state, state_e::missing);
}

TEST(PreloginChecks, AGreeterNobodyHasRunIsNotCalledReady) {
  auto facts = ready_host();
  facts.display_manager = "gdm";
  facts.sddm.reset();

  const auto report = assess(facts);

  EXPECT_FALSE(report.ready);
  EXPECT_EQ(check(report, "greeter").state, state_e::unknown);
}

TEST(PreloginChecks, AutologinIsANoteAndNotAnObstacle) {
  auto facts = ready_host();
  facts.sddm = sddm_settings_t {"wayland", "ana"};

  const auto report = assess(facts);

  EXPECT_TRUE(report.ready);
  EXPECT_EQ(check(report, "autologin").state, state_e::note);
}

TEST(PreloginChecks, DevicesTheSeatGrantCoversAreNotEnough) {
  auto facts = ready_host();
  facts.input_without_session = false;
  facts.hermes_kms_without_session = false;

  const auto report = assess(facts);

  EXPECT_FALSE(report.ready);
  EXPECT_NE(check(report, "input").fix.find("usermod -aG input ana"), std::string::npos);
  EXPECT_NE(check(report, "virtual_display").fix.find("hermes-kms-setup"), std::string::npos);
}

TEST(PreloginChecks, OnlyHermesKmsHasBeenRunBeforeLogin) {
  auto facts = ready_host();
  facts.virtual_display_backend = "evdi";

  EXPECT_FALSE(assess(facts).ready);
  EXPECT_EQ(check(assess(facts), "virtual_display").state, state_e::missing);
}

TEST(PreloginChecks, WhatCouldNotBeReadIsNotCalledReady) {
  auto facts = ready_host();
  facts.linger.reset();

  const auto report = assess(facts);

  EXPECT_FALSE(report.ready);
  EXPECT_EQ(check(report, "linger").state, state_e::unknown);
}
