// SPDX-License-Identifier: GPL-3.0-or-later
// Create two minimal uinput keyboards with distinct Hermes session phys tags.
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;

static void stop_handler(int signal_number) {
  (void) signal_number;
  stopping = 1;
}

static int create_keyboard(const char *phys) {
  struct uinput_setup setup = {
    .id = {
      .bustype = BUS_USB,
      .vendor = 0xbeef,
      .product = 0xdead,
      .version = 1,
    },
  };
  char sysname[UINPUT_MAX_NAME_SIZE] = {};
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    perror("open /dev/uinput");
    return -1;
  }

  snprintf(setup.name, sizeof(setup.name), "Hermes seat probe %s", phys);
  if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
      ioctl(fd, UI_SET_KEYBIT, KEY_A) < 0 ||
      ioctl(fd, UI_SET_PHYS, phys) < 0 ||
      ioctl(fd, UI_DEV_SETUP, &setup) < 0 ||
      ioctl(fd, UI_DEV_CREATE) < 0) {
    perror("configure uinput device");
    close(fd);
    return -1;
  }

  if (ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) {
    perror("query uinput sysname");
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return -1;
  }

  printf("%s=%s\n", phys, sysname);
  fflush(stdout);
  return fd;
}

int main(void) {
  int keyboards[2] = {-1, -1};
  const char *phys[2] = {"hermes-kms-1", "hermes-kms-2"};

  signal(SIGINT, stop_handler);
  signal(SIGTERM, stop_handler);
  for (size_t index = 0; index < 2; ++index) {
    keyboards[index] = create_keyboard(phys[index]);
    if (keyboards[index] < 0) {
      stopping = 1;
      break;
    }
  }

  while (!stopping) {
    pause();
  }
  for (size_t index = 0; index < 2; ++index) {
    if (keyboards[index] >= 0) {
      ioctl(keyboards[index], UI_DEV_DESTROY);
      close(keyboards[index]);
    }
  }
  return keyboards[0] >= 0 && keyboards[1] >= 0 ? 0 : 1;
}
