// SPDX-License-Identifier: GPL-3.0-only
// Two generic UHID gamepads for the disposable broker account/ACL test.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uhid.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;

static void stop_handler(int number) {
  (void) number;
  stopping = 1;
}

static int create_gamepad(const char *seat) {
  static const unsigned char descriptor[] = {
    0x05,
    0x01,
    0x09,
    0x05,
    0xa1,
    0x01,  // Generic Desktop / Game Pad
    0x05,
    0x09,
    0x19,
    0x01,
    0x29,
    0x08,  // Eight buttons
    0x15,
    0x00,
    0x25,
    0x01,
    0x75,
    0x01,
    0x95,
    0x08,
    0x81,
    0x02,
    0x05,
    0x01,
    0x09,
    0x30,
    0x09,
    0x31,  // X/Y axes
    0x15,
    0x81,
    0x25,
    0x7f,
    0x75,
    0x08,
    0x95,
    0x02,
    0x81,
    0x02,
    0xc0,
  };
  const int fd = open("/dev/uhid", O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    perror("open /dev/uhid");
    return -1;
  }
  struct uhid_event event = {.type = UHID_CREATE2};
  snprintf((char *) event.u.create2.name, sizeof(event.u.create2.name), "Hermes broker gamepad %s", seat);
  snprintf((char *) event.u.create2.phys, sizeof(event.u.create2.phys), "%s", seat);
  snprintf((char *) event.u.create2.uniq, sizeof(event.u.create2.uniq), "%s-probe", seat);
  event.u.create2.bus = BUS_USB;
  event.u.create2.vendor = 0xbeef;
  event.u.create2.product = 0xdead;
  event.u.create2.rd_size = sizeof(descriptor);
  memcpy(event.u.create2.rd_data, descriptor, sizeof(descriptor));
  ssize_t count;
  do {
    count = write(fd, &event, sizeof(event));
  } while (count < 0 && errno == EINTR);
  if (count != sizeof(event)) {
    perror("create UHID gamepad");
    close(fd);
    return -1;
  }
  return fd;
}

int main(void) {
  const char *seats[2] = {"hermes-kms-1", "hermes-kms-2"};
  struct pollfd devices[2] = {{.fd = -1, .events = POLLIN}, {.fd = -1, .events = POLLIN}};
  int started[2] = {0, 0};
  int result = 1;
  signal(SIGINT, stop_handler);
  signal(SIGTERM, stop_handler);
  for (int index = 0; index < 2; ++index) {
    devices[index].fd = create_gamepad(seats[index]);
    if (devices[index].fd < 0) {
      goto cleanup;
    }
  }
  for (int attempt = 0; !stopping; ++attempt) {
    if (attempt > 100 && (!started[0] || !started[1])) {
      fprintf(stderr, "timed out waiting for UHID_START\n");
      goto cleanup;
    }
    const int ready = poll(devices, 2, 50);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready < 0) {
      perror("poll UHID");
      goto cleanup;
    }
    for (int index = 0; index < 2; ++index) {
      if (devices[index].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        goto cleanup;
      }
      if (devices[index].revents & POLLIN) {
        struct uhid_event event;
        const ssize_t count = read(devices[index].fd, &event, sizeof(event));
        if (count >= (ssize_t) sizeof(event.type) && event.type == UHID_START) {
          started[index] = 1;
          printf("%s=started\n", seats[index]);
          fflush(stdout);
        }
      }
    }
  }
  result = started[0] && started[1] ? 0 : 1;
cleanup:
  for (int index = 0; index < 2; ++index) {
    if (devices[index].fd >= 0) {
      close(devices[index].fd);
    }
  }
  return result;
}
