/** @file tools/hermes-kwin-transport-check.cpp
 *  @brief Exercise Hermes' native transport against an explicit test session.
 */
#include "kwin_session.h"
#include "pipewire_session.h"

#include <chrono>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "Usage: hermes-kwin-transport-check ABSOLUTE_WAYLAND_SOCKET ABSOLUTE_PIPEWIRE_SOCKET\n";
    return 2;
  }
  // This executable is a diagnostic, not a route selector. Refuse host sockets
  // even when someone runs it by hand in their regular desktop environment.
  const std::string socket = argv[1];
  if (!socket.starts_with("/run/hermes-plasma-probe-") || socket.find("/../") != std::string::npos) {
    std::cerr << "Refusing to capture outside a temporary Hermes Plasma probe\n";
    return 2;
  }
  std::string error;
  auto connection = platf::kwin::connection_t::open({socket, argv[2], {}}, true, true, error);
  if (!connection) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto &output = connection->output();
  std::cout << "NATIVE OUTPUT: " << output.name << ' ' << output.width << 'x' << output.height << '\n';
  std::uint64_t serial = 0;
  if (!connection->start_capture(true, serial, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "NATIVE PIPEWIRE SERIAL: " << serial << '\n';
  auto receiver = platf::kwin::video_receiver_t::open(argv[2], serial, output.width, output.height, 60, error);
  if (!receiver) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!connection->dispatch(std::chrono::milliseconds(0))) {
      std::cerr << connection->error() << '\n';
      return 1;
    }
    platf::kwin::frame_t frame;
    const auto result = receiver->next(frame, std::chrono::milliseconds(50), error);
    if (result == platf::kwin::frame_result_t::failed) {
      std::cerr << error << '\n';
      return 1;
    }
    if (result == platf::kwin::frame_result_t::ready) {
      // Recheck the compositor after receiving a frame: a queued frame alone
      // must not be evidence that the owning compositor is still alive.
      if (!connection->dispatch(std::chrono::milliseconds(0))) {
        std::cerr << connection->error() << '\n';
        return 1;
      }
      std::uint32_t checksum = 2166136261u;
      for (auto byte : frame.bgrx) {
        checksum = (checksum ^ byte) * 16777619u;
      }
      std::cout << "NATIVE FRAME PASS: " << frame.width << 'x' << frame.height
                << " bytes=" << frame.bgrx.size() << " checksum=" << checksum << '\n';
      return 0;
    }
  }
  std::cerr << "Native capture timed out\n";
  return 1;
}
