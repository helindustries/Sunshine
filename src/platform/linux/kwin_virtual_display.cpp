/**
 * @file src/platform/linux/kwin_virtual_display.cpp
 * @brief Virtual displays on KWin, one per streaming session, through zkde_screencast's virtual outputs.
 * @details KWin keeps a virtual output only while the screencast stream that requested it is open,
 *          so this file holds those streams on its own Wayland connection; the session then
 *          captures the new output by name like any other KWin output.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

// lib includes
#include <poll.h>
#include <wayland-client.h>

// generated includes
#include <zkde-screencast-unstable-v1.h>

// local includes
#include "misc.h"
#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace platf {
  namespace {
    constexpr std::uint32_t virtual_output_since = 2;  ///< zkde_screencast version that added stream_virtual_output.
    constexpr auto stream_timeout = 5s;  ///< Time KWin gets to create a virtual output.

    /**
     * @brief Outcome of one stream_virtual_output request, filled by the stream listener.
     */
    struct stream_state_t {
      bool done = false;  ///< KWin answered with created or failed.
      bool created = false;  ///< The virtual output exists.
      std::string error;  ///< KWin's reason when it failed.
    };

    std::mutex kwin_mutex;
    wl_display *kwin_wl_display = nullptr;
    zkde_screencast_unstable_v1 *kwin_screencast = nullptr;
    std::uint32_t kwin_screencast_version = 0;
    std::map<std::string, zkde_screencast_stream_unstable_v1 *> kwin_virtual_outputs;  ///< Open streams by output name.
    int kwin_virtual_output_count = 0;

    void on_global(void *data, wl_registry *registry, std::uint32_t name, const char *interface, std::uint32_t version) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      if (std::string_view {interface} == zkde_screencast_unstable_v1_interface.name && version >= virtual_output_since) {
        kwin_screencast_version = std::min(version, static_cast<std::uint32_t>(zkde_screencast_unstable_v1_interface.version));
        kwin_screencast = static_cast<zkde_screencast_unstable_v1 *>(wl_registry_bind(registry, name, &zkde_screencast_unstable_v1_interface, kwin_screencast_version));
      }
    }

    void on_global_remove(void *data, wl_registry *registry, std::uint32_t name) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
    }

    constexpr wl_registry_listener registry_listener = {
      .global = on_global,
      .global_remove = on_global_remove,
    };

    void on_stream_closed(void *data, zkde_screencast_stream_unstable_v1 *stream) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      BOOST_LOG(warning) << "[kwin] a virtual output's stream was closed by KWin"sv;
    }

    void on_stream_created(void *data, zkde_screencast_stream_unstable_v1 *stream, std::uint32_t node) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      if (auto *state = static_cast<stream_state_t *>(data); state != nullptr) {
        state->done = true;
        state->created = true;
      }
    }

    void on_stream_failed(void *data, zkde_screencast_stream_unstable_v1 *stream, const char *error) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      if (auto *state = static_cast<stream_state_t *>(data); state != nullptr) {
        state->done = true;
        state->error = error != nullptr ? error : "unknown error";
      }
    }

    // From version 6 serial replaces the deprecated created event
    void on_stream_serial(void *data, zkde_screencast_stream_unstable_v1 *stream, std::uint32_t serial_high, std::uint32_t serial_low) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      if (auto *state = static_cast<stream_state_t *>(data); state != nullptr) {
        state->done = true;
        state->created = true;
      }
    }

    constexpr zkde_screencast_stream_unstable_v1_listener stream_listener = {
      .closed = on_stream_closed,
      .created = on_stream_created,
      .failed = on_stream_failed,
      .serial = on_stream_serial,
    };

    /**
     * @brief Connect to the session's KWin once and bind a screencast global that can make virtual outputs.
     * @return Whether one is bound.
     */
    bool connect_kwin() {
      if (kwin_wl_display == nullptr) {
        kwin_screencast_permission_setup();
        if (kwin_wl_display = wl_display_connect(nullptr); kwin_wl_display == nullptr) {
          BOOST_LOG(debug) << "[kwin] no Wayland display to create a virtual output on"sv;
        } else {
          auto *registry = wl_display_get_registry(kwin_wl_display);
          wl_registry_add_listener(registry, &registry_listener, nullptr);
          wl_display_roundtrip(kwin_wl_display);
          wl_registry_destroy(registry);
          if (kwin_screencast == nullptr) {
            BOOST_LOG(debug) << "[kwin] the compositor offers no zkde_screencast_unstable_v1 version "sv << virtual_output_since << " or newer"sv;
            wl_display_disconnect(kwin_wl_display);
            kwin_wl_display = nullptr;
          }
        }
      }
      return kwin_screencast != nullptr;
    }

    /**
     * @brief Dispatch KWin's events until the state is decided or the timeout passes.
     */
    void wait_for(const stream_state_t &state) {
      const auto deadline = std::chrono::steady_clock::now() + stream_timeout;
      while (!state.done && std::chrono::steady_clock::now() < deadline) {
        wl_display_flush(kwin_wl_display);
        pollfd pfd {wl_display_get_fd(kwin_wl_display), POLLIN, 0};
        if (wl_display_prepare_read(kwin_wl_display) == 0) {
          if (poll(&pfd, 1, 100) > 0) {
            wl_display_read_events(kwin_wl_display);
          } else {
            wl_display_cancel_read(kwin_wl_display);
          }
        }
        wl_display_dispatch_pending(kwin_wl_display);
      }
    }
  }  // namespace

  bool kwin_virtual_display_available() {
    std::lock_guard lock {kwin_mutex};
    return connect_kwin();
  }

  std::string kwin_virtual_display_create(int width, int height, int fps) {
    std::string output_name;
    std::lock_guard lock {kwin_mutex};

    if (connect_kwin()) {
      // KWin names the output after the request, prefixed with "Virtual-"; fps has no field in the request
      const auto name {"Sunshine-"s + std::to_string(++kwin_virtual_output_count)};
      stream_state_t state;
      auto *stream = zkde_screencast_unstable_v1_stream_virtual_output(kwin_screencast, name.c_str(), width, height, wl_fixed_from_int(1), ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
      zkde_screencast_stream_unstable_v1_add_listener(stream, &stream_listener, &state);
      wait_for(state);
      // The listener's state lives on this stack frame; later events must not reach it
      zkde_screencast_stream_unstable_v1_set_user_data(stream, nullptr);

      if (state.created) {
        output_name = "Virtual-"s + name;
        kwin_virtual_outputs.emplace(output_name, stream);
        BOOST_LOG(info) << "[kwin] created virtual output "sv << output_name << " ("sv << width << 'x' << height << ", "sv << fps << " Hz requested)"sv;
      } else {
        BOOST_LOG(error) << "[kwin] couldn't create a virtual output: "sv << (state.done ? state.error : "no answer from KWin within 5 s"s);
        zkde_screencast_stream_unstable_v1_close(stream);
        wl_display_flush(kwin_wl_display);
      }
    }
    return output_name;
  }

  void kwin_virtual_display_destroy(const std::string &output_name) {
    std::lock_guard lock {kwin_mutex};
    if (const auto output = kwin_virtual_outputs.find(output_name); output != kwin_virtual_outputs.end()) {
      zkde_screencast_stream_unstable_v1_close(output->second);
      wl_display_flush(kwin_wl_display);
      kwin_virtual_outputs.erase(output);
    } else {
      BOOST_LOG(warning) << "[kwin] not a virtual output of ours: ["sv << output_name << ']';
    }
  }
}  // namespace platf
