/**
 * @file src/platform/macos/hid_gamepad_bridge.h
 * @brief C++ access to the IOHIDUserDevice gamepads, used when libvirtualhid cannot create one.
 */
#pragma once

// local includes
#include "src/platform/common.h"

namespace platf::hid_gamepad_bridge {
  /**
   * @brief Create an IOHIDUserDevice gamepad for a global gamepad index.
   * @param index Global gamepad index.
   * @return Whether the device was created; it fails while SIP is enabled.
   */
  bool alloc(int index);

  /**
   * @brief Whether the global gamepad index is served by an IOHIDUserDevice gamepad.
   * @param index Global gamepad index.
   * @return Whether this bridge owns the index.
   */
  bool owns(int index);

  /**
   * @brief Disconnect the IOHIDUserDevice gamepad for a global gamepad index.
   * @param index Global gamepad index.
   */
  void free(int index);

  /**
   * @brief Send a state report to the IOHIDUserDevice gamepad.
   * @param index Global gamepad index.
   * @param state Gamepad state from the client.
   */
  void update(int index, const gamepad_state_t &state);
}  // namespace platf::hid_gamepad_bridge
