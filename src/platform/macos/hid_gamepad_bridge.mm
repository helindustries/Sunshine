/**
 * @file src/platform/macos/hid_gamepad_bridge.mm
 * @brief Definitions for C++ access to the IOHIDUserDevice gamepads.
 */
// standard includes
#include <array>
#include <mutex>

// local includes
#include "hid_gamepad_bridge.h"
#include "src/logging.h"
#import "src/platform/macos/hid_gamepad.h"

using namespace std::literals;

namespace platf::hid_gamepad_bridge {
  namespace {
    constexpr auto max_gamepads = 16;

    std::mutex gamepads_mutex;
    std::array<HIDGamepad *, max_gamepads> gamepads {};

    bool valid(int index) {
      return index >= 0 && index < max_gamepads;
    }
  }  // namespace

  bool alloc(int index) {
    auto created = false;
    if (valid(index)) {
      std::lock_guard lock {gamepads_mutex};
      if (gamepads[index] == nil) {
        HIDGamepad *gamepad = [[HIDGamepad alloc] initWithIndex:index];
        if ([gamepad createDevice]) {
          gamepads[index] = gamepad;
          created = true;
          BOOST_LOG(info) << "Gamepad "sv << index << " allocated as IOHIDUserDevice"sv;
        } else {
          [gamepad release];
          BOOST_LOG(warning) << "Gamepad "sv << index << ": IOHIDUserDevice creation failed (SIP enabled?)"sv;
        }
      }
    }
    return created;
  }

  bool owns(int index) {
    std::lock_guard lock {gamepads_mutex};
    return valid(index) && gamepads[index] != nil;
  }

  void free(int index) {
    std::lock_guard lock {gamepads_mutex};
    if (valid(index) && gamepads[index] != nil) {
      [gamepads[index] disconnect];
      [gamepads[index] release];
      gamepads[index] = nil;
    }
  }

  void update(int index, const gamepad_state_t &state) {
    std::lock_guard lock {gamepads_mutex};
    if (valid(index) && gamepads[index] != nil) {
      [gamepads[index] updateState:state.buttonFlags
                        leftStickX:state.lsX
                        leftStickY:state.lsY
                       rightStickX:state.rsX
                       rightStickY:state.rsY
                       leftTrigger:state.lt
                      rightTrigger:state.rt];
    }
  }
}  // namespace platf::hid_gamepad_bridge
