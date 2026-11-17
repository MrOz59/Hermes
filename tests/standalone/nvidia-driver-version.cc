// A GPU-free regression for naming the loaded NVIDIA kernel module in logs.
// Compile with -std=c++17 -I. and run directly (also exercised by branch CI).
#include "src/platform/linux/nvidia_driver_version.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

namespace {

  void expect(const char *line, unsigned int major, unsigned int minor, unsigned int patch, bool open, const char *text) {
    const auto version = platf::nvidia::parse_version_line(line);
    if (!version || version->major != major || version->minor != minor || version->patch != patch ||
        version->open != open || version->text != text) {
      std::fprintf(stderr, "FAIL: %s\n", line);
      std::exit(1);
    }
  }

  void reject(const char *line) {
    if (platf::nvidia::parse_version_line(line)) {
      std::fprintf(stderr, "FAIL: accepted %s\n", line);
      std::exit(1);
    }
  }

}  // namespace

int main() {
  using namespace platf::nvidia;

  expect("NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  610.43.03  Release Build  "
         "(dvs-builder@U22-I3-AM21-27-1)  Thu Aug 14 01:00:00 UTC 2026\n",
         610, 43, 3, true, "610.43.03");
  expect("NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.178.04  Tue Sep 23 2026\n", 580, 178, 4, false, "580.178.04");
  expect("NVRM version: NVIDIA UNIX Open Kernel Module for aarch64  595.91.07  Release Build", 595, 91, 7, true, "595.91.07");
  expect("NVRM version: NVIDIA UNIX x86_64 Kernel Module  560.35  Fri Aug 16 2024", 560, 35, 0, false, "560.35");
  expect("NVRM version:\tNVIDIA UNIX Open Kernel Module for x86_64\t615.71.09", 615, 71, 9, true, "615.71.09");

  reject("");
  reject("GCC version:  gcc version 15.2.1 20250813 (GCC)\n");
  reject("NVRM version: NVIDIA UNIX x86_64 Kernel Module\n");
  reject("NVRM version: NVIDIA UNIX x86_64 Kernel Module  610.43.03.1  build\n");
  reject("NVRM version: NVIDIA UNIX x86_64 Kernel Module  610.  build\n");
  reject("NVRM version: NVIDIA UNIX x86_64 Kernel Module  .43  build\n");
  reject("NVRM version: NVIDIA UNIX x86_64 Kernel Module  99999999999.1  build\n");

  assert(describe(std::nullopt).rfind("not loaded", 0) == 0);
  assert(describe(parse_version_line("NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  610.57.04")) ==
         "610.57.04 (open kernel module)");
  assert(describe(parse_version_line("NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.178.04")) ==
         "580.178.04 (proprietary kernel module)");

  // The file reader: a missing file is "not loaded", not an error.
  char path[] = "/tmp/hermes-nvidia-version.XXXXXX";
  const int fd = ::mkstemp(path);
  assert(fd >= 0);
  ::close(fd);
  {
    std::ofstream file {path};
    file << "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  610.57.04  Release Build\n"
         << "GCC version:  gcc version 15.2.1 (GCC)\n";
  }
  const auto loaded = loaded_driver_version(path);
  assert(loaded && loaded->major == 610 && loaded->minor == 57 && loaded->patch == 4 && loaded->open);
  ::unlink(path);
  assert(!loaded_driver_version(path));

  std::puts("NVIDIA driver version: parsing, description and file reading PASS");
  return 0;
}
