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
#include <string>
#include <system_error>
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
}  // namespace platf
