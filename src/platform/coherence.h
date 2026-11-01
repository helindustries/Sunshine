/**
 * @file src/platform/coherence.h
 * @brief Coherence mode's Start menu and program icons, apart from common.h so adding to it
 *        rebuilds one file.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>

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
}  // namespace platf
