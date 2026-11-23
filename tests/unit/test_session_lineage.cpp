/**
 * @file tests/unit/test_session_lineage.cpp
 * @brief Which processes belong to an isolated session.
 */
#include "../tests_common.h"

#include <map>
#include <src/platform/linux/session_lineage.h>

namespace {
  using platf::session_devices::belongs_to_session;

  constexpr ino_t host_ns = 4026531832;
  constexpr ino_t session_ns = 4026533171;
  constexpr ino_t steam_runtime_ns = 4026533228;
  constexpr ino_t proton_ns = 4026533300;

  struct process_t {
    pid_t parent;
    ino_t ns;
  };

  /// systemd (1) -> user manager (900) -> Hermes (1000)
  ///   -> session desktop (2000) -> Steam (2100, its runtime's namespace) -> game (2200, Proton's)
  /// user manager (900) -> desktop Steam (3000, host namespace)
  const std::map<pid_t, process_t> table {
    {1, {0, host_ns}},
    {900, {1, host_ns}},
    {1000, {900, host_ns}},
    {2000, {1000, session_ns}},
    {2100, {2000, steam_runtime_ns}},
    {2200, {2100, proton_ns}},
    {3000, {900, host_ns}},
  };

  bool belongs(pid_t pid) {
    return belongs_to_session(
      pid,
      session_ns,
      [](pid_t p) -> std::optional<ino_t> {
        const auto it = table.find(p);
        return it == table.end() ? std::nullopt : std::optional<ino_t> {it->second.ns};
      },
      [](pid_t p) -> std::optional<pid_t> {
        const auto it = table.find(p);
        return it == table.end() ? std::nullopt : std::optional<pid_t> {it->second.parent};
      }
    );
  }
}  // namespace

TEST(SessionLineage, TheSessionsOwnNamespace) {
  EXPECT_TRUE(belongs(2000));
}

TEST(SessionLineage, ContainersStartedInsideTheSession) {
  EXPECT_TRUE(belongs(2100)) << "the Steam client in its runtime's mount namespace";
  EXPECT_TRUE(belongs(2200)) << "a game in Proton's";
}

TEST(SessionLineage, HostProcessesNever) {
  EXPECT_FALSE(belongs(3000)) << "the desktop's Steam";
  EXPECT_FALSE(belongs(1000)) << "Hermes itself";
  EXPECT_FALSE(belongs(1));
}

TEST(SessionLineage, AProcessThatIsGone) {
  EXPECT_FALSE(belongs(4242));
}
