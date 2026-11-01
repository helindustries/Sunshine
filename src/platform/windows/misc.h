/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  /**
   * @brief Whether a user is logged on at the console, as opposed to the sign-in screen.
   */
  bool console_user_logged_on();

  /**
   * @brief Whether the console shows the sign-in or the lock screen, which Windows draws on the
   *        primary display only.
   */
  bool console_awaits_sign_in();

  /**
   * @brief The primary display's output name, empty when there is none.
   */
  std::string primary_display();

  /**
   * @brief Move displays to the desktop positions given, by output name; the others stay.
   * @return Whether Windows took the arrangement.
   */
  bool move_displays(const std::map<std::string, POINTL> &positions);

  /**
   * @brief Tell the next session's encoder check that the displays just added or removed leave
   *        the encoders as they were, as virtual displays on the same adapter do: any display
   *        change otherwise makes it test every encoder again, minutes on a busy host.
   */
  void encoders_unaffected_by_display_change();

  /**
   * @brief The console user's token, elevated if requested and the user is an admin.
   * @param elevated Whether the token should carry the user's admin rights.
   * @return The token, which the caller closes; nullptr when nobody is logged on.
   */
  HANDLE retrieve_users_token(bool elevated);

  /**
   * @brief Whether this process runs as SYSTEM (the service), not as the user (a dev run).
   */
  bool is_running_as_system();

  /**
   * @brief Write status details to the log.
   *
   * @param prefix Text prefix used when formatting the message.
   * @param status Native status code returned by the platform API.
   */
  void print_status(const std::string_view &prefix, HRESULT status);
  /**
   * @brief Synchronize thread desktop.
   *
   * @return true when the thread desktop was synchronized successfully.
   */
  HDESK syncThreadDesktop();

  /**
   * @brief Read the current Windows high-resolution performance counter.
   *
   * @return Raw QPC tick value from `QueryPerformanceCounter`.
   */
  int64_t qpc_counter();

  /**
   * @brief Convert the difference between two QPC readings to nanoseconds.
   *
   * @param performance_counter1 Newer performance-counter reading.
   * @param performance_counter2 Older performance-counter reading.
   * @return Duration represented by the difference between two QPC values.
   */
  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Get file version information from a Windows executable or driver file.
   * @param file_path Path to the file to query.
   * @param version_str Output parameter for version string in format "major.minor.build.revision".
   * @return true if version info was successfully extracted, false otherwise.
   */
  bool getFileVersionInfo(const std::filesystem::path &file_path, std::string &version_str);
}  // namespace platf
