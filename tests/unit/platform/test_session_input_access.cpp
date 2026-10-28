// SPDX-License-Identifier: GPL-3.0-only
/** @file test_session_input_access.cpp
 *  @brief Input lease, sysfs identity and ACL tests without touching host devices.
 */
#if defined(__linux__) && defined(HERMES_TEST_SESSION_INPUT)
  #include "tools/session-broker-input.h"

  #include <acl/libacl.h>
  #include <cerrno>
  #include <fcntl.h>
  #include <filesystem>
  #include <fstream>
  #include <gtest/gtest.h>
  #include <limits>
  #include <sstream>
  #include <sys/sysmacros.h>
  #include <unistd.h>

namespace {
  namespace access = hermes::session_broker::input_access;
  namespace fs = std::filesystem;

  class SessionInputAccessTest: public testing::Test {
  protected:
    void SetUp() override {
      std::string pattern = (fs::temp_directory_path() / "hermes-input-access-XXXXXX").string();
      const auto *directory = ::mkdtemp(pattern.data());
      ASSERT_NE(directory, nullptr);
      root = directory;
      paths = {.sys = root / "sys", .dev = root / "dev"};
      state = root / "leases";
      fs::create_directories(paths.sys / "class" / "input");
      fs::create_directories(paths.sys / "class" / "hidraw");
      fs::create_directories(paths.sys / "dev" / "char");
      fs::create_directories(paths.dev / "input");
    }

    void TearDown() override {
      fs::remove_all(root);
    }

    void put(const fs::path &path, const std::string &text) {
      fs::create_directories(path.parent_path());
      std::ofstream {path} << text;
    }

    fs::path input_node(std::string name = "event0", std::string seat = "hermes-kms-1", std::string device = "13:64") {
      const auto input = paths.sys / "devices" / "virtual" / "input" / "input20";
      const auto leaf = input / name;
      put(input / "phys", seat + "\n");
      put(leaf / "dev", device + "\n");
      const auto entry = paths.sys / "class" / "input" / name;
      fs::create_directory_symlink(leaf, entry);
      fs::create_directory_symlink(leaf, paths.sys / "dev" / "char" / device);
      return entry;
    }

    fs::path hid_node(bool hidraw, std::string seat = "hermes-kms-1") {
      const auto parent = paths.sys / "devices" / "virtual" / "misc" / "uhid" / "0005:054C:0CE6.0042";
      put(parent / "uevent", "HID_ID=0005:0000054C:00000CE6\nHID_PHYS=" + seat + "\n");
      const auto leaf = hidraw ? parent / "hidraw" / "hidraw7" : parent / "input" / "input77" / "event7";
      const std::string device = hidraw ? "243:7" : "13:71";
      put(leaf / "dev", device + "\n");
      // A child's phys cannot overrule the HID parent's identity.
      if (!hidraw) {
        put(leaf.parent_path() / "phys", "hermes-kms-99\n");
      }
      const auto entry = paths.sys / "class" / (hidraw ? "hidraw" : "input") / (hidraw ? "hidraw7" : "event7");
      fs::create_directory_symlink(leaf, entry);
      fs::create_directory_symlink(leaf, paths.sys / "dev" / "char" / device);
      return entry;
    }

    static int perms(acl_t acl, acl_tag_t wanted, unsigned int id = 0) {
      int mask = 7, bits = -1;
      acl_entry_t entry {};
      int next = ACL_FIRST_ENTRY;
      while (::acl_get_entry(acl, next, &entry) == 1) {
        next = ACL_NEXT_ENTRY;
        acl_tag_t tag {};
        ::acl_get_tag_type(entry, &tag);
        bool matches = tag == wanted;
        if (matches && (tag == ACL_USER || tag == ACL_GROUP)) {
          void *qualifier = ::acl_get_qualifier(entry);
          matches = qualifier && *static_cast<unsigned int *>(qualifier) == id;
          ::acl_free(qualifier);
        }
        acl_permset_t set {};
        ::acl_get_permset(entry, &set);
        int value = 0;
        for (const auto bit : {ACL_READ, ACL_WRITE, ACL_EXECUTE}) {
          if (::acl_get_perm(set, bit) == 1) {
            value |= bit;
          }
        }
        if (tag == ACL_MASK) {
          mask = value;
        }
        if (matches) {
          bits = value;
        }
      }
      return bits < 0 ? bits : wanted == ACL_USER_OBJ || wanted == ACL_OTHER ? bits :
                                                                               bits & mask;
    }

    fs::path root, state;
    access::roots paths;
    access::lease owner {"abcd-1234", "hermes-s01", 12001, "hermes-kms-1"};
  };

  TEST_F(SessionInputAccessTest, AcceptsOnlyCanonicalNonzeroSeats) {
    for (const auto *seat : {"hermes-kms-1", "hermes-kms-12", "hermes-kms-4294967295"}) {
      EXPECT_TRUE(access::valid_seat(seat)) << seat;
    }
    for (const auto *seat : {"", "seat0", "hermes-kms-0", "hermes-kms-01", "hermes-kms--1", "hermes-kms-1/../2", "hermes-kms-4294967296", "hermes-kms-1\n"}) {
      EXPECT_FALSE(access::valid_seat(seat)) << seat;
    }
  }

  TEST_F(SessionInputAccessTest, RejectsDuplicateConflictingOrMalformedEnvironmentSeats) {
    std::string seat;
    EXPECT_TRUE(access::seat_from_environment({"DISPLAY=:0"}, seat));
    EXPECT_TRUE(seat.empty());
    EXPECT_TRUE(access::seat_from_environment({"XDG_SEAT=hermes-kms-12", "HERMES_SESSION_SEAT=hermes-kms-12"}, seat));
    EXPECT_EQ(seat, "hermes-kms-12");
    EXPECT_FALSE(access::seat_from_environment({"XDG_SEAT=hermes-kms-1", "XDG_SEAT=hermes-kms-1"}, seat));
    EXPECT_FALSE(access::seat_from_environment({"XDG_SEAT=hermes-kms-1", "HERMES_SESSION_SEAT=hermes-kms-2"}, seat));
    EXPECT_FALSE(access::seat_from_environment({"XDG_SEAT=seat0"}, seat));
    EXPECT_FALSE(access::seat_from_environment({"HERMES_SESSION_SEAT="}, seat));
  }

  TEST_F(SessionInputAccessTest, PersistsLeasesPrivatelyAndReadsThemAfterRestart) {
    ASSERT_TRUE(access::read_leases(state));
    EXPECT_TRUE(access::read_leases(state)->empty());
    ASSERT_TRUE(access::write_leases(state, {owner}));

    struct stat info {};

    ASSERT_EQ(::stat(state.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777U, 0600U);
    const auto reloaded = access::read_leases(state);
    ASSERT_TRUE(reloaded);
    ASSERT_EQ(reloaded->size(), 1U);
    EXPECT_EQ(reloaded->front().client, owner.client);
    EXPECT_EQ(reloaded->front().account, owner.account);
    EXPECT_EQ(reloaded->front().uid, owner.uid);
    EXPECT_EQ(reloaded->front().seat, owner.seat);
    ASSERT_TRUE(access::write_leases(state, {}));
    EXPECT_TRUE(access::read_leases(state)->empty());
  }

  TEST_F(SessionInputAccessTest, RefusesCollidingClientsAccountsUidsAndSeatsWithoutLosingState) {
    ASSERT_TRUE(access::write_leases(state, {owner}));
    const access::lease other {"abcd-5678", "hermes-s02", 12002, "hermes-kms-2"};
    for (int field = 0; field < 4; ++field) {
      auto conflict = other;
      if (field == 0) {
        conflict.client = owner.client;
      }
      if (field == 1) {
        conflict.account = owner.account;
      }
      if (field == 2) {
        conflict.uid = owner.uid;
      }
      if (field == 3) {
        conflict.seat = owner.seat;
      }
      EXPECT_FALSE(access::write_leases(state, {owner, conflict}));
      ASSERT_EQ(access::read_leases(state)->size(), 1U);
    }
    EXPECT_TRUE(access::write_leases(state, {owner, other}));
  }

  TEST_F(SessionInputAccessTest, RefusesUnsafeLeaseFilesAndNonexistentParents) {
    put(root / "target", "unchanged\n");
    fs::create_symlink(root / "target", state);
    EXPECT_FALSE(access::read_leases(state));
    EXPECT_FALSE(access::write_leases(state, {owner}));
    fs::remove(state);
    ASSERT_EQ(::mkfifo(state.c_str(), 0600), 0);
    EXPECT_FALSE(access::read_leases(state));
    fs::remove(state);
    ASSERT_TRUE(access::write_leases(state, {owner}));
    fs::create_hard_link(state, root / "alias");
    EXPECT_FALSE(access::read_leases(state));
    fs::remove(root / "alias");
    ASSERT_EQ(::chmod(state.c_str(), 0666), 0);
    EXPECT_FALSE(access::read_leases(state));
    EXPECT_FALSE(access::write_leases(root / "missing" / "state", {owner}));
  }

  TEST_F(SessionInputAccessTest, RefusesDamagedRootOrOversizedLeaseRecords) {
    for (const auto &record : {"abcd hermes-s01 0 hermes-kms-1\n", "abcd root 123 hermes-kms-1\n", "abcd hermes-s01 12suffix hermes-kms-1\n", "abcd hermes-s01 123 seat0\n", "abcd hermes-s01 123 hermes-kms-1 extra\n"}) {
      put(state, record);
      ASSERT_EQ(::chmod(state.c_str(), 0600), 0);
      EXPECT_FALSE(access::read_leases(state)) << record;
    }
    put(state, std::string(65537, 'a'));
    EXPECT_FALSE(access::read_leases(state));
    put(state, std::string {"abcd"} + '\0' + " hermes-s01 123 hermes-kms-1\n");
    EXPECT_FALSE(access::read_leases(state));
  }

  TEST_F(SessionInputAccessTest, DiscoversOnlyExactVirtualSeatAndDeviceNumber) {
    const auto entry = input_node();
    const auto found = access::inspect_node(paths, entry, owner.seat);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->path, paths.dev / "input" / "event0");
    EXPECT_EQ(found->device, ::makedev(13, 64));
    EXPECT_FALSE(access::inspect_node(paths, entry, "hermes-kms-2"));
    put(fs::canonical(entry).parent_path() / "phys", "hermes-kms-10\n");
    EXPECT_FALSE(access::inspect_node(paths, entry, "hermes-kms-1"));
    put(fs::canonical(entry).parent_path() / "phys", "hermes-kms-1\n");
    fs::remove(paths.sys / "dev" / "char" / "13:64");
    EXPECT_FALSE(access::inspect_node(paths, entry, owner.seat));
  }

  TEST_F(SessionInputAccessTest, RejectsPhysicalOrOutsideVirtualDeviceAncestry) {
    const auto physical = paths.sys / "devices" / "pci0000:00" / "input20" / "event0";
    put(physical / "dev", "13:64\n");
    put(physical.parent_path() / "phys", owner.seat + "\n");
    const auto entry = paths.sys / "class" / "input" / "event0";
    fs::create_directory_symlink(physical, entry);
    fs::create_directory_symlink(physical, paths.sys / "dev" / "char" / "13:64");
    EXPECT_FALSE(access::inspect_node(paths, entry, owner.seat));
    fs::remove(entry);
    const auto lookalike = paths.sys / "devices" / "virtual-foreign" / "input20" / "event0";
    put(lookalike / "dev", "13:64\n");
    put(lookalike.parent_path() / "phys", owner.seat + "\n");
    fs::create_directory_symlink(lookalike, entry);
    EXPECT_FALSE(access::inspect_node(paths, entry, owner.seat));
  }

  TEST_F(SessionInputAccessTest, DiscoversHidrawAndRenamedInputChildrenThroughTheirNearestHidParent) {
    const auto event = hid_node(false);
    const auto raw = hid_node(true);
    EXPECT_TRUE(access::inspect_node(paths, event, owner.seat));
    EXPECT_TRUE(access::inspect_node(paths, raw, owner.seat));
    EXPECT_FALSE(access::inspect_node(paths, event, "hermes-kms-99"));
    EXPECT_FALSE(access::inspect_node(paths, raw, "hermes-kms-2"));
    const auto parent = fs::canonical(event).parent_path().parent_path().parent_path();
    put(parent / "uevent", "HID_ID=0005:0000054C:00000CE6\n");
    EXPECT_FALSE(access::inspect_node(paths, event, owner.seat));
    put(parent / "uevent", "HID_ID=0005:0000054C:00000CE6\nHID_PHYS=hermes-kms-1\nHID_PHYS=hermes-kms-2\n");
    EXPECT_FALSE(access::inspect_node(paths, event, owner.seat));
  }

  TEST_F(SessionInputAccessTest, RejectsNonNumberedNamesAndMismatchedOpenedDescriptors) {
    const auto entry = input_node("event0-foreign");
    EXPECT_FALSE(access::inspect_node(paths, entry, owner.seat));
    access::node candidate {.path = {}, .sysfs = {}, .device = ::makedev(13, 64)};

    struct stat info {};

    info.st_mode = S_IFREG | 0600;
    info.st_rdev = candidate.device;
    EXPECT_FALSE(access::matches_descriptor(candidate, info));
    info.st_mode = S_IFCHR | 0600;
    EXPECT_TRUE(access::matches_descriptor(candidate, info));
    info.st_rdev = ::makedev(13, 65);
    EXPECT_FALSE(access::matches_descriptor(candidate, info));
  }

  TEST_F(SessionInputAccessTest, DoesNotApplyAnAclToARegularFileOrSymlinkReusingADeviceName) {
    input_node();
    put(paths.dev / "input" / "event0", "unrelated file");
    const auto regular = access::update_devices(paths, owner, true);
    EXPECT_TRUE(regular.ok);
    EXPECT_EQ(regular.changed, 0U);
    fs::remove(paths.dev / "input" / "event0");
    fs::create_symlink(root / "target", paths.dev / "input" / "event0");
    put(root / "target", "unrelated file");
    const auto symlink = access::update_devices(paths, owner, true);
    EXPECT_FALSE(symlink.ok);
    EXPECT_EQ(symlink.changed, 0U);
  }

  TEST_F(SessionInputAccessTest, PreservesMaskedUsersAndGroupsWhenGrantingAndRevoking) {
    acl_t acl = ::acl_from_text("u::rw-,u:101:rwx,g::rwx,g:202:rwx,m::r--,o::---");
    ASSERT_NE(acl, nullptr);
    const auto granted = access::prepare_acl(acl, owner.uid, true);
    ASSERT_TRUE(granted.ok) << granted.error;
    EXPECT_EQ(granted.changed, 1U);
    EXPECT_EQ(perms(acl, ACL_USER, owner.uid), ACL_READ | ACL_WRITE);
    EXPECT_EQ(perms(acl, ACL_USER, 101), ACL_READ);
    EXPECT_EQ(perms(acl, ACL_GROUP, 202), ACL_READ);
    EXPECT_EQ(perms(acl, ACL_GROUP_OBJ), ACL_READ);
    EXPECT_EQ(perms(acl, ACL_USER_OBJ), ACL_READ | ACL_WRITE);
    EXPECT_EQ(perms(acl, ACL_OTHER), 0);
    EXPECT_EQ(access::prepare_acl(acl, owner.uid, true).changed, 0U);
    ASSERT_TRUE(access::prepare_acl(acl, owner.uid, false).ok);
    EXPECT_EQ(perms(acl, ACL_USER, owner.uid), -1);
    EXPECT_EQ(perms(acl, ACL_USER, 101), ACL_READ);
    EXPECT_EQ(perms(acl, ACL_GROUP, 202), ACL_READ);
    EXPECT_EQ(access::prepare_acl(acl, owner.uid, false).changed, 0U);
    ::acl_free(acl);
  }

  TEST_F(SessionInputAccessTest, GrantsAnExistingMaskedEntryWithoutBroadeningAnotherUid) {
    acl_t acl = ::acl_from_text("u::rw-,u:12001:rw-,u:12002:rwx,g::---,m::r--,o::---");
    ASSERT_NE(acl, nullptr);
    ASSERT_TRUE(access::prepare_acl(acl, owner.uid, true).ok);
    EXPECT_EQ(perms(acl, ACL_USER, owner.uid), ACL_READ | ACL_WRITE);
    EXPECT_EQ(perms(acl, ACL_USER, 12002), ACL_READ);
    ::acl_free(acl);
  }

  TEST_F(SessionInputAccessTest, RejectsRootInvalidUidsAndInvalidAcls) {
    acl_t acl = ::acl_from_text("u::rw-,g::r--,o::---");
    ASSERT_NE(acl, nullptr);
    EXPECT_FALSE(access::prepare_acl(acl, 0, true).ok);
    EXPECT_FALSE(access::prepare_acl(acl, std::numeric_limits<uid_t>::max(), true).ok);
    ::acl_free(acl);
    acl = nullptr;
    EXPECT_FALSE(access::prepare_acl(acl, owner.uid, true).ok);
    EXPECT_FALSE(access::update_descriptor_acl(-1, owner.uid, true).ok);
  }

  TEST_F(SessionInputAccessTest, AppliesAclByDescriptorEvenAfterTheFileIsRenamed) {
    // The editor is tested on our own regular file; production additionally
    // requires a validated character device before calling it.
    std::ifstream map {"/proc/self/uid_map"};
    unsigned long long inside = 0, outside = 0, count = 0;
    bool mapped = false;
    while (map >> inside >> outside >> count) {
      mapped |= owner.uid >= inside && static_cast<unsigned long long>(owner.uid) - inside < count;
    }
    if (!mapped) {
      GTEST_SKIP() << "The test namespace maps only UID 0; named non-root ACLs require the kernel CI guest";
    }
    put(root / "owned", "owned");
    const int fd = ::open((root / "owned").c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_GE(fd, 0);
    fs::rename(root / "owned", root / "renamed");
    put(root / "owned", "replacement");
    const auto granted = access::update_descriptor_acl(fd, owner.uid, true);
    ASSERT_TRUE(granted.ok) << granted.error;
    acl_t actual = ::acl_get_file((root / "renamed").c_str(), ACL_TYPE_ACCESS);
    ASSERT_NE(actual, nullptr);
    EXPECT_EQ(perms(actual, ACL_USER, owner.uid), ACL_READ | ACL_WRITE);
    ::acl_free(actual);
    actual = ::acl_get_file((root / "owned").c_str(), ACL_TYPE_ACCESS);
    ASSERT_NE(actual, nullptr);
    EXPECT_EQ(perms(actual, ACL_USER, owner.uid), -1);
    ::acl_free(actual);
    ASSERT_TRUE(access::update_descriptor_acl(fd, owner.uid, false).ok);
    ::close(fd);
  }
}  // namespace
#endif
