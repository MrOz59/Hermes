/**
 * @file src/platform/linux/session_devices.h
 * @brief What an isolated session sees of the host's /dev.
 */
#pragma once

#ifndef BOOST_PROCESS_VERSION
  #define BOOST_PROCESS_VERSION 1
#endif

#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <sys/types.h>

#include <boost/filesystem/path.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/environment.hpp>
#include <boost/process/v1/group.hpp>

namespace platf::session_devices {

  struct options_t {
    /// A private /dev: basic nodes, the GPUs, and only the session's own input
    /// devices (its seat's, and Steam's virtual pads made inside it).
    bool devices {};
    /// No /dev/snd; the session's audio server is its only way to sound.
    bool sound {};
  };

  /// Whether Hermes holds what isolation needs: CAP_SYS_ADMIN and CAP_MKNOD.
  bool available();

  /**
   * @brief Work done in a session's process between fork and exec.
   *
   * Prepared in Hermes; the child only makes system calls with it.
   */
  class plan_t {
  public:
    /// A new mount namespace with /dev built on the empty directory staging.
    static std::unique_ptr<plan_t> isolate(const std::string &staging, options_t options);

    /// The mount namespace of a running session process.
    static std::unique_ptr<plan_t> join(pid_t pid);

    /// Child side; exits the child on failure.
    void apply() const noexcept;

    plan_t();
    ~plan_t();

    struct op_t;

  private:
    std::string join_path;
    std::string staging;
    std::vector<op_t> ops;
  };

  /// run_command() with plan applied in the child first; no plan runs it as is.
  boost::process::v1::child run_command(const plan_t *plan, const std::string &cmd, boost::filesystem::path &working_dir, const boost::process::v1::environment &env, FILE *file, std::error_code &ec, boost::process::v1::group *group);

  /**
   * @brief Keeps a session's /dev holding its devices while they come and go.
   *
   * Hermes creates copies of the nodes of seat's devices and of Valve virtual
   * pads made by the session's processes, from kernel uevents, so they are
   * there before udev announces them.
   */
  class mirror_t {
  public:
    virtual ~mirror_t() = default;
  };

  /// Mirror into the /dev of pid's mount namespace; nullptr on failure.
  std::unique_ptr<mirror_t> mirror(pid_t pid, const std::string &seat);
}  // namespace platf::session_devices
