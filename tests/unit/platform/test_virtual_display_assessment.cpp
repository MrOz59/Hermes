/**
 * @file tests/unit/platform/test_virtual_display_assessment.cpp
 * @brief Test the per-feature session assessment in src/platform/linux/virtual_display.*.
 *
 * Hermes' advice to a user is this function's output. Every session shape it
 * has to advise - COSMIC without wlr-screencopy, Hyprland with a virtual
 * display it cannot composite onto, an isolation setup whose DRM half was never
 * installed - is a machine somebody would otherwise have to own to check.
 * Keeping the rules pure makes each of them a row here instead.
 */
#include "../../tests_common.h"

#include <src/platform/common.h>
#include <src/platform/linux/virtual_display.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

  /** The report for one feature, so a test names the feature it is about. */
  VDISPLAY::FeatureReport reportFor(const VDISPLAY::SessionFacts &facts, VDISPLAY::feature_e feature) {
    const auto reports = VDISPLAY::assessSession(facts);
    const auto it = std::find_if(reports.begin(), reports.end(), [&](const auto &report) {
      return report.feature == feature;
    });
    EXPECT_NE(it, reports.end()) << "no report for " << VDISPLAY::featureName(feature);
    return it == reports.end() ? VDISPLAY::FeatureReport {} : *it;
  }

  /** A KDE session with everything Hermes wants, as the baseline to vary from. */
  VDISPLAY::SessionFacts workingKwinSession() {
    VDISPLAY::SessionFacts facts;
    facts.compositor = VDISPLAY::compositor_e::kwin;
    facts.wayland = true;
    facts.x11 = false;
    facts.output_management = false;  // KWin drives kscreen-doctor, not wlr-output-management.
    facts.screencopy = true;
    facts.linux_dmabuf = true;
    facts.kscreen = true;
    facts.hermes_kms_present = true;
    facts.hermes_kms_multi_output = true;
    facts.hermes_kms_multi_device = true;
    facts.drm_seat_isolation = true;
    return facts;
  }

}  // namespace

TEST(SessionAssessment, ReportsEveryFeature) {
  // A feature missing from the report is a feature the user gets no answer
  // about, which is the state this whole layer replaced.
  const auto reports = VDISPLAY::assessSession(workingKwinSession());
  for (const auto feature : {
         VDISPLAY::feature_e::virtual_display,
         VDISPLAY::feature_e::client_requested_mode,
         VDISPLAY::feature_e::exclusive_mode,
         VDISPLAY::feature_e::multiple_displays,
         VDISPLAY::feature_e::isolated_sessions,
         VDISPLAY::feature_e::zero_copy_capture,
       }) {
    const auto it = std::find_if(reports.begin(), reports.end(), [&](const auto &report) {
      return report.feature == feature;
    });
    EXPECT_NE(it, reports.end()) << VDISPLAY::featureName(feature) << " is unreported";
  }
}

TEST(SessionAssessment, EveryVerdictExplainsItself) {
  // The detail is what a user reads; a verdict without one is a verdict they
  // cannot act on. Anything not ready must additionally say what would change.
  VDISPLAY::SessionFacts facts;  // The emptiest session there is: nothing available.
  facts.wayland = true;
  for (const auto &report : VDISPLAY::assessSession(facts)) {
    EXPECT_FALSE(report.detail.empty()) << VDISPLAY::featureName(report.feature) << " has no detail";
  }
}

TEST(SessionAssessment, AWorkingKdeSessionIsReadyThroughout) {
  const auto facts = workingKwinSession();
  for (const auto &report : VDISPLAY::assessSession(facts)) {
    EXPECT_EQ(report.readiness, VDISPLAY::readiness_e::ready)
      << VDISPLAY::featureName(report.feature) << ": " << report.detail;
  }
}

TEST(SessionAssessment, HyprlandCannotDriveAVirtualDisplayAndSaysWhy) {
  auto facts = workingKwinSession();
  facts.compositor = VDISPLAY::compositor_e::hyprland;
  facts.kscreen = false;
  facts.output_management = true;  // Hyprland does implement it - that is not the problem.

  const auto display = reportFor(facts, VDISPLAY::feature_e::virtual_display);
  EXPECT_EQ(display.readiness, VDISPLAY::readiness_e::unavailable);
  EXPECT_NE(display.detail.find("aquamarine"), std::string::npos)
    << "the reason must name the backend, not the protocol: " << display.detail;
  EXPECT_FALSE(display.remediation.empty());

  // Exclusive mode is degraded rather than unavailable: the compositor really
  // can disable an output - that was measured - there is just no virtual
  // display here for it to do that on behalf of. Reporting it as unavailable
  // would send someone looking for a missing protocol that is present.
  const auto exclusive = reportFor(facts, VDISPLAY::feature_e::exclusive_mode);
  EXPECT_EQ(exclusive.readiness, VDISPLAY::readiness_e::degraded);
}

TEST(SessionAssessment, IsolationWithoutTheDrmSeatRuleIsUnavailableNotReady) {
  // The quiet failure this layer exists for: the driver supports session
  // devices, a session would start, its input would be isolated - and its
  // screen would be the host's, because a card with no private seat is on
  // seat0. Every other check passes, so this one has to be explicit.
  auto facts = workingKwinSession();
  facts.drm_seat_isolation = false;
  facts.isolated_sessions_requested = true;

  const auto isolated = reportFor(facts, VDISPLAY::feature_e::isolated_sessions);
  EXPECT_EQ(isolated.readiness, VDISPLAY::readiness_e::unavailable);
  EXPECT_NE(isolated.remediation.find("70-hermes-kms-session-seats.rules"), std::string::npos)
    << "the fix must name the rule to install: " << isolated.remediation;

  // Nothing else may be dragged down with it: the host session still works.
  EXPECT_EQ(reportFor(facts, VDISPLAY::feature_e::virtual_display).readiness, VDISPLAY::readiness_e::ready);
}

TEST(SessionAssessment, IsolationNeedsTheDriverPoolBeforeItNeedsTheSeatRule) {
  // With no session-device pool the seat rule is not the user's problem yet,
  // so the advice must be about the driver rather than about udev.
  auto facts = workingKwinSession();
  facts.hermes_kms_multi_device = false;
  facts.drm_seat_isolation = false;

  const auto isolated = reportFor(facts, VDISPLAY::feature_e::isolated_sessions);
  EXPECT_EQ(isolated.readiness, VDISPLAY::readiness_e::unavailable);
  EXPECT_NE(isolated.remediation.find("hermes-kms"), std::string::npos);
  EXPECT_EQ(isolated.remediation.find("udev"), std::string::npos)
    << "do not send someone to udev before the driver can offer a session device";
}

TEST(SessionAssessment, AWlrootsSessionIsReadyWithoutKscreenOrMutter) {
  auto facts = workingKwinSession();
  facts.compositor = VDISPLAY::compositor_e::wlroots;
  facts.kscreen = false;
  facts.output_management = true;

  EXPECT_EQ(reportFor(facts, VDISPLAY::feature_e::virtual_display).readiness, VDISPLAY::readiness_e::ready);
  EXPECT_EQ(reportFor(facts, VDISPLAY::feature_e::exclusive_mode).readiness, VDISPLAY::readiness_e::ready);
}

TEST(SessionAssessment, AnUnrecognisedCompositorWithTheProtocolIsDegradedNotUnavailable) {
  // It advertises wlr-output-management, so the generic path applies and the
  // session is very likely fine. Calling that "unavailable" would tell someone
  // to abandon a setup that works.
  auto facts = workingKwinSession();
  facts.compositor = VDISPLAY::compositor_e::unknown;
  facts.kscreen = false;
  facts.output_management = true;

  EXPECT_EQ(reportFor(facts, VDISPLAY::feature_e::virtual_display).readiness, VDISPLAY::readiness_e::degraded);
  // And the mode is honestly unknown rather than promised: set_custom_mode is
  // in the protocol, but whether a given compositor honours it is not.
  EXPECT_EQ(reportFor(facts, VDISPLAY::feature_e::client_requested_mode).readiness, VDISPLAY::readiness_e::unknown);
}

TEST(SessionAssessment, MutterDrivesADisplayButCannotBlankThePhysicalOne) {
  auto facts = workingKwinSession();
  facts.compositor = VDISPLAY::compositor_e::mutter;
  facts.kscreen = false;
  facts.mutter = true;

  EXPECT_EQ(reportFor(facts, VDISPLAY::feature_e::virtual_display).readiness, VDISPLAY::readiness_e::ready);
  const auto exclusive = reportFor(facts, VDISPLAY::feature_e::exclusive_mode);
  EXPECT_EQ(exclusive.readiness, VDISPLAY::readiness_e::unavailable);
  EXPECT_FALSE(exclusive.remediation.empty());
}

TEST(SessionAssessment, CaptureFallsBackToACpuCopyWithoutDmabuf) {
  // Without a Hermes-KMS device the capture path is the compositor's, and the
  // difference between dmabuf and no dmabuf is a CPU copy per frame - degraded,
  // not broken, and the user should be told which one they are getting.
  auto facts = workingKwinSession();
  facts.hermes_kms_present = false;
  facts.linux_dmabuf = false;
  facts.screencopy = true;
  EXPECT_EQ(reportFor(facts, VDISPLAY::feature_e::zero_copy_capture).readiness, VDISPLAY::readiness_e::degraded);

  facts.linux_dmabuf = true;
  EXPECT_EQ(reportFor(facts, VDISPLAY::feature_e::zero_copy_capture).readiness, VDISPLAY::readiness_e::ready);

  // COSMIC: wlr-output-management without wlr-screencopy. The display works and
  // the capture does not, which is exactly the split a single verdict hides.
  facts.compositor = VDISPLAY::compositor_e::cosmic;
  facts.screencopy = false;
  facts.image_copy_capture = false;
  facts.output_management = true;
  facts.kscreen = false;
  EXPECT_EQ(reportFor(facts, VDISPLAY::feature_e::virtual_display).readiness, VDISPLAY::readiness_e::ready);
  EXPECT_EQ(reportFor(facts, VDISPLAY::feature_e::zero_copy_capture).readiness, VDISPLAY::readiness_e::unavailable);
}

/**
 * Print what Hermes makes of the session actually running, without asserting
 * anything about it. Disabled because the answer is a property of the machine,
 * not of the code - but the probing half of this layer has no other way to be
 * exercised, and "what does Hermes think my session can do" is the first
 * question asked when someone reports a black stream.
 *
 *   ./build-test/tests/test_sunshine \
 *     --gtest_also_run_disabled_tests --gtest_filter='*LiveSession*'
 *
 * Run it from the session under test: unlike the rest of the suite it must NOT
 * have WAYLAND_DISPLAY stripped.
 */
TEST(SessionAssessment, DISABLED_LiveSession) {
  // platf::init() is what sets the window system Hermes will use; without it
  // the probe reports no session at all, which is how the "not Wayland means
  // X11" bug was found in the first place.
  auto platform = platf::init();
  const auto facts = VDISPLAY::probeSessionFacts();
  std::cout << "compositor:          " << VDISPLAY::compositorName(facts.compositor) << '\n'
            << "wayland:             " << facts.wayland << '\n'
            << "output_management:   " << facts.output_management << '\n'
            << "screencopy:          " << facts.screencopy << '\n'
            << "image_copy_capture:  " << facts.image_copy_capture << '\n'
            << "linux_dmabuf:        " << facts.linux_dmabuf << '\n'
            << "kscreen:             " << facts.kscreen << '\n'
            << "mutter:              " << facts.mutter << '\n'
            << "hermes_kms_present:  " << facts.hermes_kms_present << '\n'
            << "multi_output:        " << facts.hermes_kms_multi_output << '\n'
            << "multi_device:        " << facts.hermes_kms_multi_device << '\n'
            << "drm_seat_isolation:  " << facts.drm_seat_isolation << '\n';
  // Why the device is or is not present is the first thing asked when
  // hermes_kms_present reads 0 with the module plainly loaded.
  const auto kms = VDISPLAY::getHermesKmsStatus();
  std::cout << "  [hermes-kms] module_loaded=" << kms.module_loaded
            << " card_index=" << kms.card_index
            << " uapi=" << kms.uapi_version << " (Hermes needs " << kms.required_uapi_version << ")"
            << " driver=" << kms.driver_version << "\n\n";
  for (const auto &report : VDISPLAY::assessSession(facts)) {
    std::cout << VDISPLAY::featureName(report.feature) << ": "
              << VDISPLAY::readinessName(report.readiness) << "\n  " << report.detail << '\n';
    if (!report.remediation.empty()) {
      std::cout << "  Fix: " << report.remediation << '\n';
    }
  }
}

TEST(SessionAssessment, ASessionWithNoWindowSystemIsUnknownRatherThanX11) {
  // The bug this guards: "not Wayland" was read as "X11", so a process with no
  // window system attached at all - a service started before the session, or
  // any caller that reaches this before the platform is initialised - was told
  // every feature was ready. A diagnostic that invents a working session is
  // worse than one that admits it cannot see.
  VDISPLAY::SessionFacts facts = workingKwinSession();
  facts.wayland = false;
  facts.x11 = false;

  const auto reports = VDISPLAY::assessSession(facts);
  EXPECT_FALSE(reports.empty());
  for (const auto &report : reports) {
    EXPECT_EQ(report.readiness, VDISPLAY::readiness_e::unknown)
      << VDISPLAY::featureName(report.feature) << " claims a verdict without a session";
  }
}

TEST(SessionAssessment, AnX11SessionUsesItsOwnPathRatherThanReportingNoProtocols) {
  auto facts = workingKwinSession();
  facts.wayland = false;
  facts.x11 = true;
  facts.compositor = VDISPLAY::compositor_e::unknown;
  facts.kscreen = false;
  facts.output_management = false;
  facts.screencopy = false;
  facts.linux_dmabuf = false;

  EXPECT_EQ(reportFor(facts, VDISPLAY::feature_e::virtual_display).readiness, VDISPLAY::readiness_e::ready);
  EXPECT_EQ(reportFor(facts, VDISPLAY::feature_e::exclusive_mode).readiness, VDISPLAY::readiness_e::ready);
}
