// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file session-broker-input.h
 * @brief Session-scoped input discovery, descriptor validation and ACL lifetime.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace hermes::session_broker::input_access {
  /** @brief A broker-derived account/seat association, persisted before any ACL. */
  struct lease {
    std::string client;
    std::string account;
    uid_t uid {};
    std::string seat;
  };

  /** @brief Filesystem roots; production always uses the kernel's /sys and /dev. */
  struct roots {
    std::filesystem::path sys {"/sys"};
    std::filesystem::path dev {"/dev"};
  };

  /** @brief A sysfs snapshot, which must be revalidated against an opened fd. */
  struct node {
    std::filesystem::path path;
    std::filesystem::path sysfs;
    dev_t device {};
  };

  /** @brief An operation result; a partial failure must retain the lease for retry. */
  struct result {
    bool ok = true;
    std::size_t changed = 0;
    std::string error;
  };

  /** @return Whether a seat is a canonical, nonzero Hermes-KMS instance. */
  bool valid_seat(std::string_view seat);

  /**
   * @brief Select an optional seat without accepting duplicate or conflicting values.
   * @param environment START assignments, already validated by the protocol parser.
   * @param seat Selected seat; empty for legacy sessions without a seat.
   * @return False for malformed, duplicate or conflicting seat assignments.
   */
  bool seat_from_environment(const std::vector<std::string> &environment, std::string &seat);

  /** @return Trusted leases, an empty vector for ENOENT, or nullopt for unsafe/damaged state. */
  std::optional<std::vector<lease>> read_leases(const std::filesystem::path &path);

  /** @brief Atomically persist validated leases in a private, broker-owned file. */
  bool write_leases(const std::filesystem::path &path, const std::vector<lease> &leases);

  /**
   * @brief Inspect one numbered class node; require a virtual device and exact phys.
   * @return Its snapshot only when /sys/dev/char agrees with its sysfs identity.
   */
  std::optional<node> inspect_node(const roots &paths, const std::filesystem::path &entry, std::string_view seat);

  /** @return Whether an opened descriptor is the character device in the snapshot. */
  bool matches_descriptor(const node &candidate, const struct stat &info);

  /**
   * @brief Prepare an ACL edit in memory, preserving other entries' effective rights.
   * @param acl Caller-owned ACL; acl_create_entry may replace its allocation.
   * @param uid Non-root session UID.
   * @param grant Grant read/write or remove the named UID entry.
   * @return Success/change status; the caller decides whether to apply the ACL.
   */
  result prepare_acl(acl_t &acl, uid_t uid, bool grant);

  /**
   * @brief Edit only one named UID ACL, preserving other users' effective access.
   * @param fd Open descriptor; the production caller must validate its identity first.
   * @param uid Non-root session UID.
   * @param grant Grant read/write, or remove the session's named ACL entry.
   * @return Whether the operation succeeded and whether it changed the ACL.
   */
  result update_descriptor_acl(int fd, uid_t uid, bool grant);

  /** @brief Validate and edit one class node, including for a synchronous udev hook. */
  result update_node(const roots &paths, const lease &owner, const std::filesystem::path &entry, bool grant);

  /**
   * @brief Discover and update only a lease's current virtual character devices.
   *
   * Paths and UIDs never come from a request. Device paths are opened without
   * following symlinks, checked by fstat and sysfs again, and modified by fd.
   * A node removed/recycled during discovery is ignored, never reauthorized.
   */
  result update_devices(const roots &paths, const lease &owner, bool grant);
}  // namespace hermes::session_broker::input_access
