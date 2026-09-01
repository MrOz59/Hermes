/**
 * @file src/platform/linux/card_broker.h
 * @brief Ask the privileged broker for a Hermes-KMS card.
 */
#pragma once

#include <optional>
#include <string>

#include <sys/types.h>

namespace VDISPLAY::card_broker {

  /**
   * @brief A card the broker created, and the device nodes it was given.
   *
   * The paths come from the driver rather than from udev, so they are known
   * before any rule has run. The render node is not necessarily *openable*
   * yet: `92-hermes-kms-access.rules` turns the card's `access_uid` into
   * ownership of that node, and that happens on udev's schedule.
   */
  struct card_t {
    std::string name;  ///< configfs group name, needed to remove it again.
    std::string card_path;  ///< /dev/dri/cardN, root-only and reached through the seat broker.
    std::string render_node_path;  ///< /dev/dri/renderDN, owned by this user once udev has run.
  };

  /**
   * @brief Whether a broker socket is there to talk to.
   *
   * False on every host that has not installed and enabled the broker, which
   * is the normal case; callers fall back to the statically configured pool.
   */
  bool available();

  /**
   * @brief Ask for a new card on a private DRM seat.
   *
   * The broker picks the session index, because that index is what the driver's
   * udev rules turn into a seat and its seatd instance: two cards sharing one
   * would share a seat. Fails when the caller's uid is not in the broker's
   * allow file, when it already holds as many cards as it may, or when every
   * private seat is in use.
   *
   * @param session_account_uid When set, the card belongs to that uid - a
   *        Hermes session account, which the broker verifies - instead of to
   *        the caller. An isolated session's compositor runs as hermes-sNN,
   *        and the driver's udev rules hand the card's device nodes to its
   *        access_uid, so a card made for the host user is one the session
   *        could never open.
   */
  std::optional<card_t> create(std::optional<uid_t> session_account_uid = std::nullopt);

  /**
   * @brief Remove a card this user created. Removal is a hot-unplug, so a
   *        compositor still holding the card sees ENODEV rather than a device
   *        disappearing under it.
   */
  bool remove(const std::string &name);

  /**
   * @brief Remove every card this user still holds, and report how many went.
   *
   * Cards outlive the process that asked for one, so a Hermes that died
   * without removing its own would strand a card and its private seat. Sweeping
   * at startup - when this Hermes owns none by definition - is what collects
   * them.
   */
  int sweep();

}  // namespace VDISPLAY::card_broker
