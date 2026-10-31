// SPDX-License-Identifier: GPL-3.0-only
/** @file session-broker-input.cpp
 *  @brief Validated virtual-input ACLs for the privileged session broker.
 */
#include "session-broker-input.h"

#include <acl/libacl.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <sys/acl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace hermes::session_broker::input_access {
  namespace {
    namespace fs = std::filesystem;

    struct owned_fd {
      int value = -1;

      ~owned_fd() {
        if (value >= 0) {
          ::close(value);
        }
      }
    };

    struct owned_acl {
      acl_t value {};

      ~owned_acl() {
        if (value) {
          ::acl_free(value);
        }
      }
    };

    bool digits(std::string_view text) {
      return !text.empty() && std::ranges::all_of(text, [](char ch) {
        return ch >= '0' && ch <= '9';
      });
    }

    bool numbered(std::string_view text, std::string_view prefix) {
      return text.starts_with(prefix) && digits(text.substr(prefix.size()));
    }

    template<class T>
    bool number(std::string_view text, T &value) {
      if (!digits(text)) {
        return false;
      }
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      return parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size();
    }

    bool below(const fs::path &path, const fs::path &root) {
      auto part = path.begin();
      for (const auto &component : root) {
        if (part == path.end() || *part++ != component) {
          return false;
        }
      }
      return part != path.end();
    }

    std::optional<std::string> first_line(const fs::path &path) {
      std::ifstream stream {path};
      std::string text;
      if (!std::getline(stream, text) || text.size() > 4096) {
        return std::nullopt;
      }
      return text;
    }

    bool valid_lease(const lease &owner) {
      const bool client = !owner.client.empty() && owner.client.size() <= 64 &&
                          std::ranges::all_of(owner.client, [](char ch) {
                            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                                   (ch >= 'A' && ch <= 'F') || ch == '-';
                          });
      return client && owner.account.starts_with("hermes-s") && owner.account.size() <= 16 &&
             digits(std::string_view {owner.account}.substr(8)) && owner.uid > 0 &&
             owner.uid != std::numeric_limits<uid_t>::max() && valid_seat(owner.seat);
    }

    bool valid_leases(const std::vector<lease> &leases) {
      if (leases.size() > 64) {
        return false;
      }
      std::set<std::string> clients, accounts, seats;
      std::set<uid_t> uids;
      for (const auto &owner : leases) {
        if (!valid_lease(owner) || !clients.insert(owner.client).second || !accounts.insert(owner.account).second || !seats.insert(owner.seat).second || !uids.insert(owner.uid).second) {
          return false;
        }
      }
      return true;
    }

    bool trusted_state(int fd) {
      struct stat info {};

      return ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
             info.st_nlink == 1 && (info.st_mode & 077) == 0 && info.st_size <= 65536;
    }

    result failure(const char *operation) {
      return {.ok = false, .error = std::string {operation} + ": " + std::strerror(errno)};
    }

    std::optional<int> permissions(acl_entry_t entry) {
      acl_permset_t set {};
      if (::acl_get_permset(entry, &set) != 0) {
        return std::nullopt;
      }
      int value = 0;
      for (const auto bit : {ACL_READ, ACL_WRITE, ACL_EXECUTE}) {
        const int present = ::acl_get_perm(set, bit);
        if (present < 0) {
          return std::nullopt;
        }
        if (present) {
          value |= bit;
        }
      }
      return value;
    }

    bool set_permissions(acl_entry_t entry, int value) {
      acl_permset_t set {};
      if (::acl_get_permset(entry, &set) != 0 || ::acl_clear_perms(set) != 0) {
        return false;
      }
      for (const auto bit : {ACL_READ, ACL_WRITE, ACL_EXECUTE}) {
        if ((value & bit) != 0 && ::acl_add_perm(set, bit) != 0) {
          return false;
        }
      }
      return ::acl_set_permset(entry, set) == 0;
    }

    bool is_uid(acl_entry_t entry, uid_t uid) {
      void *qualifier = ::acl_get_qualifier(entry);
      if (!qualifier) {
        return false;
      }
      const bool match = *static_cast<uid_t *>(qualifier) == uid;
      ::acl_free(qualifier);
      return match;
    }

    /** @brief Use the nearest HID parent; input child phys cannot override it. */
    bool belongs_to_seat(const fs::path &leaf, const fs::path &virtual_root, std::string_view seat, bool hidraw) {
      std::optional<std::string> input_phys;
      for (auto parent = leaf; below(parent, virtual_root); parent = parent.parent_path()) {
        if (!input_phys) {
          input_phys = first_line(parent / "phys");
        }
        std::ifstream stream {parent / "uevent"};
        std::string line;
        bool hid = false;
        bool duplicate = false;
        std::optional<std::string> phys;
        while (std::getline(stream, line)) {
          hid |= line.starts_with("HID_ID=");
          if (line.starts_with("HID_PHYS=")) {
            duplicate |= phys.has_value();
            phys = line.substr(9);
          }
        }
        if (hid) {
          return !duplicate && phys && *phys == seat;
        }
      }
      // uinput has no HID parent. Other virtual-device classes do not qualify.
      return !hidraw && below(leaf, virtual_root / "input") && input_phys && *input_phys == seat;
    }
  }  // namespace

  bool valid_seat(std::string_view seat) {
    constexpr std::string_view prefix {"hermes-kms-"};
    if (!seat.starts_with(prefix)) {
      return false;
    }
    const auto suffix = seat.substr(prefix.size());
    std::uint32_t instance = 0;
    return !suffix.empty() && suffix.front() != '0' && number(suffix, instance) && instance != 0;
  }

  bool seat_from_environment(const std::vector<std::string> &environment, std::string &seat) {
    std::optional<std::string> xdg, hermes;
    seat.clear();
    for (const auto &value : environment) {
      for (const auto &[prefix, slot] : {
             std::pair {std::string_view {"XDG_SEAT="}, &xdg},
             std::pair {std::string_view {"HERMES_SESSION_SEAT="}, &hermes}
           }) {
        if (value.starts_with(prefix)) {
          if (*slot) {
            return false;
          }
          *slot = value.substr(prefix.size());
          if (!valid_seat(**slot)) {
            return false;
          }
        }
      }
    }
    if (xdg && hermes && *xdg != *hermes) {
      return false;
    }
    seat = xdg.value_or(hermes.value_or(""));
    return true;
  }

  std::optional<std::vector<lease>> read_leases(const fs::path &path) {
    owned_fd fd {::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (fd.value < 0) {
      return errno == ENOENT ? std::optional {std::vector<lease> {}} : std::nullopt;
    }
    if (!trusted_state(fd.value)) {
      return std::nullopt;
    }
    std::string text;
    std::array<char, 4096> buffer {};
    for (;;) {
      const auto count = ::read(fd.value, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        return std::nullopt;
      }
      if (count == 0) {
        break;
      }
      text.append(buffer.data(), static_cast<std::size_t>(count));
      if (text.size() > 65536 || text.find('\0') != std::string::npos) {
        return std::nullopt;
      }
    }
    std::vector<lease> leases;
    std::istringstream lines {text};
    std::string line;
    while (std::getline(lines, line)) {
      if (line.empty()) {
        continue;
      }
      std::istringstream fields {line};
      lease owner;
      std::string uid, extra;
      if (!(fields >> owner.client >> owner.account >> uid >> owner.seat) || (fields >> extra) || !number(uid, owner.uid)) {
        return std::nullopt;
      }
      leases.push_back(std::move(owner));
    }
    return valid_leases(leases) ? std::optional {std::move(leases)} : std::nullopt;
  }

  bool write_leases(const fs::path &path, const std::vector<lease> &leases) {
    if (!valid_leases(leases) || !read_leases(path)) {
      return false;
    }
    std::string temporary = path.string() + ".XXXXXX";
    owned_fd fd {::mkostemp(temporary.data(), O_CLOEXEC)};
    if (fd.value < 0) {
      return false;
    }
    const auto finish = [&](bool ok) {
      ::unlink(temporary.c_str());
      return ok;
    };
    std::string text;
    for (const auto &owner : leases) {
      text += owner.client + ' ' + owner.account + ' ' + std::to_string(owner.uid) + ' ' + owner.seat + '\n';
    }
    std::size_t written = 0;
    while (written < text.size()) {
      const auto count = ::write(fd.value, text.data() + written, text.size() - written);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return finish(false);
      }
      written += static_cast<std::size_t>(count);
    }
    if (::fsync(fd.value) != 0 || ::rename(temporary.c_str(), path.c_str()) != 0) {
      return finish(false);
    }
    const auto directory = path.parent_path().empty() ? fs::path {"."} : path.parent_path();
    owned_fd parent {::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    return finish(parent.value >= 0 && ::fsync(parent.value) == 0);
  }

  std::optional<node> inspect_node(const roots &paths, const fs::path &entry, std::string_view seat) {
    if (!valid_seat(seat)) {
      return std::nullopt;
    }
    const auto name = entry.filename().string();
    const bool hidraw = numbered(name, "hidraw");
    const auto class_root = paths.sys / "class" / (hidraw ? "hidraw" : "input");
    if ((!hidraw && !numbered(name, "event") && !numbered(name, "js")) || entry.parent_path() != class_root) {
      return std::nullopt;
    }
    std::error_code error;
    const auto canonical = fs::canonical(entry, error);
    if (error || !below(canonical, paths.sys / "devices" / "virtual") || !belongs_to_seat(canonical, paths.sys / "devices" / "virtual", seat, hidraw)) {
      return std::nullopt;
    }
    const auto dev = first_line(canonical / "dev");
    if (!dev) {
      return std::nullopt;
    }
    const auto colon = dev->find(':');
    unsigned int major_number = 0, minor_number = 0;
    if (colon == std::string::npos || !number(std::string_view {*dev}.substr(0, colon), major_number) || !number(std::string_view {*dev}.substr(colon + 1), minor_number) || major_number == 0) {
      return std::nullopt;
    }
    const auto device = ::makedev(major_number, minor_number);
    if (::major(device) != major_number || ::minor(device) != minor_number) {
      return std::nullopt;
    }
    const auto by_device = fs::canonical(paths.sys / "dev" / "char" / *dev, error);
    if (error || by_device != canonical) {
      return std::nullopt;
    }
    return node {.path = (hidraw ? paths.dev : paths.dev / "input") / name, .sysfs = canonical, .device = device};
  }

  bool matches_descriptor(const node &candidate, const struct stat &info) {
    return S_ISCHR(info.st_mode) && info.st_rdev == candidate.device;
  }

  result prepare_acl(acl_t &acl, uid_t uid, bool grant) {
    if (uid == 0 || uid == std::numeric_limits<uid_t>::max()) {
      return {.ok = false, .error = "refusing a root or invalid session UID"};
    }
    if (!acl || ::acl_valid(acl) != 0) {
      return {.ok = false, .error = "invalid original ACL"};
    }
    int mask = ACL_READ | ACL_WRITE | ACL_EXECUTE;
    acl_entry_t own {};
    acl_entry_t entry {};
    int next = ACL_FIRST_ENTRY;
    int found = 0;
    while ((found = ::acl_get_entry(acl, next, &entry)) == 1) {
      next = ACL_NEXT_ENTRY;
      acl_tag_t tag {};
      const auto bits = permissions(entry);
      if (::acl_get_tag_type(entry, &tag) != 0 || !bits) {
        return failure("inspect device ACL");
      }
      if (tag == ACL_MASK) {
        mask = *bits;
      } else if (tag == ACL_USER && is_uid(entry, uid)) {
        own = entry;
      }
    }
    if (found < 0) {
      return failure("enumerate device ACL");
    }
    constexpr int requested = ACL_READ | ACL_WRITE;
    const auto existing = own ? permissions(own) : std::optional<int> {};
    if ((!grant && !own) || (grant && existing && ((*existing & mask & requested) == requested))) {
      return {};
    }
    // Raising a POSIX ACL mask can otherwise activate permissions that an
    // administrator deliberately masked off for a different user or group.
    next = ACL_FIRST_ENTRY;
    while (::acl_get_entry(acl, next, &entry) == 1) {
      next = ACL_NEXT_ENTRY;
      acl_tag_t tag {};
      const auto bits = permissions(entry);
      if (::acl_get_tag_type(entry, &tag) != 0 || !bits || ((tag == ACL_USER || tag == ACL_GROUP || tag == ACL_GROUP_OBJ) && !set_permissions(entry, *bits & mask))) {
        return failure("preserve existing ACL permissions");
      }
    }
    if (grant) {
      // acl_create_entry may reallocate the ACL; do not reuse old entry handles.
      if (!own && (::acl_create_entry(&acl, &own) != 0 || ::acl_set_tag_type(own, ACL_USER) != 0 || ::acl_set_qualifier(own, &uid) != 0)) {
        return failure("create session ACL entry");
      }
      if (!set_permissions(own, requested)) {
        return failure("set session ACL permissions");
      }
    } else if (::acl_delete_entry(acl, own) != 0) {
      return failure("remove session ACL entry");
    }
    if (::acl_calc_mask(&acl) != 0 || ::acl_valid(acl) != 0) {
      return failure("validate session ACL");
    }
    return {.ok = true, .changed = 1, .error = {}};
  }

  result update_descriptor_acl(int fd, uid_t uid, bool grant) {
    owned_acl acl {::acl_get_fd(fd)};
    if (!acl.value) {
      return failure("read device ACL");
    }
    const auto prepared = prepare_acl(acl.value, uid, grant);
    if (!prepared.ok || !prepared.changed) {
      return prepared;
    }
    if (::acl_set_fd(fd, acl.value) != 0) {
      return failure("write device ACL");
    }
    return prepared;
  }

  result update_node(const roots &paths, const lease &owner, const fs::path &entry, bool grant) {
    if (!valid_lease(owner)) {
      return {.ok = false, .error = "invalid input lease"};
    }
    const auto candidate = inspect_node(paths, entry, owner.seat);
    if (!candidate) {
      return {};
    }
    owned_fd fd {::open(candidate->path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)};
    if (fd.value < 0) {
      if (errno == ENOENT || errno == ENODEV || errno == ENXIO) {
        return {};
      }
      return failure("open session input device");
    }

    struct stat info {};

    const auto current = inspect_node(paths, entry, owner.seat);
    if (::fstat(fd.value, &info) != 0 || !matches_descriptor(*candidate, info) || !current || current->device != candidate->device || current->sysfs != candidate->sysfs) {
      return {};
    }
    return update_descriptor_acl(fd.value, owner.uid, grant);
  }

  result update_devices(const roots &paths, const lease &owner, bool grant) {
    if (!valid_lease(owner)) {
      return {.ok = false, .error = "invalid input lease"};
    }
    result total;
    const auto remember = [&](const result &error) {
      if (total.ok) {
        total.ok = false;
        total.error = error.error;
      }
    };
    for (const auto *subsystem : {"input", "hidraw"}) {
      std::error_code error;
      const auto class_root = paths.sys / "class" / subsystem;
      if (!fs::exists(class_root, error)) {
        if (error) {
          remember({.ok = false, .error = "cannot inspect input class: " + error.message()});
        }
        continue;
      }
      for (fs::directory_iterator it {class_root, error}, end; !error && it != end; it.increment(error)) {
        const auto updated = update_node(paths, owner, it->path(), grant);
        if (!updated.ok) {
          remember(updated);
          continue;
        }
        total.changed += updated.changed;
      }
      if (error) {
        remember({.ok = false, .error = "cannot enumerate input class: " + error.message()});
      }
    }
    return total;
  }
}  // namespace hermes::session_broker::input_access
