/**
 * @file linux_hid_rumble.cpp
 * @brief Fresh unprivileged SDL process used by VM-only input access tests.
 */
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <SDL.h>
#include <string_view>
#include <thread>
#include <unistd.h>

/**
 * @brief Verify per-user access and emit a real evdev force-feedback request.
 * @param argc Argument count.
 * @param argv Own event path followed by an inaccessible foreign-seat event path.
 * @return Zero only when access isolation and ordinary rumble both work.
 */
int main(int argc, char **argv) {
  if (!std::getenv("HERMES_TEST_KERNEL_INPUT") || ::geteuid() == 0 || argc != 3) {
    return 2;
  }
  const auto own = ::open(argv[1], O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (own < 0) {
    std::cerr << "own device open failed: " << std::strerror(errno) << '\n';
    return 1;
  }
  ::close(own);
  const auto foreign = ::open(argv[2], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (foreign >= 0) {
    ::close(foreign);
    std::cerr << "foreign-seat device was accessible\n";
    return 1;
  }
  if (errno != EACCES && errno != EPERM) {
    std::cerr << "foreign device was not present for access validation\n";
    return 1;
  }
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (SDL_Init(SDL_INIT_JOYSTICK) != 0) {
    std::cerr << SDL_GetError() << '\n';
    return 1;
  }
  SDL_Joystick *joystick = nullptr;
  for (int attempt = 0; attempt < 100 && !joystick; ++attempt) {
    SDL_PumpEvents();
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
      const auto *path = SDL_JoystickPathForIndex(index);
      if (path && std::string_view(path) == argv[1]) {
        joystick = SDL_JoystickOpen(index);
        break;
      }
    }
    if (!joystick) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  bool success = joystick && SDL_JoystickHasRumble(joystick) == SDL_TRUE;
  if (success) {
    success = SDL_JoystickRumble(joystick, 0x1234, 0x5678, 100) == 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  if (!success) {
    std::cerr << "unprivileged SDL rumble failed: " << SDL_GetError() << '\n';
  }
  if (joystick) {
    SDL_JoystickClose(joystick);
  }
  SDL_Quit();
  return success ? 0 : 1;
}
