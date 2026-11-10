/**
 * @file src/platform/coherence.h
 * @brief Coherence mode's Start menu and program icons.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace platf {
  /**
   * @brief Open the host's Start menu on a client's coherence screen, by its own taskbar's Start
   *        button: the Start key opens it on the primary display, which no client mirrors.
   * @param client_id Paired client the screen belongs to.
   * @return Whether the button was found and pressed.
   */
  bool open_coherence_start_menu(const std::string &client_id);

  /**
   * @brief The icon of the program a host window belongs to, which a client shows its windows
   *        under (coherence mode).
   * @param id Window id from host_windows().
   * @return PNG, empty where the window is gone or its program has no icon.
   */
  std::string host_window_icon(std::uint64_t id);

  /**
   * @brief Ask a host window to close, as its close button does: the program may still ask
   *        about unsaved work.
   * @param id Window id from host_windows().
   * @return Whether the window was there to ask.
   */
  bool close_host_window(std::uint64_t id);

  /**
   * @brief Whether a program hides the host's pointer, as a 3D viewport does while it reads
   *        relative motion.
   */
  bool host_cursor_hidden();

  /**
   * @brief The desktop's top-left corner. Absolute pointer positions count from it, while host
   *        window and display positions count from the primary display's corner.
   */
  std::pair<int, int> desktop_origin();

  /**
   * @brief Whether the secure desktop shows (a UAC prompt, the lock screen): it covers every
   *        display, and draws its prompt on the primary one, which a coherence atlas then shows.
   */
  bool secure_desktop_active();

  /**
   * @brief What a session captures now: a coherence atlas captures the primary display while
   *        the secure desktop shows. Capture reinitializes on each desktop switch.
   * @param output_name The session's output name.
   * @return The output to capture.
   */
  std::string coherence_capture_output(const std::string &output_name);

  /**
   * @brief A pinned notification area icon: its tooltip, and its picture as on the taskbar, as
   *        PNG with the taskbar's background transparent.
   */
  struct host_tray_icon_t {
    std::string name;
    std::string png;
  };

  /**
   * @brief The primary taskbar's pinned notification area icons, in their order there; the
   *        hidden ones and the system's own (network, volume, clock) are left out.
   */
  std::vector<host_tray_icon_t> host_tray_icons();

  /**
   * @brief Click a pinned notification area icon, by its place in host_tray_icons().
   * @param index Its place.
   * @param right A right click, for the icon's menu.
   * @return Whether the icon was there to click.
   */
  bool click_host_tray_icon(std::size_t index, bool right);
}  // namespace platf
