/**
 * @file src/platform/linux/session_lineage.h
 * @brief Whether a process belongs to an isolated session.
 */
#pragma once

#include <optional>
#include <sys/types.h>

namespace platf::session_devices {

  /**
   * Whether `pid` belongs to the session whose mount namespace is `ns`: it runs
   * in that namespace, or descends from a process that does.
   *
   * Sessions start containers of their own: the Steam client runs in its
   * runtime's mount namespace, and games in Proton's, so a namespace match
   * alone misses the Steam that makes the session's Steam Input pads. A host
   * process never descends from a session's: an orphan is re-parented to
   * init or the user's systemd, both on the host, and the walk stops there.
   *
   * @param mount_ns The mount namespace inode of a pid, or nothing if gone.
   * @param parent The parent pid of a pid, or nothing if gone.
   */
  template<class MountNs, class Parent>
  bool belongs_to_session(pid_t pid, ino_t ns, MountNs mount_ns, Parent parent) {
    // Deep enough for any launcher chain; a cycle would be a kernel bug.
    for (int depth = 0; depth < 64 && pid > 1; ++depth) {
      const std::optional<ino_t> mnt = mount_ns(pid);
      if (!mnt) {
        return false;
      }
      if (*mnt == ns) {
        return true;
      }
      const std::optional<pid_t> up = parent(pid);
      if (!up) {
        return false;
      }
      pid = *up;
    }
    return false;
  }

}  // namespace platf::session_devices
