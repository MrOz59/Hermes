// Build the production script for behavioural tests without KWin or a GPU.
#include "src/platform/linux/window_placement.h"
#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
  const std::string_view mode = argc > 1 ? argv[1] : "gamescope";
  const std::string output = argc > 2 ? argv[2] : "Virtual-1";
  const std::vector<std::string> classes {mode == "steam" ? "steam" : "gamescope"};
  std::vector<std::uint32_t> pids;
  if (mode == "gamescope") {
    pids = {42, 43};
  } else if (mode == "invalid-pids") {
    pids = {0, 1};
  }
  std::cout << platf::window_placement::kwin_script(output, classes, pids);
}
