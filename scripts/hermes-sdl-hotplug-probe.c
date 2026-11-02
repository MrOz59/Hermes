// SPDX-License-Identifier: GPL-3.0-only
// Observe a controller added after SDL initialization under a session UID.
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

static int controller_seat(int index, char *seat, size_t length) {
  if (SDL_JoystickGetDeviceVendor(index) != 0xbeef || SDL_JoystickGetDeviceProduct(index) != 0xdead) {
    return 0;
  }
  const char *path = SDL_JoystickPathForIndex(index);
  const char *name = path ? strrchr(path, '/') : NULL;
  char sysfs[256];
  if (!name || strncmp(name + 1, "event", 5) != 0 || snprintf(sysfs, sizeof(sysfs), "/sys/class/input/%s/device/phys", name + 1) >= (int) sizeof(sysfs)) {
    fprintf(stderr, "test controller has no evdev path\n");
    return -1;
  }
  FILE *stream = fopen(sysfs, "r");
  if (!stream) {
    perror("open controller phys");
    return -1;
  }
  const int read = fgets(seat, (int) length, stream) != NULL;
  fclose(stream);
  if (!read) {
    return -1;
  }
  seat[strcspn(seat, "\n")] = '\0';
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    return 2;
  }
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
  SDL_LogSetPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_DEBUG);
  if (SDL_Init(SDL_INIT_JOYSTICK) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  for (int index = 0; index < SDL_NumJoysticks(); ++index) {
    char seat[128];
    if (controller_seat(index, seat, sizeof(seat)) != 0) {
      fprintf(stderr, "test controllers must be created after SDL initialization\n");
      SDL_Quit();
      return 1;
    }
  }
  printf("ready\n");
  fflush(stdout);
  const Uint32 started = SDL_GetTicks();
  SDL_JoystickID last_seen = -1;
  int result = 1;
  while (SDL_GetTicks() - started < 15000) {
    SDL_bool own_attached = SDL_FALSE;
    SDL_JoystickUpdate();
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
      const char *name = SDL_JoystickNameForIndex(index);
      char seat[128];
      const int found = controller_seat(index, seat, sizeof(seat));
      if (found < 0) {
        goto done;
      }
      if (!found) {
        continue;
      }
      const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(index);
      if (instance > last_seen) {
        fprintf(stderr, "SDL discovered %s (%s)\n", name, SDL_JoystickPathForIndex(index));
        last_seen = instance;
      }
      if (strcmp(seat, argv[2]) == 0) {
        fprintf(stderr, "SDL enumerated the foreign seat controller\n");
        goto done;
      }
      if (strcmp(seat, argv[1]) == 0) {
        SDL_Joystick *device = SDL_JoystickOpen(index);
        if (!device) {
          continue;
        }
        const SDL_bool attached = SDL_JoystickGetAttached(device);
        SDL_JoystickClose(device);
        if (attached) {
          own_attached = SDL_TRUE;
        }
      }
    }
    if (own_attached) {
      printf("PASS: controller discovered after SDL initialization\n");
      result = 0;
      goto done;
    }
    SDL_Delay(20);
  }
  fprintf(stderr, "SDL did not discover its readable hotplugged controller\n");
  // Keep the failure, but distinguish a missed add event from classification
  // or naming errors by recording what a fresh SDL scan sees afterwards.
  SDL_Quit();
  if (SDL_Init(SDL_INIT_JOYSTICK) == 0) {
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
      fprintf(stderr, "fresh scan: %s (%s)\n", SDL_JoystickNameForIndex(index), SDL_JoystickPathForIndex(index));
    }
  }
done:
  SDL_Quit();
  return result;
}
