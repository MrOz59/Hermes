/**
 * @file src/platform/linux/monitor_restore.h
 * @brief Verified physical-output restoration, independent of the compositor transport.
 */
#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace VDISPLAY {
  /** @brief One output as `kscreen-doctor -j` reports it. */
  struct kscreen_output_t {
    std::string name;  ///< Connector name, e.g. "HDMI-A-1".
    bool connected {false};  ///< The connector exists in this session.
    bool enabled {false};  ///< KWin is composing on it.
    int priority {0};  ///< KScreen priority; 1 marks the primary output.
    int x {0};  ///< Logical x.
    int y {0};  ///< Logical y.
    int width {0};  ///< Logical width.
    int height {0};  ///< Logical height.
  };

  namespace monitor_restore {
    using targets_t = std::map<std::string, int>;
    using snapshot_t = std::optional<std::vector<kscreen_output_t>>;

    /**
     * Restore only saved connectors still physically present. A successful
     * command is not confirmation: read the compositor back before releasing
     * the virtual output. Conversely, a timed-out command may have applied its
     * transaction; a confirmed live layout is sufficient in that case too.
     * A missing snapshot is an error, never an empty/headless desktop.
     */
    template<class Query, class Apply>
    bool restore(const targets_t &saved, Query query, Apply apply) {
      if (saved.empty()) {
        return true;
      }
      const auto before = query();
      if (!before) {
        return false;
      }
      targets_t connected;
      bool already_restored = true;
      for (const auto &output : *before) {
        const auto target = saved.find(output.name);
        if (target != saved.end() && output.connected) {
          connected.emplace(*target);
          already_restored &= output.enabled &&
                               (target->second <= 0 || output.priority == target->second);
        }
      }
      if (connected.empty() || already_restored) {
        return true;
      }

      // Always verify, even if the command failed after partially applying.
      apply(connected);
      const auto after = query();
      if (!after) {
        return false;
      }
      for (const auto &[name, priority] : connected) {
        const auto output = std::find_if(after->begin(), after->end(), [&name](const auto &entry) {
          return entry.name == name;
        });
        if (output != after->end() && output->connected && !output->enabled) {
          return false;
        }
      }
      return true;
    }
  }  // namespace monitor_restore
}  // namespace VDISPLAY
