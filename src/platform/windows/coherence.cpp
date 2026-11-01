/**
 * @file src/platform/windows/coherence.cpp
 * @brief Coherence mode on Windows: the desktop's windows, the pinned apps, the start menu.
 */
// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// local includes required before platform includes
// Boost.Process must be included before Windows.h so Boost.Asio sees WinSock2.h first.
#include "src/boost_process_compat.h"

// platform includes
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <Windows.h>

// local includes
#include "misc.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"
#include "utf_utils.h"

namespace platf {
  using namespace std::literals;

  namespace {
    // Desktop and taskbar surfaces, which report themselves as visible, titled windows
    constexpr std::array<std::wstring_view, 4> shell_window_classes {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"};

    // Pinned app icons, as the shell renders them for the largest taskbar and Start sizes
    constexpr int pinned_app_icon_size {256};

    HWND window_handle(std::uint64_t id) {
      return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
    }

    std::string window_process_name(HWND hwnd) {
      std::string name;
      DWORD process_id {};
      GetWindowThreadProcessId(hwnd, &process_id);
      if (auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id); process != nullptr) {
        std::array<wchar_t, MAX_PATH> path {};
        if (auto size = static_cast<DWORD>(path.size()); QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
          name = utf_utils::to_utf8(std::filesystem::path {std::wstring {path.data(), size}}.filename().wstring());
        }
        CloseHandle(process);
      }
      return name;
    }

    BOOL CALLBACK collect_host_window(HWND hwnd, LPARAM windows_param) {
      auto &windows {*reinterpret_cast<std::vector<host_window_t> *>(windows_param)};
      const auto ex_style {GetWindowLongPtrW(hwnd, GWL_EXSTYLE)};
      std::array<wchar_t, 64> class_name {};
      GetClassNameW(hwnd, class_name.data(), static_cast<int>(class_name.size()));
      BOOL cloaked {};
      DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

      // Extended frame bounds are what capture delivers, in physical pixels whatever the DPI
      // awareness; a minimized window has none, its restored placement stands in
      const auto minimized {IsIconic(hwnd) != FALSE};
      RECT bounds {};
      auto has_bounds {false};
      if (minimized) {
        WINDOWPLACEMENT placement {.length = sizeof(WINDOWPLACEMENT)};
        has_bounds = GetWindowPlacement(hwnd, &placement) != FALSE;
        bounds = placement.rcNormalPosition;
      } else {
        has_bounds = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)));
      }
      if (
        IsWindowVisible(hwnd) && !cloaked && !(ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) &&
        GetWindowTextLengthW(hwnd) > 0 && std::ranges::find(shell_window_classes, std::wstring_view {class_name.data()}) == shell_window_classes.end() &&
        has_bounds && bounds.right > bounds.left && bounds.bottom > bounds.top
      ) {
        std::wstring title(GetWindowTextLengthW(hwnd) + 1, L'\0');
        title.resize(GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size())));
        windows.push_back({
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd)),
          utf_utils::to_utf8(title),
          window_process_name(hwnd),
          static_cast<int>(bounds.left),
          static_cast<int>(bounds.top),
          static_cast<int>(bounds.right - bounds.left),
          static_cast<int>(bounds.bottom - bounds.top),
          minimized,
        });
      }
      return TRUE;
    }

    std::filesystem::path pinned_apps_directory() {
      std::filesystem::path directory;

      // The service runs as SYSTEM and looks up the console user's folder; a dev run is that
      // user, and a null token names it
      const auto as_system {is_running_as_system()};
      if (auto token = as_system ? retrieve_users_token(false) : nullptr; token != nullptr || !as_system) {
        PWSTR app_data {};
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, token, &app_data))) {
          directory = std::filesystem::path {app_data} / L"Microsoft" / L"Internet Explorer" / L"Quick Launch" / L"User Pinned" / L"TaskBar";
        }
        CoTaskMemFree(app_data);
        if (token != nullptr) {
          CloseHandle(token);
        }
      }
      return directory;
    }

    std::vector<std::filesystem::path> pinned_app_shortcuts() {
      std::vector<std::filesystem::path> shortcuts;
      std::error_code error;
      if (const auto directory {pinned_apps_directory()}; !directory.empty()) {
        for (auto entry = std::filesystem::directory_iterator {directory, error}; !error && entry != std::filesystem::directory_iterator {}; entry.increment(error)) {
          if (entry->is_regular_file() && entry->path().extension() == L".lnk") {
            shortcuts.push_back(entry->path());
          }
        }
        if (error) {
          BOOST_LOG(warning) << "Couldn't read the pinned apps in "sv << directory.string() << ": "sv << error.message();
        }
      }
      return shortcuts;
    }

    // Ids are looked up among the shortcuts, never joined to a path: a client cannot reach
    // anything but a pinned app
    std::filesystem::path pinned_app_shortcut(const std::string &id) {
      std::filesystem::path shortcut;
      for (const auto &path : pinned_app_shortcuts()) {
        if (utf_utils::to_utf8(path.filename().wstring()) == id) {
          shortcut = path;
        }
      }
      return shortcut;
    }

    std::string bitmap_png(HBITMAP bitmap) {
      std::string png;
      IWICImagingFactory *wic {};
      IWICBitmap *source {};
      IWICBitmapEncoder *encoder {};
      IWICBitmapFrameEncode *frame {};
      IStream *stream {SHCreateMemStream(nullptr, 0)};
      auto release = util::fail_guard([&]() {
        for (IUnknown *object : std::initializer_list<IUnknown *> {frame, encoder, source, wic, stream}) {
          if (object != nullptr) {
            object->Release();
          }
        }
      });

      ULARGE_INTEGER size {};
      auto status {stream != nullptr ? S_OK : E_OUTOFMEMORY};
      if (SUCCEEDED(status)) {
        status = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
      }
      if (SUCCEEDED(status)) {
        status = wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, &source);
      }
      if (SUCCEEDED(status)) {
        status = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
      }
      if (SUCCEEDED(status)) {
        status = encoder->Initialize(stream, WICBitmapEncoderNoCache);
      }
      if (SUCCEEDED(status)) {
        status = encoder->CreateNewFrame(&frame, nullptr);
      }
      if (SUCCEEDED(status)) {
        status = frame->Initialize(nullptr);
      }
      if (SUCCEEDED(status)) {
        status = frame->WriteSource(source, nullptr);
      }
      if (SUCCEEDED(status)) {
        status = frame->Commit();
      }
      if (SUCCEEDED(status)) {
        status = encoder->Commit();
      }
      if (SUCCEEDED(status)) {
        status = IStream_Size(stream, &size);
      }
      if (SUCCEEDED(status)) {
        status = IStream_Reset(stream);
      }
      if (SUCCEEDED(status)) {
        png.resize(size.QuadPart);
        status = IStream_Read(stream, png.data(), static_cast<ULONG>(png.size()));
      }
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Couldn't encode an app icon as PNG [0x"sv << util::hex(status).to_string_view() << ']';
        png.clear();
      }
      return png;
    }

    // Coherence screens refresh at this rate; the windows on them are streamed at their own
    constexpr int coherence_screen_fps {60};

    /**
     * @brief A client's coherence screen and the free area it was sized for.
     */
    struct coherence_screen_t {
      std::string output_name;  ///< The virtual display.
      int width;  ///< Width of the client's free area.
      int height;  ///< Height of the client's free area, the screen's work area.
      std::map<HWND, WINDOWPLACEMENT> home;  ///< Where each gathered window was, to put it back.
    };

    std::mutex coherence_mutex;
    std::map<std::string, coherence_screen_t> coherence_screens;  ///< By client.

    // The coherence screen adds the taskbar below the client's free area, as tall as the
    // primary display's: Windows shows a taskbar on every display unless told otherwise
    int primary_taskbar_height() {
      auto height {0};
      if (MONITORINFO info {.cbSize = sizeof(MONITORINFO)}; GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info)) {
        height = (info.rcMonitor.bottom - info.rcMonitor.top) - (info.rcWork.bottom - info.rcWork.top);
      }
      return height;
    }

    std::optional<RECT> output_rect(const std::string &output_name) {
      std::optional<RECT> rect;
      DEVMODEW mode {};
      mode.dmSize = sizeof(mode);
      if (EnumDisplaySettingsW(utf_utils::from_utf8(output_name).c_str(), ENUM_CURRENT_SETTINGS, &mode)) {
        rect = RECT {mode.dmPosition.x, mode.dmPosition.y, mode.dmPosition.x + static_cast<LONG>(mode.dmPelsWidth), mode.dmPosition.y + static_cast<LONG>(mode.dmPelsHeight)};
      }
      return rect;
    }

    /**
     * @brief Move a window onto a coherence screen, keeping its offset in the work area it was
     *        on and shrinking it to fit; a maximized window is maximized there again. Minimized
     *        windows stay until restored, the next gathering moves them.
     */
    void gather_window(HWND hwnd, const RECT &screen, int work_width, int work_height, std::map<HWND, WINDOWPLACEMENT> &home) {
      const auto monitor {MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)};
      if (!IsIconic(hwnd) && monitor != MonitorFromRect(&screen, MONITOR_DEFAULTTONULL)) {
        if (WINDOWPLACEMENT placement {.length = sizeof(WINDOWPLACEMENT)}; GetWindowPlacement(hwnd, &placement)) {
          home.try_emplace(hwnd, placement);
        }

        // Asynchronous calls throughout: this runs on the HTTPS thread, which serves one request
        // at a time, and a synchronous move waits for every window's own thread to handle it
        const auto maximized {IsZoomed(hwnd) != FALSE};
        if (maximized) {
          ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE);
        }

        // Positions are the visible frame's; the window rectangle adds invisible borders
        RECT frame {};
        RECT window_rect {};
        if (MONITORINFO info {.cbSize = sizeof(MONITORINFO)}; GetMonitorInfoW(monitor, &info) && GetWindowRect(hwnd, &window_rect) && SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
          const auto width {std::min<LONG>(frame.right - frame.left, work_width)};
          const auto height {std::min<LONG>(frame.bottom - frame.top, work_height)};
          const auto left {screen.left + std::clamp<LONG>(frame.left - info.rcWork.left, 0, work_width - width)};
          const auto top {screen.top + std::clamp<LONG>(frame.top - info.rcWork.top, 0, work_height - height)};
          const auto border_left {frame.left - window_rect.left};
          const auto border_top {frame.top - window_rect.top};
          const auto border_width {(window_rect.right - window_rect.left) - (frame.right - frame.left)};
          const auto border_height {(window_rect.bottom - window_rect.top) - (frame.bottom - frame.top)};
          if (!SetWindowPos(hwnd, nullptr, left - border_left, top - border_top, width + border_width, height + border_height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)) {
            BOOST_LOG(warning) << "Couldn't move window "sv << reinterpret_cast<std::uintptr_t>(hwnd) << " onto the coherence screen: "sv << GetLastError();
          }
        }
        if (maximized) {
          ShowWindowAsync(hwnd, SW_MAXIMIZE);
        }
      }
    }

    // Removing the screen alone would leave every gathered window on the primary display
    void send_windows_home(const coherence_screen_t &screen) {
      for (const auto &[hwnd, placement] : screen.home) {
        if (IsWindow(hwnd) && !SetWindowPlacement(hwnd, &placement)) {
          BOOST_LOG(warning) << "Couldn't put window "sv << reinterpret_cast<std::uintptr_t>(hwnd) << " back where it was: "sv << GetLastError();
        }
      }
    }
  }  // namespace

  std::vector<host_window_t> host_windows() {
    std::vector<host_window_t> windows;
    syncThreadDesktop();
    if (!EnumWindows(collect_host_window, reinterpret_cast<LPARAM>(&windows))) {
      BOOST_LOG(warning) << "Couldn't enumerate the desktop's windows: "sv << GetLastError();
    }
    return windows;
  }

  void raise_host_window(std::uint64_t id) {
    if (const auto hwnd {window_handle(id)}; IsWindow(hwnd) && GetForegroundWindow() != hwnd) {
      if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
      }

      // Windows hands the foreground only to the process the last input went to; sharing the
      // foreground thread's input state for the call makes this process count as that one
      const auto foreground_thread {GetWindowThreadProcessId(GetForegroundWindow(), nullptr)};
      const auto own_thread {GetCurrentThreadId()};
      const auto attached {foreground_thread != 0 && foreground_thread != own_thread && AttachThreadInput(own_thread, foreground_thread, TRUE)};
      if (!SetForegroundWindow(hwnd)) {
        BOOST_LOG(debug) << "Couldn't bring window "sv << id << " to the front: "sv << GetLastError();
      }
      if (attached) {
        AttachThreadInput(own_thread, foreground_thread, FALSE);
      }
    }
  }

  bool resize_host_window(std::uint64_t id, int width, int height) {
    auto resized {false};
    const auto hwnd {window_handle(id)};
    RECT frame {};
    RECT window_rect {};

    // Sizes are what capture delivers, the visible frame; the window rectangle adds the
    // invisible resize borders around it
    if (IsWindow(hwnd) && width > 0 && height > 0 && GetWindowRect(hwnd, &window_rect) && SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
      const auto border_width {(window_rect.right - window_rect.left) - (frame.right - frame.left)};
      const auto border_height {(window_rect.bottom - window_rect.top) - (frame.bottom - frame.top)};
      if (IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
      }
      resized = SetWindowPos(hwnd, nullptr, 0, 0, width + border_width, height + border_height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
    }
    if (!resized) {
      BOOST_LOG(warning) << "Couldn't resize window "sv << id << " to "sv << width << 'x' << height << ": "sv << GetLastError();
    }
    return resized;
  }

  std::vector<host_app_t> pinned_host_apps() {
    std::vector<host_app_t> apps;
    for (const auto &shortcut : pinned_app_shortcuts()) {
      apps.push_back({utf_utils::to_utf8(shortcut.filename().wstring()), utf_utils::to_utf8(shortcut.stem().wstring())});
    }
    return apps;
  }

  std::string pinned_host_app_icon(const std::string &id) {
    // Rendering one takes seconds while the shell warms up, and requests are served one at a
    // time, so each icon is rendered once per version of its shortcut
    static std::mutex cache_mutex;
    static std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::string>> cache;

    std::string png;
    if (const auto shortcut {pinned_app_shortcut(id)}; !shortcut.empty()) {
      std::error_code error;
      const auto modified {std::filesystem::last_write_time(shortcut, error)};
      std::lock_guard lock {cache_mutex};
      if (const auto cached {cache.find(shortcut)}; cached != cache.end() && cached->second.first == modified) {
        png = cached->second.second;
      } else {
        // The shell and WIC are COM, and an HTTP worker thread has no apartment of its own
        const auto started {std::chrono::steady_clock::now()};
        const auto com_initialized {SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))};
        IShellItemImageFactory *factory {};
        HBITMAP bitmap {};
        if (SUCCEEDED(SHCreateItemFromParsingName(shortcut.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
          if (SUCCEEDED(factory->GetImage(SIZE {pinned_app_icon_size, pinned_app_icon_size}, SIIGBF_ICONONLY, &bitmap))) {
            png = bitmap_png(bitmap);
            DeleteObject(bitmap);
          }
          factory->Release();
        }
        if (com_initialized) {
          CoUninitialize();
        }
        BOOST_LOG(debug) << "Rendered the icon of pinned app ["sv << id << "] in "sv << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << " ms"sv;
        if (!png.empty()) {
          cache[shortcut] = {modified, png};
        }
      }
    }
    if (png.empty()) {
      BOOST_LOG(warning) << "Expected an icon for pinned app ["sv << id << "], found none"sv;
    }
    return png;
  }

  bool launch_pinned_host_app(const std::string &id) {
    auto launched {false};
    if (const auto shortcut {pinned_app_shortcut(id)}; !shortcut.empty()) {
      // The shell resolves the shortcut as a click on the taskbar would
      const auto command {"explorer.exe \""s + utf_utils::to_utf8(shortcut.wstring()) + '"'};
      boost::process::v1::environment env = boost::this_process::environment();
      boost::filesystem::path working_dir;
      std::error_code error;
      auto child {run_command(false, false, command, working_dir, env, nullptr, error, nullptr)};
      if (error) {
        BOOST_LOG(warning) << "Couldn't start pinned app ["sv << id << "]: "sv << error.message();
      } else {
        child.detach();
        launched = true;
      }
    } else {
      BOOST_LOG(warning) << "Expected a pinned app named ["sv << id << "], the user has none"sv;
    }
    return launched;
  }

  std::optional<std::pair<int, int>> coherence_screen(const std::string &client_id, int width, int height) {
    std::optional<std::pair<int, int>> origin;
    std::lock_guard lock {coherence_mutex};

    // A client whose free area changed (Dock shown, another screen) gets a screen of the new size;
    // its windows keep the homes they had before the first one
    std::map<HWND, WINDOWPLACEMENT> home;
    auto screen {coherence_screens.find(client_id)};
    if (screen != coherence_screens.end() && (screen->second.width != width || screen->second.height != height)) {
      home = std::move(screen->second.home);
      virtual_display_destroy(screen->second.output_name);
      coherence_screens.erase(screen);
      screen = coherence_screens.end();
    }
    if (screen == coherence_screens.end() && width > 0 && height > 0) {
      const auto identity {display_device::virtual_display_identity(client_id, "coherence")};
      if (const auto output_name {virtual_display_create(width, height + primary_taskbar_height(), coherence_screen_fps, identity)}; !output_name.empty()) {
        BOOST_LOG(info) << "Coherence screen "sv << output_name << " for a "sv << width << 'x' << height << " client area"sv;
        screen = coherence_screens.emplace(client_id, coherence_screen_t {output_name, width, height, std::move(home)}).first;
      }
    }

    if (screen != coherence_screens.end()) {
      if (const auto rect {output_rect(screen->second.output_name)}; rect) {
        origin = std::make_pair(static_cast<int>(rect->left), static_cast<int>(rect->top));
        for (const auto &window : host_windows()) {
          gather_window(window_handle(window.id), *rect, width, height, screen->second.home);
        }
      } else {
        BOOST_LOG(warning) << "Expected coherence screen "sv << screen->second.output_name << " on the desktop, it is not"sv;
      }
    }
    return origin;
  }

  void release_coherence_screen(const std::string &client_id) {
    std::lock_guard lock {coherence_mutex};
    if (const auto screen {coherence_screens.find(client_id)}; screen != coherence_screens.end()) {
      send_windows_home(screen->second);
      virtual_display_destroy(screen->second.output_name);
      coherence_screens.erase(screen);
    }
  }
}  // namespace platf
