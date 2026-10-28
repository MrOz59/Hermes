/**
 * @file src/platform/linux/session_devices.cpp
 * @brief What an isolated session sees of the host's /dev.
 */
#include "session_devices.h"

// standard includes
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

// platform includes
#include <fcntl.h>
#include <linux/netlink.h>
#include <sched.h>
#include <sys/capability.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

// lib includes
#include <boost/process/v1/env.hpp>
#include <boost/process/v1/extend.hpp>
#include <boost/process/v1/handles.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/start_dir.hpp>

// local includes
#include "src/logging.h"

namespace bp = boost::process::v1;
namespace fs = std::filesystem;
using namespace std::literals;

namespace platf::session_devices {
  struct plan_t::op_t {
    enum kind_e {
      tmpfs,
      mkdir,
      mknod,
      symlink,
      bind,
      move,
    } kind;

    std::string path;
    std::string source;
    mode_t mode {};
    dev_t dev {};
    unsigned long flags {};
  };

  namespace {
    constexpr std::uint32_t needed_caps = (1u << CAP_SYS_ADMIN) | (1u << CAP_MKNOD) | (1u << CAP_SYS_CHROOT);

    [[noreturn]] void fail(const char *what) noexcept {
      constexpr char prefix[] = "hermes: session /dev setup failed at ";
      (void) !::write(STDERR_FILENO, prefix, sizeof prefix - 1);
      (void) !::write(STDERR_FILENO, what, std::strlen(what));
      (void) !::write(STDERR_FILENO, "\n", 1);
      ::_exit(127);
    }

    // Raw system calls: the child of a multithreaded parent may not allocate.
    bool set_caps(bool raise) noexcept {
      __user_cap_header_struct header {_LINUX_CAPABILITY_VERSION_3, 0};
      __user_cap_data_struct data[2] {};
      if (::syscall(SYS_capget, &header, data)) {
        return false;
      }
      if (raise) {
        data[0].effective |= data[0].permitted & needed_caps;
      } else {
        for (auto &set : data) {
          set.effective = set.permitted = set.inheritable = 0;
        }
      }
      return ::syscall(SYS_capset, &header, data) == 0;
    }

    bool has_cap(cap_t caps, cap_value_t cap) {
      cap_flag_value_t value = CAP_CLEAR;
      return cap_get_flag(caps, cap, CAP_PERMITTED, &value) == 0 && value == CAP_SET;
    }
  }  // namespace

  plan_t::plan_t() = default;
  plan_t::~plan_t() = default;

  bool available() {
    cap_t caps = cap_get_proc();
    if (!caps) {
      return false;
    }
    const bool ok = has_cap(caps, CAP_SYS_ADMIN) && has_cap(caps, CAP_MKNOD);
    cap_free(caps);
    return ok;
  }

  std::unique_ptr<plan_t> plan_t::isolate(const std::string &staging, options_t options) {
    auto plan = std::make_unique<plan_t>();
    auto add = [&](op_t::kind_e kind, std::string path, std::string source = {}, mode_t mode = 0, dev_t dev = 0, unsigned long flags = 0) {
      plan->ops.push_back({kind, std::move(path), std::move(source), mode, dev, flags});
    };

    if (!options.devices) {
      if (options.sound && fs::is_directory("/dev/snd")) {
        add(op_t::tmpfs, "/dev/snd", {}, 0, 0, MS_NODEV);
      }
      return plan;
    }

    plan->staging = staging;
    const auto under = [&](const std::string &name) {
      return staging + "/" + name;
    };
    add(op_t::tmpfs, staging);

    // Copies of the nodes anyone may use, and of those the user can already
    // open. Never the host's input, sound, USB, camera or storage devices.
    std::vector<std::string> nodes {"null", "zero", "full", "random", "urandom", "tty", "ptmx", "fuse", "uinput", "ntsync", "kfd", "udmabuf"};
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator {"/dev", ec}) {
      if (const auto name = entry.path().filename().string(); name.starts_with("nvidia")) {
        nodes.push_back(name);
      }
    }
    for (const auto &name : nodes) {
      const auto path = "/dev/" + name;
      struct stat st {};
      if (::lstat(path.c_str(), &st) || !S_ISCHR(st.st_mode) ||
          (::access(path.c_str(), R_OK) && ::access(path.c_str(), W_OK))) {
        continue;
      }
      add(op_t::mknod, under(name), {}, (st.st_mode & 0006) == 0006 ? 0666 : 0600, st.st_rdev);
    }

    for (const auto &[name, target] : {std::pair {"fd", "/proc/self/fd"}, {"stdin", "/proc/self/fd/0"}, {"stdout", "/proc/self/fd/1"}, {"stderr", "/proc/self/fd/2"}}) {
      add(op_t::symlink, under(name), target);
    }
    if (fs::is_symlink("/dev/log", ec)) {
      add(op_t::symlink, under("log"), fs::read_symlink("/dev/log", ec).string());
    }

    // Live: GPUs (Hermes-KMS cards come and go), terminals, shared memory.
    std::vector<std::string> live {"dri", "pts", "shm", "mqueue", "hugepages"};
    if (!options.sound) {
      live.emplace_back("snd");
    }
    for (const auto &name : live) {
      if (fs::is_directory("/dev/" + name, ec)) {
        add(op_t::mkdir, under(name), {}, 0755);
        add(op_t::bind, under(name), "/dev/" + name);
      }
    }

    // Filled by mirror() as the session's devices appear.
    add(op_t::mkdir, under("input"), {}, 0755);
    add(op_t::move, "/dev", staging);
    return plan;
  }

  std::unique_ptr<plan_t> plan_t::join(pid_t pid) {
    auto plan = std::make_unique<plan_t>();
    plan->join_path = "/proc/" + std::to_string(pid) + "/ns/mnt";
    return plan;
  }

  void plan_t::apply() const noexcept {
    if (!set_caps(true)) {
      fail("capset");
    }

    if (!join_path.empty()) {
      const int fd = ::open(join_path.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0 || ::setns(fd, CLONE_NEWNS)) {
        fail(join_path.c_str());
      }
      ::close(fd);
    } else {
      if (::unshare(CLONE_NEWNS) || ::mount(nullptr, "/", nullptr, MS_REC | MS_SLAVE, nullptr)) {
        fail("unshare");
      }
      const mode_t mask = ::umask(0);
      for (const auto &op : ops) {
        int result = 0;
        switch (op.kind) {
          case op_t::tmpfs:
            result = ::mount("tmpfs", op.path.c_str(), "tmpfs", MS_NOSUID | MS_NOEXEC | op.flags, "mode=0755,size=1m");
            break;
          case op_t::mkdir:
            result = ::mkdir(op.path.c_str(), op.mode);
            break;
          case op_t::mknod:
            result = ::mknod(op.path.c_str(), S_IFCHR | op.mode, op.dev);
            break;
          case op_t::symlink:
            result = ::symlink(op.source.c_str(), op.path.c_str());
            break;
          case op_t::bind:
            result = ::mount(op.source.c_str(), op.path.c_str(), nullptr, MS_BIND | MS_REC, nullptr);
            break;
          case op_t::move:
            result = ::mount(op.source.c_str(), op.path.c_str(), nullptr, MS_MOVE, nullptr);
            break;
        }
        if (result) {
          fail(op.path.c_str());
        }
      }
      ::umask(mask);
    }

    if (!set_caps(false)) {
      fail("capset");
    }
  }

  bp::child run_command(const plan_t *plan, const std::string &cmd, boost::filesystem::path &working_dir, const bp::environment &env, FILE *file, std::error_code &ec, bp::group *group) {
    // First, so start_dir's chdir happens inside the joined namespace.
    auto setup = bp::extend::on_exec_setup([plan](auto &) {
      if (plan) {
        plan->apply();
      }
    });
    // clang-format off
    if (!group) {
      if (!file) {
        return bp::child(cmd, setup, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > bp::null, bp::std_err > bp::null, bp::limit_handles, ec);
      }
      return bp::child(cmd, setup, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > file, bp::std_err > file, bp::limit_handles, ec);
    }
    if (!file) {
      return bp::child(cmd, setup, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > bp::null, bp::std_err > bp::null, bp::limit_handles, ec, *group);
    }
    return bp::child(cmd, setup, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > file, bp::std_err > file, bp::limit_handles, ec, *group);
    // clang-format on
  }

  namespace {
    std::string read_line(const fs::path &path) {
      std::ifstream file {path};
      std::string line;
      std::getline(file, line);
      return line;
    }

    std::string uevent_value(const fs::path &uevent, std::string_view key) {
      std::ifstream file {uevent};
      for (std::string line; std::getline(file, line);) {
        if (line.size() > key.size() && line.starts_with(key) && line[key.size()] == '=') {
          return line.substr(key.size() + 1);
        }
      }
      return {};
    }

    struct device_t {
      std::string action;
      std::string subsystem;
      std::string devpath;
      std::string devname;
      unsigned major {};
      unsigned minor {};

      fs::path sysfs() const {
        return fs::path {"/sys"} / devpath.substr(1);
      }

      // What Hermes stamped the device with: its seat.
      std::string phys() const {
        if (subsystem == "hidraw") {
          return uevent_value(sysfs().parent_path().parent_path() / "uevent", "HID_PHYS");
        }
        return read_line(sysfs().parent_path() / "phys");
      }

      // Steam Input's pads: uinput devices with Valve's vendor id.
      bool valve_virtual_pad() const {
        return subsystem == "input" && devpath.starts_with("/devices/virtual/input/") &&
               read_line(sysfs().parent_path() / "phys").empty() &&
               read_line(sysfs().parent_path() / "id" / "vendor") == "28de";
      }
    };

    // Whether only processes in mount namespace ns hold uinput, Hermes aside:
    // a uinput device does not record who made it, and a Steam outside the
    // session (its virtual pads pass on the host's controllers) must not count.
    bool only_session_holds_uinput(ino_t ns) {
      std::error_code ec;
      const auto self = std::to_string(::getpid());
      bool session = false;
      for (const auto &proc : fs::directory_iterator {"/proc", ec}) {
        const auto pid = proc.path().filename().string();
        if (pid == self || !std::isdigit(static_cast<unsigned char>(pid[0]))) {
          continue;
        }
        std::error_code fd_ec;
        for (const auto &fd : fs::directory_iterator {proc.path() / "fd", fd_ec}) {
          std::error_code link_ec;
          if (fs::read_symlink(fd.path(), link_ec) != "/dev/uinput") {
            continue;
          }
          struct stat st {};
          if (::stat((proc.path() / "ns" / "mnt").c_str(), &st) || st.st_ino != ns) {
            return false;
          }
          session = true;
          break;
        }
      }
      return session;
    }

    // Capabilities are per thread: raise CAP_MKNOD around the call only.
    int mknod_as_hermes(int dirfd, const char *path, dev_t dev) {
      cap_t caps = cap_get_proc();
      cap_value_t mknod_cap = CAP_MKNOD;
      cap_set_flag(caps, CAP_EFFECTIVE, 1, &mknod_cap, CAP_SET);
      cap_set_proc(caps);
      int result = ::mknodat(dirfd, path, S_IFCHR | 0600, dev);
      if (result && errno == EEXIST) {
        ::unlinkat(dirfd, path, 0);
        result = ::mknodat(dirfd, path, S_IFCHR | 0600, dev);
      }
      const int error = errno;
      cap_set_flag(caps, CAP_EFFECTIVE, 1, &mknod_cap, CAP_CLEAR);
      cap_set_proc(caps);
      cap_free(caps);
      errno = error;
      return result;
    }

    class mirror_impl_t: public mirror_t {
    public:
      mirror_impl_t(int dev_fd, ino_t ns, std::string seat):
          dev_fd {dev_fd},
          ns {ns},
          seat {std::move(seat)} {
      }

      ~mirror_impl_t() override;

      bool owns(const device_t &device) const {
        return device.phys() == seat || (device.valve_virtual_pad() && only_session_holds_uinput(ns));
      }

      void add(const device_t &device) {
        if (mknod_as_hermes(dev_fd, device.devname.c_str(), makedev(device.major, device.minor))) {
          BOOST_LOG(warning) << "Couldn't give session seat ["sv << seat << "] /dev/"sv << device.devname << ": "sv << std::strerror(errno);
          return;
        }
        nodes.insert(device.devname);
        BOOST_LOG(debug) << "Session seat ["sv << seat << "] got /dev/"sv << device.devname;
      }

      void remove(const device_t &device) {
        if (nodes.erase(device.devname)) {
          ::unlinkat(dev_fd, device.devname.c_str(), 0);
        }
      }

      int dev_fd;
      ino_t ns;
      std::string seat;
      std::set<std::string> nodes;
    };

    std::mutex mirrors_mutex;
    std::vector<mirror_impl_t *> mirrors;

    mirror_impl_t::~mirror_impl_t() {
      std::lock_guard lock {mirrors_mutex};
      std::erase(mirrors, this);
      ::close(dev_fd);
    }

    // Kernel uevents arrive before udev has processed the device, and so
    // before any application in the session hears of it from udev.
    void monitor() {
      const int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
      sockaddr_nl address {};
      address.nl_family = AF_NETLINK;
      address.nl_groups = 1;
      if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address)) {
        BOOST_LOG(error) << "Couldn't listen to kernel uevents; isolated sessions get no input devices: "sv << std::strerror(errno);
        return;
      }

      char buffer[8192];
      while (true) {
        const auto size = ::recv(fd, buffer, sizeof buffer - 1, 0);
        if (size <= 0) {
          if (size < 0 && errno == ENOBUFS) {
            BOOST_LOG(warning) << "Kernel uevents were lost; a session may miss a device"sv;
          }
          continue;
        }
        buffer[size] = '\0';

        device_t device;
        for (const char *field = buffer; field < buffer + size; field += std::strlen(field) + 1) {
          const std::string_view entry {field};
          const auto equals = entry.find('=');
          if (equals == std::string_view::npos) {
            continue;
          }
          const auto key = entry.substr(0, equals);
          const auto value = std::string {entry.substr(equals + 1)};
          if (key == "ACTION") {
            device.action = value;
          } else if (key == "SUBSYSTEM") {
            device.subsystem = value;
          } else if (key == "DEVPATH") {
            device.devpath = value;
          } else if (key == "DEVNAME") {
            device.devname = value;
          } else if (key == "MAJOR") {
            device.major = std::stoul(value);
          } else if (key == "MINOR") {
            device.minor = std::stoul(value);
          }
        }
        if (device.devname.empty() || (device.subsystem != "input" && device.subsystem != "hidraw")) {
          continue;
        }

        std::lock_guard lock {mirrors_mutex};
        for (auto *mirror : mirrors) {
          if (device.action == "add" && mirror->owns(device)) {
            mirror->add(device);
          } else if (device.action == "remove") {
            mirror->remove(device);
          }
        }
      }
    }

    // Devices that already exist: from sysfs, as the kernel would announce them.
    void sync(mirror_impl_t &mirror) {
      for (const auto *klass : {"/sys/class/input", "/sys/class/hidraw"}) {
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator {klass, ec}) {
          device_t device;
          device.subsystem = fs::path {klass}.filename().string();
          device.devname = uevent_value(entry.path() / "uevent", "DEVNAME");
          unsigned major = 0;
          unsigned minor = 0;
          if (device.devname.empty() || std::sscanf(read_line(entry.path() / "dev").c_str(), "%u:%u", &major, &minor) != 2) {
            continue;
          }
          device.major = major;
          device.minor = minor;
          std::error_code canonical_ec;
          device.devpath = fs::canonical(entry.path(), canonical_ec).string().substr("/sys"sv.size());
          if (!canonical_ec && mirror.owns(device)) {
            mirror.add(device);
          }
        }
      }
    }
  }  // namespace

  std::unique_ptr<mirror_t> mirror(pid_t pid, const std::string &seat) {
    static std::once_flag started;
    std::call_once(started, [] {
      std::thread {monitor}.detach();
    });

    const auto proc = "/proc/" + std::to_string(pid);
    struct stat st {};
    const int dev_fd = ::open((proc + "/root/dev").c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dev_fd < 0 || ::stat((proc + "/ns/mnt").c_str(), &st)) {
      BOOST_LOG(error) << "Couldn't reach the /dev of session seat ["sv << seat << "]: "sv << std::strerror(errno);
      if (dev_fd >= 0) {
        ::close(dev_fd);
      }
      return nullptr;
    }

    auto mirror = std::make_unique<mirror_impl_t>(dev_fd, st.st_ino, seat);
    std::lock_guard lock {mirrors_mutex};
    sync(*mirror);
    mirrors.push_back(mirror.get());
    return mirror;
  }
}  // namespace platf::session_devices
