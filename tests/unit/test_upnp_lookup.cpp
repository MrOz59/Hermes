/**
 * @file tests/unit/test_upnp_lookup.cpp
 * @brief Reproduce the Movistar zero-port lookup defect without trusting foreign rules.
 */

#include "../tests_common.h"

#include <functional>
#include <src/upnp_mapping.h>

namespace {
  class UpnpVerifiedLookupTest: public testing::Test {
  protected:
    upnp::owned_mapping_t wanted {"http://10.0.0.1/control", "WANIPConnection:1", "10.0.0.86", "37989", "37989", "TCP", "Hermes - Client HTTP"};
    upnp::mapping_reply_t first {0, wanted.target, "0", wanted.description, "1", "3600"};
    upnp::mapping_reply_t last = first;
    upnp::mapping_reply_t entry {0, wanted.target, wanted.internal_port, "Hermes - Client HTTPgarbage", "1", "3500", wanted.external_port, wanted.protocol, "0.0.0.0"};
    std::vector<upnp::mapping_reply_t> table {entry};
    int end_result = 713;
    int specific_calls = 0, generic_calls = 0;
    std::function<void()> during_recheck;

    upnp::mapping_lookup_t lookup(bool recorded = true) {
      specific_calls = generic_calls = 0;
      return upnp::verify_mapping_lookup(wanted, recorded, [&]() {
        if (++specific_calls == 1) {
          return first;
        }
        if (during_recheck) {
          during_recheck();
        }
        return last;
      },
                                         [&](std::size_t index) {
                                           ++generic_calls;
                                           return index < table.size() ? table[index] : upnp::mapping_reply_t {end_result};
                                         });
    }
  };

  TEST_F(UpnpVerifiedLookupTest, CorroboratesZeroPortUsingBothQueriesAndLocalOwnership) {
    const auto result = lookup();
    EXPECT_EQ(result.state, upnp::mapping_state_e::ours);
    EXPECT_EQ(result.enabled, "1");
    EXPECT_EQ(result.lease, "3500");
    EXPECT_EQ(specific_calls, 2);
    EXPECT_EQ(generic_calls, 3);
  }

  TEST_F(UpnpVerifiedLookupTest, NormalRoutersNeedNoGenericLookup) {
    first.internal_port = wanted.internal_port;
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::ours);
    EXPECT_EQ(specific_calls, 1);
    EXPECT_EQ(generic_calls, 0);
  }

  TEST_F(UpnpVerifiedLookupTest, NeverAdoptsAnUnrecordedIdenticalRule) {
    EXPECT_EQ(lookup(false).state, upnp::mapping_state_e::conflict);
    EXPECT_EQ(generic_calls, 0);
    first.internal_port = wanted.internal_port;
    EXPECT_FALSE(upnp::may_manage_mapping(lookup(false).state, false));
    EXPECT_EQ(generic_calls, 0);
  }

  TEST_F(UpnpVerifiedLookupTest, OtherPortErrorsAndForeignDescriptionsDoNotUseFallback) {
    for (const auto port : {"", "37990", "00", "-1"}) {
      first.internal_port = port;
      EXPECT_EQ(lookup().state, upnp::mapping_state_e::conflict);
      EXPECT_EQ(generic_calls, 0);
    }
    first.internal_port = "0";
    first.description = "Manual forwarding";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::conflict);
    EXPECT_EQ(generic_calls, 0);
    first.description = wanted.description;
    first.client = "10.0.0.99";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::conflict);
    EXPECT_EQ(generic_calls, 0);
  }

  TEST_F(UpnpVerifiedLookupTest, SpecificLookupFailureIsNotRepairedByEnumeration) {
    for (const int error : {-1, 501, 606, 718}) {
      first.result = error;
      EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
      EXPECT_EQ(generic_calls, 0);
    }
    first.result = 714;
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::missing);
    EXPECT_EQ(generic_calls, 0);
  }

  TEST_F(UpnpVerifiedLookupTest, RequiresTheExactGenericDestinationAndPort) {
    table[0].client = "10.0.0.99";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
    table[0] = entry;
    table[0].internal_port = "37990";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
    table[0].internal_port = "0";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
  }

  TEST_F(UpnpVerifiedLookupTest, NeverUsesAnotherProtocolPortOrRestrictedRemoteHost) {
    table[0].protocol = "UDP";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
    table[0] = entry;
    table[0].external_port = "37990";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
    table[0] = entry;
    table[0].remote_host = "203.0.113.1";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
    table.push_back(entry);
    table.back().remote_host.clear();
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::ours);
  }

  TEST_F(UpnpVerifiedLookupTest, RefusesDuplicateWildcardKeysIncludingDifferentTargets) {
    table.push_back(entry);
    table.back().remote_host.clear();
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
    table.back().client = "10.0.0.99";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
  }

  TEST_F(UpnpVerifiedLookupTest, RequiresCompleteEnumerationEvenAfterFindingTheRule) {
    for (const int error : {-1, 501, 606, 714}) {
      end_result = error;
      EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
      EXPECT_EQ(specific_calls, 1);
    }
    end_result = 713;
    table.clear();
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
  }

  TEST_F(UpnpVerifiedLookupTest, IncompleteSuccessfulResponsesDoNotHideConflictingEntries) {
    for (auto member : {&upnp::mapping_reply_t::client, &upnp::mapping_reply_t::internal_port, &upnp::mapping_reply_t::external_port, &upnp::mapping_reply_t::protocol, &upnp::mapping_reply_t::enabled}) {
      table = {entry, entry};
      table[1].*member = "";
      EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
    }
  }

  TEST_F(UpnpVerifiedLookupTest, BoundsAnUnterminatedEnumeration) {
    auto other = entry;
    other.external_port = "40000";
    table.assign(256, other);
    table[0] = entry;
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
    EXPECT_EQ(generic_calls, 256);
    EXPECT_EQ(specific_calls, 1);
  }

  TEST_F(UpnpVerifiedLookupTest, RechecksOwnershipAfterEnumeration) {
    for (auto member : {&upnp::mapping_reply_t::client, &upnp::mapping_reply_t::internal_port, &upnp::mapping_reply_t::description, &upnp::mapping_reply_t::enabled}) {
      last = first;
      last.*member = "changed";
      EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
      EXPECT_EQ(specific_calls, 2);
    }
    last = first;
    for (const int error : {714, 501, -1}) {
      last.result = error;
      EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
    }
  }

  TEST_F(UpnpVerifiedLookupTest, DisabledAndMalformedEnableStatesAreNotConfused) {
    table[0].enabled = "0";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
    first.enabled = last.enabled = "0";
    const auto disabled = lookup();
    EXPECT_EQ(disabled.state, upnp::mapping_state_e::ours);
    EXPECT_EQ(disabled.enabled, "0");
    first.enabled = last.enabled = table[0].enabled = "";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
  }

  TEST_F(UpnpVerifiedLookupTest, RechecksTheSelectedGenericRowForPortChangesAndIndexShifts) {
    for (auto member : {&upnp::mapping_reply_t::client, &upnp::mapping_reply_t::internal_port, &upnp::mapping_reply_t::external_port, &upnp::mapping_reply_t::protocol, &upnp::mapping_reply_t::remote_host, &upnp::mapping_reply_t::enabled}) {
      table = {entry};
      during_recheck = [&, member]() {
        table[0].*member = "changed";
      };
      EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
    }
    table = {entry};
    during_recheck = [&]() {
      table.clear();
    };
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
  }

  TEST_F(UpnpVerifiedLookupTest, UsesTheShorterFiniteLeaseForReplacement) {
    table[0].lease = "300";
    EXPECT_TRUE(upnp::lease_needs_refresh(lookup().lease));
    table[0].lease = "3600";
    last.lease = "300";
    EXPECT_TRUE(upnp::lease_needs_refresh(lookup().lease));
  }

  TEST_F(UpnpVerifiedLookupTest, RequiresAgreementOnPermanentLeases) {
    first.lease = "0";
    last.lease = "0";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
    table[0].lease = "0";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::ours);
    EXPECT_EQ(lookup().lease, "0");
    last.lease = "3600";
    EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
  }

  TEST_F(UpnpVerifiedLookupTest, MalformedLeasesKeepOwnershipUncertain) {
    for (const auto invalid : {"", "-1", "300junk", "4294967296"}) {
      table[0].lease = invalid;
      EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
      table[0].lease = "3600";
      last.lease = invalid;
      EXPECT_EQ(lookup().state, upnp::mapping_state_e::unknown);
      last = first;
    }
  }
}  // namespace
