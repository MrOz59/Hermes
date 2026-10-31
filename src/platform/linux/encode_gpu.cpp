/**
 * @file src/platform/linux/encode_gpu.cpp
 * @brief Choosing the GPU a stream is encoded on.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

// platform includes
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>

// local includes
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/utility.h"
#include "vaapi.h"

using namespace std::literals;

namespace platf {
  namespace {
    using version_t = util::safe_ptr<drmVersion, drmFreeVersion>;

    std::string driver_name(int fd) {
      version_t version {drmGetVersion(fd)};
      if (!version || !version->name) {
        return {};
      }
      return {version->name, static_cast<std::size_t>(version->name_len)};
    }

    /**
     * Video memory, where the driver reports it (amdgpu does). A dedicated
     * card has far more than an integrated GPU's carve-out, which is what
     * makes it the tie-breaker between two GPUs that encode the same codecs.
     */
    std::uint64_t video_memory(int fd) {
      struct stat node {};

      if (::fstat(fd, &node) != 0 || !S_ISCHR(node.st_mode)) {
        return 0;
      }
      std::ifstream file {std::format("/sys/dev/char/{}:{}/device/mem_info_vram_total", major(node.st_rdev), minor(node.st_rdev))};
      std::uint64_t bytes = 0;
      file >> bytes;
      return file ? bytes : 0;
    }

    std::string capability_names(int capability) {
      std::string names;
      for (const auto &[bit, name] : std::array<std::pair<int, std::string_view>, 4> {{
             {8, "AV1"sv},
             {4, "HEVC Main10"sv},
             {2, "HEVC"sv},
             {1, "H.264"sv},
           }}) {
        if (capability & bit) {
          names += (names.empty() ? ""s : ", "s) + std::string {name};
        }
      }
      return names.empty() ? "no VAAPI encoding"s : names;
    }
  }  // namespace

  int open_encode_render_node(bool nvenc) {
    if (!config::video.adapter_name.empty()) {
      const auto &node = config::video.adapter_name;
      const int fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
      if (fd < 0) {
        BOOST_LOG(error) << "Could not open the configured adapter "sv << node << ": "sv << std::strerror(errno);
        return -1;
      }
      if (driver_name(fd) == "hermes-kms"sv) {
        BOOST_LOG(error) << "The configured adapter "sv << node
                         << " is the capture-only Hermes-KMS render node, not a GPU that can encode."sv;
        ::close(fd);
        return -1;
      }
      return fd;
    }

    std::array<drmDevicePtr, DRM_MAX_MINOR> devices {};
    const int n = drmGetDevices2(0, devices.data(), static_cast<int>(devices.size()));
    if (n <= 0) {
      return -1;
    }
    const int device_count = std::min(n, static_cast<int>(devices.size()));
    auto free_devices = util::fail_guard([&]() {
      drmFreeDevices(devices.data(), device_count);
    });

    struct choice_t {
      int fd = -1;
      std::string node;
      std::string driver;
      bool nvidia = false;
      int capability = 0;
      std::uint64_t memory = 0;
    } best;

    // Better is: the vendor NVENC needs, then more codecs, then more memory.
    const auto better = [nvenc](const choice_t &a, const choice_t &b) {
      if (nvenc && a.nvidia != b.nvidia) {
        return a.nvidia;
      }
      if (a.capability != b.capability) {
        return a.capability > b.capability;
      }
      return a.memory > b.memory;
    };

    for (int i = 0; i < device_count; ++i) {
      if (!devices[i] || !(devices[i]->available_nodes & (1U << DRM_NODE_RENDER)) ||
          !devices[i]->nodes[DRM_NODE_RENDER]) {
        continue;
      }
      choice_t candidate;
      candidate.node = devices[i]->nodes[DRM_NODE_RENDER];
      candidate.fd = ::open(candidate.node.c_str(), O_RDWR | O_CLOEXEC);
      if (candidate.fd < 0) {
        continue;
      }
      candidate.driver = driver_name(candidate.fd);
      if (candidate.driver.empty() || candidate.driver == "hermes-kms"sv) {
        ::close(candidate.fd);
        continue;
      }
      candidate.nvidia = candidate.driver == "nvidia-drm"sv;
#ifdef SUNSHINE_BUILD_VAAPI
      candidate.capability = va::encode_capability(candidate.fd);
#endif
      candidate.memory = video_memory(candidate.fd);
      BOOST_LOG(debug) << "Encoding GPU candidate "sv << candidate.node << " ("sv << candidate.driver
                       << "): "sv << capability_names(candidate.capability) << ", "sv
                       << candidate.memory / (1024 * 1024) << " MiB video memory"sv;

      if (best.fd < 0 || better(candidate, best)) {
        if (best.fd >= 0) {
          ::close(best.fd);
        }
        best = std::move(candidate);
      } else {
        ::close(candidate.fd);
      }
    }

    if (best.fd >= 0) {
      // Every encoder check opens a device, so the choice is reported when it
      // changes rather than each time it is made.
      static std::mutex reported_mutex;
      static std::string reported;
      std::lock_guard lock {reported_mutex};
      if (reported != best.node) {
        reported = best.node;
        BOOST_LOG(info) << "Encoding on "sv << best.node << " ("sv << best.driver << "): "sv
                        << capability_names(best.capability);
      }
    }
    return best.fd;
  }

  namespace {
    /**
     * The name a GPU is sold under, where the system knows it: libdrm's table
     * of AMD names by device and revision, or the NVIDIA driver's model line.
     * Empty when neither says, as for Intel.
     */
    std::string gpu_marketing_name(const std::string &sysfs, const std::string &vendor, const std::string &device, const std::string &revision) {
      const auto upper = [](std::string text) {
        std::ranges::transform(text, text.begin(), [](unsigned char c) {
          return static_cast<char>(std::toupper(c));
        });
        return text;
      };
      if (vendor == "1002") {
        // "7550,\tC0,\tAMD Radeon RX 9070 XT": one line per device and revision.
        for (const auto *path : {"/usr/share/libdrm/amdgpu.ids", "/usr/local/share/libdrm/amdgpu.ids"}) {
          std::ifstream ids {path};
          for (std::string line; std::getline(ids, line);) {
            if (line.empty() || line.front() == '#') {
              continue;
            }
            const auto first = line.find(',');
            const auto second = first == std::string::npos ? first : line.find(',', first + 1);
            if (second == std::string::npos) {
              continue;
            }
            const auto trim = [](std::string text) {
              const auto begin = text.find_first_not_of(" \t");
              const auto end = text.find_last_not_of(" \t\r");
              return begin == std::string::npos ? std::string {} : text.substr(begin, end - begin + 1);
            };
            if (upper(trim(line.substr(0, first))) == upper(device) &&
                upper(trim(line.substr(first + 1, second - first - 1))) == upper(revision)) {
              return trim(line.substr(second + 1));
            }
          }
        }
      } else if (vendor == "10de") {
        // /proc/driver/nvidia/gpus/<PCI address>/information: "Model: \t NVIDIA ...".
        std::error_code ec;
        const auto address = std::filesystem::canonical(sysfs, ec).filename().string();
        std::ifstream information {"/proc/driver/nvidia/gpus/" + address + "/information"};
        for (std::string line; std::getline(information, line);) {
          if (line.starts_with("Model:")) {
            const auto begin = line.find_first_not_of(" \t", 6);
            return begin == std::string::npos ? std::string {} : line.substr(begin);
          }
        }
      }
      return {};
    }
  }  // namespace

  std::string encode_gpu_description(bool nvenc) {
    const int fd = open_encode_render_node(nvenc);
    if (fd < 0) {
      return {};
    }
    auto close_fd = util::fail_guard([fd]() {
      ::close(fd);
    });
    struct stat st {};
    if (::fstat(fd, &st) || !S_ISCHR(st.st_mode)) {
      return {};
    }
    const auto sysfs = std::format("/sys/dev/char/{}:{}/device/", major(st.st_rdev), minor(st.st_rdev));
    const auto read_id = [&](const char *file) {
      std::string value;
      std::ifstream {sysfs + file} >> value;
      return value.starts_with("0x") ? value.substr(2) : value;
    };
    const auto vendor = read_id("vendor");
    const auto device = read_id("device");
    std::error_code ec;
    const auto node = std::filesystem::read_symlink(std::format("/proc/self/fd/{}", fd), ec).string();
    auto name = gpu_marketing_name(sysfs, vendor, device, read_id("revision"));
    if (name.empty()) {
      const auto vendor_name = vendor == "1002" ? "AMD"s : vendor == "10de" ? "NVIDIA"s : vendor == "8086" ? "Intel"s : "PCI"s;
      name = std::format("{} {}:{}", vendor_name, vendor, device);
    }
    return std::format("{} ({}, {})", node, driver_name(fd), name);
  }
}  // namespace platf
