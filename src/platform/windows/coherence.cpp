/**
 * @file src/platform/windows/coherence.cpp
 * @brief Coherence mode on Windows: the desktop's windows, the pinned apps, the start menu.
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
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
#include <uiautomation.h>
#include <wincodec.h>
#include <Windows.h>

// local includes
#include "misc.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/platform/coherence.h"
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

    // Coherence screens and their atlases refresh at this rate
    constexpr int coherence_screen_fps {60};

    // An atlas holds this many work areas of its coherence screen, and so as many maximized
    // windows at full size
    constexpr int atlas_screens_wide {2};
    constexpr int atlas_screens_high {2};

    // A window's slot leaves this much room to grow, so resizing it moves no other window
    constexpr LONG atlas_headroom {128};

    // Windows that do not fit all shrink by this factor until they do
    constexpr double atlas_scale_step {0.9};
    constexpr double atlas_min_scale {0.1};

    constexpr DWORD atlas_interval_ms {1000 / coherence_screen_fps};
    constexpr auto atlas_class_name {L"SunshineCoherenceAtlas"};

    /**
     * @brief One of a client's screens and the coherence screen mirroring it.
     */
    struct coherence_display_t {
      std::string output_name;  ///< The virtual display.
      host_rect_t client;  ///< The client screen's free area, relative to all the client's screens.
    };

    /**
     * @brief A client's atlas display, and the thread drawing its coherence screens' windows there.
     */
    struct atlas_t {
      std::string output_name;  ///< The atlas display.
      int width;  ///< Atlas width in pixels.
      int height;  ///< Atlas height in pixels.
      std::atomic<bool> stop {false};
      std::mutex mutex;  ///< Guards displays and regions.
      std::vector<coherence_display_t> displays;  ///< The coherence screens whose windows are drawn.
      std::vector<atlas_region_t> regions;  ///< What the thread drew last.
      std::thread thread;
    };

    /**
     * @brief A client in coherence mode: its coherence screens and atlas.
     */
    struct coherence_client_t {
      std::vector<coherence_display_t> displays;  ///< One per client screen, its primary first.
      std::map<HWND, WINDOWPLACEMENT> home;  ///< Where each gathered window was, to put it back.
      std::unique_ptr<atlas_t> atlas;  ///< Null where the atlas display could not be made.
      std::chrono::steady_clock::time_point last_request;  ///< The client's last coherence_screens().
    };

    std::mutex coherence_mutex;
    std::map<std::string, coherence_client_t> coherence_clients;  ///< By client.

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
     * @brief A coherence screen as it is on the desktop now.
     */
    struct placed_display_t {
      HMONITOR monitor;
      RECT work;
      host_rect_t client;  ///< The client screen it mirrors.
    };

    std::vector<placed_display_t> place_displays(const std::vector<coherence_display_t> &displays) {
      std::vector<placed_display_t> placed;
      for (const auto &display : displays) {
        if (const auto rect {output_rect(display.output_name)}; rect) {
          const auto monitor {MonitorFromRect(&*rect, MONITOR_DEFAULTTONULL)};
          if (MONITORINFO info {.cbSize = sizeof(MONITORINFO)}; monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
            placed.push_back({monitor, info.rcWork, display.client});
          }
        }
      }
      return placed;
    }

    // By the display the point is on, or the one given: a window's corner can lie off its display
    std::pair<int, int> client_point(const std::vector<placed_display_t> &placed, POINT point, HMONITOR monitor = nullptr) {
      auto client {std::make_pair(static_cast<int>(point.x), static_cast<int>(point.y))};
      if (!placed.empty()) {
        const auto display {std::ranges::find(placed, monitor != nullptr ? monitor : MonitorFromPoint(point, MONITOR_DEFAULTTONULL), &placed_display_t::monitor)};
        const auto &on {display != placed.end() ? *display : placed.front()};
        client = std::make_pair(on.client.x + static_cast<int>(point.x - on.work.left), on.client.y + static_cast<int>(point.y - on.work.top));
      }
      return client;
    }

    // A point between client screens goes to the one nearest to it
    std::optional<std::pair<int, int>> host_point(const std::vector<placed_display_t> &placed, int x, int y) {
      std::optional<std::pair<int, int>> host;
      auto nearest {std::numeric_limits<long long>::max()};
      for (const auto &display : placed) {
        const auto clamped_x {std::clamp(x, display.client.x, display.client.x + display.client.width - 1)};
        const auto clamped_y {std::clamp(y, display.client.y, display.client.y + display.client.height - 1)};
        if (const auto distance {static_cast<long long>(clamped_x - x) * (clamped_x - x) + static_cast<long long>(clamped_y - y) * (clamped_y - y)}; distance < nearest) {
          nearest = distance;
          host = std::make_pair(static_cast<int>(display.work.left) + clamped_x - display.client.x, static_cast<int>(display.work.top) + clamped_y - display.client.y);
        }
      }
      return host;
    }

    /**
     * @brief Arrange a client's coherence screens on the desktop as its screens are, right of
     *        every other display: a window dragged across a screen edge there crosses here too.
     *        The taskbar each adds below its work area moves the screens under it down.
     */
    void arrange_displays(const std::vector<coherence_display_t> &displays, int taskbar_height) {
      struct right_edge_t {
        std::vector<RECT> own;
        LONG right;
      } edge {{}, 0};
      for (const auto &display : displays) {
        if (const auto rect {output_rect(display.output_name)}; rect) {
          edge.own.push_back(*rect);
        }
      }
      EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT rect, LPARAM edge_param) -> BOOL {
        auto &edge {*reinterpret_cast<right_edge_t *>(edge_param)};
        if (std::ranges::none_of(edge.own, [rect](const RECT &own) {
              return EqualRect(&own, rect) != FALSE;
            })) {
          edge.right = std::max(edge.right, rect->right);
        }
        return TRUE;
      }, reinterpret_cast<LPARAM>(&edge));
      const auto left {edge.right};

      std::map<std::string, POINTL> positions;
      for (const auto &display : displays) {
        const auto above {std::ranges::count_if(displays, [&display](const coherence_display_t &other) {
          return other.client.y + other.client.height <= display.client.y;
        })};
        positions.emplace(display.output_name, POINTL {left + display.client.x, display.client.y + static_cast<LONG>(above) * taskbar_height});
      }
      move_displays(positions);
    }

    /**
     * @brief Move a window onto a coherence screen at its size, keeping its offset in the work
     *        area it was on as far as it fits; a maximized window is maximized there again.
     *        Windows on any coherence screen stay, minimized ones until restored: the next
     *        gathering moves them.
     */
    void gather_window(HWND hwnd, const std::vector<placed_display_t> &placed, std::map<HWND, WINDOWPLACEMENT> &home) {
      const auto monitor {MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)};
      if (!IsIconic(hwnd) && !placed.empty() && std::ranges::find(placed, monitor, &placed_display_t::monitor) == placed.end()) {
        const RECT screen {placed.front().work};
        const auto work_width {screen.right - screen.left};
        const auto work_height {screen.bottom - screen.top};
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
          // A window larger than the screen keeps its top left corner on it
          const auto width {frame.right - frame.left};
          const auto height {frame.bottom - frame.top};
          const auto left {screen.left + std::clamp<LONG>(frame.left - info.rcWork.left, 0, std::max<LONG>(work_width - width, 0))};
          const auto top {screen.top + std::clamp<LONG>(frame.top - info.rcWork.top, 0, std::max<LONG>(work_height - height, 0))};
          if (!SetWindowPos(hwnd, nullptr, left - (frame.left - window_rect.left), top - (frame.top - window_rect.top), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)) {
            BOOST_LOG(warning) << "Couldn't move window "sv << reinterpret_cast<std::uintptr_t>(hwnd) << " onto the coherence screen: "sv << GetLastError();
          }
        }
        if (maximized) {
          ShowWindowAsync(hwnd, SW_MAXIMIZE);
        }
      }
    }

    // Removing the screens alone would leave every gathered window on the primary display. A
    // window goes back to its place at the size it has now, maximized if it is now or was there
    void send_windows_home(const coherence_client_t &client) {
      for (const auto &[hwnd, home] : client.home) {
        if (WINDOWPLACEMENT now {.length = sizeof(WINDOWPLACEMENT)}; IsWindow(hwnd) && GetWindowPlacement(hwnd, &now)) {
          auto placement {home};
          placement.rcNormalPosition.right = home.rcNormalPosition.left + (now.rcNormalPosition.right - now.rcNormalPosition.left);
          placement.rcNormalPosition.bottom = home.rcNormalPosition.top + (now.rcNormalPosition.bottom - now.rcNormalPosition.top);
          if (now.showCmd == SW_SHOWMAXIMIZED) {
            placement.showCmd = SW_SHOWMAXIMIZED;
          }
          if (!SetWindowPlacement(hwnd, &placement)) {
            BOOST_LOG(warning) << "Couldn't put window "sv << reinterpret_cast<std::uintptr_t>(hwnd) << " back where it was: "sv << GetLastError();
          }
        }
      }
    }

    /**
     * @brief A file's icon as the shell shows it, the largest size, as PNG; empty where it has
     *        none. Rendering one takes seconds while the shell warms up, and requests are served
     *        one at a time, so each is rendered once per version of its file.
     */
    std::string file_icon_png(const std::filesystem::path &file) {
      static std::mutex cache_mutex;
      static std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::string>> cache;

      std::string png;
      std::error_code error;
      const auto modified {std::filesystem::last_write_time(file, error)};
      std::lock_guard lock {cache_mutex};
      if (const auto cached {cache.find(file)}; cached != cache.end() && cached->second.first == modified) {
        png = cached->second.second;
      } else {
        // The shell and WIC are COM, and an HTTP worker thread has no apartment of its own
        const auto started {std::chrono::steady_clock::now()};
        const auto com_initialized {SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))};
        IShellItemImageFactory *factory {};
        HBITMAP bitmap {};
        if (SUCCEEDED(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
          if (SUCCEEDED(factory->GetImage(SIZE {pinned_app_icon_size, pinned_app_icon_size}, SIIGBF_ICONONLY, &bitmap))) {
            png = bitmap_png(bitmap);
            DeleteObject(bitmap);
          }
          factory->Release();
        }
        if (com_initialized) {
          CoUninitialize();
        }
        BOOST_LOG(debug) << "Rendered the icon of "sv << file.string() << " in "sv << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << " ms"sv;
        if (!png.empty()) {
          cache[file] = {modified, png};
        }
      }
      return png;
    }

    // UI Automation's class and interface, which MinGW declares without defining
    constexpr CLSID cuiautomation_class {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
    constexpr IID iuiautomation_interface {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};

    // Start and Search draw their panel inside a window as large as the work area, transparent
    // around it. The window's automation children span the panel's width, and leave out its
    // padding above (20 px) and below (15 px), measured on Start at 125%
    constexpr std::wstring_view shell_panel_class {L"Windows.UI.Core.CoreWindow"};
    constexpr LONG shell_panel_padding_top {20};
    constexpr LONG shell_panel_padding_bottom {16};
    constexpr int shell_panel_refresh_updates {30};

    // Search keeps a transparent window over Start's search box while nothing is searched for,
    // which a stream shows black; with results it is far taller
    constexpr LONG shell_panel_min_height {120};

    template<class T>
    struct com_release {
      void operator()(T *object) const {
        object->Release();
      }
    };

    template<class T>
    using com_ptr_t = std::unique_ptr<T, com_release<T>>;

    com_ptr_t<IUIAutomation> make_automation() {
      IUIAutomation *automation {};
      if (const auto status {CoCreateInstance(cuiautomation_class, nullptr, CLSCTX_INPROC_SERVER, iuiautomation_interface, reinterpret_cast<void **>(&automation))}; FAILED(status)) {
        BOOST_LOG(warning) << "Couldn't create UI Automation: "sv << util::hex(status).to_string_view();
        automation = nullptr;
      }
      return com_ptr_t<IUIAutomation> {automation};
    }

    // Start's search box, apps and user row span its panel's width; other children, like the
    // scroll viewer of the full app list, reach past the panel
    constexpr std::array<std::wstring_view, 3> start_panel_parts {L"SearchBoxToggleButton", L"TopLevelRoot", L"UserControl"};

    std::optional<RECT> automation_children_bounds(IUIAutomation &automation, HWND hwnd) {
      std::optional<RECT> bounds;
      std::optional<RECT> start_bounds;
      IUIAutomationElement *element {};
      IUIAutomationCondition *condition {};
      IUIAutomationElementArray *children {};
      if (SUCCEEDED(automation.ElementFromHandle(hwnd, &element)) && element != nullptr) {
        const com_ptr_t<IUIAutomationElement> element_owner {element};
        // The control view: the raw one holds an invisible child larger than the panel
        if (SUCCEEDED(automation.get_ControlViewCondition(&condition)) && condition != nullptr) {
          const com_ptr_t<IUIAutomationCondition> condition_owner {condition};
          if (SUCCEEDED(element->FindAll(TreeScope_Children, condition, &children)) && children != nullptr) {
            const com_ptr_t<IUIAutomationElementArray> children_owner {children};
            auto count {0};
            children->get_Length(&count);
            for (auto index {0}; index < count; ++index) {
              if (IUIAutomationElement *child {}; SUCCEEDED(children->GetElement(index, &child)) && child != nullptr) {
                const com_ptr_t<IUIAutomationElement> child_owner {child};
                if (RECT rect {}; SUCCEEDED(child->get_CurrentBoundingRectangle(&rect)) && !IsRectEmpty(&rect)) {
                  if (bounds) {
                    UnionRect(&*bounds, &*bounds, &rect);
                  } else {
                    bounds = rect;
                  }
                  if (BSTR id {}; SUCCEEDED(child->get_CurrentAutomationId(&id)) && id != nullptr) {
                    if (std::ranges::find(start_panel_parts, std::wstring_view {id}) != start_panel_parts.end()) {
                      if (start_bounds) {
                        UnionRect(&*start_bounds, &*start_bounds, &rect);
                      } else {
                        start_bounds = rect;
                      }
                    }
                    SysFreeString(id);
                  }
                }
              }
            }
          }
        }
      }
      return start_bounds ? start_bounds : bounds;
    }

    std::optional<RECT> automation_element_bounds(IUIAutomation &automation, HWND hwnd, const wchar_t *automation_id) {
      std::optional<RECT> bounds;
      IUIAutomationElement *element {};
      IUIAutomationCondition *condition {};
      IUIAutomationElement *found {};
      if (SUCCEEDED(automation.ElementFromHandle(hwnd, &element)) && element != nullptr) {
        const com_ptr_t<IUIAutomationElement> element_owner {element};
        VARIANT id;
        VariantInit(&id);
        id.vt = VT_BSTR;
        id.bstrVal = SysAllocString(automation_id);
        const auto created {SUCCEEDED(automation.CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &condition)) && condition != nullptr};
        VariantClear(&id);
        if (created) {
          const com_ptr_t<IUIAutomationCondition> condition_owner {condition};
          if (SUCCEEDED(element->FindFirst(TreeScope_Descendants, condition, &found)) && found != nullptr) {
            const com_ptr_t<IUIAutomationElement> found_owner {found};
            if (RECT rect {}; SUCCEEDED(found->get_CurrentBoundingRectangle(&rect)) && !IsRectEmpty(&rect)) {
              bounds = rect;
            }
          }
        }
      }
      return bounds;
    }

    /**
     * @brief A pinned notification area icon as UI Automation finds it.
     */
    struct tray_button_t {
      std::wstring name;  ///< Its tooltip.
      RECT button;  ///< On the desktop.
      RECT image;  ///< On the desktop; the button's where the icon has no image child.
    };

    // Pinned icons are the NotifyItemIcon buttons of the primary taskbar; the system's own
    // (network, volume, clock) and the hidden icons' chevron are SystemTrayIcon ones
    constexpr const wchar_t *pinned_tray_icon_id {L"NotifyItemIcon"};

    std::vector<tray_button_t> tray_buttons(IUIAutomation &automation) {
      std::vector<tray_button_t> buttons;
      IUIAutomationElement *taskbar {};
      IUIAutomationCondition *condition {};
      IUIAutomationCondition *any {};
      IUIAutomationElementArray *found {};
      if (const auto tray {FindWindowW(L"Shell_TrayWnd", nullptr)}; tray != nullptr && SUCCEEDED(automation.ElementFromHandle(tray, &taskbar)) && taskbar != nullptr) {
        const com_ptr_t<IUIAutomationElement> taskbar_owner {taskbar};
        VARIANT id;
        VariantInit(&id);
        id.vt = VT_BSTR;
        id.bstrVal = SysAllocString(pinned_tray_icon_id);
        const auto created {SUCCEEDED(automation.CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &condition)) && condition != nullptr && SUCCEEDED(automation.CreateTrueCondition(&any)) && any != nullptr};
        VariantClear(&id);
        const com_ptr_t<IUIAutomationCondition> condition_owner {condition};
        const com_ptr_t<IUIAutomationCondition> any_owner {any};
        if (created && SUCCEEDED(taskbar->FindAll(TreeScope_Descendants, condition, &found)) && found != nullptr) {
          const com_ptr_t<IUIAutomationElementArray> found_owner {found};
          auto count {0};
          found->get_Length(&count);
          for (auto index {0}; index < count; ++index) {
            if (IUIAutomationElement *button {}; SUCCEEDED(found->GetElement(index, &button)) && button != nullptr) {
              const com_ptr_t<IUIAutomationElement> button_owner {button};
              tray_button_t entry {};
              if (BSTR name {}; SUCCEEDED(button->get_CurrentName(&name)) && name != nullptr) {
                entry.name = name;
                SysFreeString(name);
              }
              if (SUCCEEDED(button->get_CurrentBoundingRectangle(&entry.button)) && !IsRectEmpty(&entry.button)) {
                entry.image = entry.button;
                if (IUIAutomationElement *image {}; SUCCEEDED(button->FindFirst(TreeScope_Children, any, &image)) && image != nullptr) {
                  const com_ptr_t<IUIAutomationElement> image_owner {image};
                  if (RECT rect {}; SUCCEEDED(image->get_CurrentBoundingRectangle(&rect)) && !IsRectEmpty(&rect)) {
                    entry.image = rect;
                  }
                }
                buttons.push_back(std::move(entry));
              }
            }
          }
        }
      }
      return buttons;
    }

    // Pixels this close to the taskbar's colour (the largest channel difference) are its
    // background, transparent; up to the opaque limit they fade in, which keeps antialiased edges
    constexpr int tray_background_tolerance {12};
    constexpr int tray_opaque_difference {48};

    /**
     * @brief A rectangle of the screen as PNG, with the colour just outside it transparent: the
     *        taskbar behind a tray icon.
     */
    std::string tray_image_png(const RECT &image) {
      std::string png;
      const auto width {image.right - image.left};
      const auto height {image.bottom - image.top};
      BITMAPINFO info {};
      info.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER), .biWidth = width, .biHeight = -height, .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB};
      void *bits {};
      const auto screen {GetDC(nullptr)};
      const auto memory {CreateCompatibleDC(screen)};
      if (const auto bitmap {CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0)}; bitmap != nullptr) {
        const auto previous {SelectObject(memory, bitmap)};
        const auto background {GetPixel(screen, image.left - 1, image.top - 1)};
        if (BitBlt(memory, 0, 0, width, height, screen, image.left, image.top, SRCCOPY | CAPTUREBLT) && background != CLR_INVALID) {
          auto *pixels {static_cast<std::uint8_t *>(bits)};
          for (auto offset {0}; offset < width * height * 4; offset += 4) {
            const auto difference {std::max({std::abs(pixels[offset] - GetBValue(background)), std::abs(pixels[offset + 1] - GetGValue(background)), std::abs(pixels[offset + 2] - GetRValue(background))})};
            pixels[offset + 3] = static_cast<std::uint8_t>(std::clamp((difference - tray_background_tolerance) * 255 / (tray_opaque_difference - tray_background_tolerance), 0, 255));
          }
          SelectObject(memory, previous);
          png = bitmap_png(bitmap);
        } else {
          SelectObject(memory, previous);
        }
        DeleteObject(bitmap);
      }
      DeleteDC(memory);
      ReleaseDC(nullptr, screen);
      return png;
    }

    // Tray icons open their menus and windows on the taskbar's edge, where the Mac's Dock covers
    // them; the Mac's tray is its menu bar, so for a while after a click, what shows up on that
    // edge (within the margin) moves to the top of the screen
    constexpr auto tray_popup_watch {std::chrono::seconds {2}};
    constexpr auto tray_popup_poll {std::chrono::milliseconds {50}};
    constexpr LONG tray_popup_margin {24};

    std::set<HWND> visible_windows() {
      std::set<HWND> windows;
      EnumWindows([](HWND hwnd, LPARAM windows_param) -> BOOL {
        if (IsWindowVisible(hwnd)) {
          reinterpret_cast<std::set<HWND> *>(windows_param)->insert(hwnd);
        }
        return TRUE;
      }, reinterpret_cast<LPARAM>(&windows));
      return windows;
    }

    void move_tray_popups_up(std::set<HWND> shown_before, HMONITOR monitor) {
      std::thread {[shown_before = std::move(shown_before), monitor]() {
        std::set<HWND> moved;
        MONITORINFO info {.cbSize = sizeof(MONITORINFO)};
        const auto deadline {std::chrono::steady_clock::now() + tray_popup_watch};
        while (GetMonitorInfoW(monitor, &info) && std::chrono::steady_clock::now() < deadline) {
          for (const auto hwnd : visible_windows()) {
            if (RECT rect {}; !shown_before.contains(hwnd) && !moved.contains(hwnd) && MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) == monitor && GetWindowRect(hwnd, &rect) && rect.bottom >= info.rcWork.bottom - tray_popup_margin) {
              moved.insert(hwnd);
              if (!SetWindowPos(hwnd, nullptr, rect.left, info.rcWork.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)) {
                BOOST_LOG(warning) << "Couldn't move tray window "sv << reinterpret_cast<std::uintptr_t>(hwnd) << " to the top: "sv << GetLastError();
              }
            }
          }
          std::this_thread::sleep_for(tray_popup_poll);
        }
      }}.detach();
    }

    // A real press: neither Start nor the tray icons offer an invoke pattern that opens them
    bool click_at(POINT point, bool right) {
      const auto left {GetSystemMetrics(SM_XVIRTUALSCREEN)};
      const auto top {GetSystemMetrics(SM_YVIRTUALSCREEN)};
      const auto width {GetSystemMetrics(SM_CXVIRTUALSCREEN)};
      const auto height {GetSystemMetrics(SM_CYVIRTUALSCREEN)};
      std::array<INPUT, 3> inputs {};
      for (auto &input : inputs) {
        input.type = INPUT_MOUSE;
        input.mi.dx = MulDiv(point.x - left, 65535, width - 1);
        input.mi.dy = MulDiv(point.y - top, 65535, height - 1);
      }
      inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
      inputs[1].mi.dwFlags = (right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN) | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
      inputs[2].mi.dwFlags = (right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP) | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
      return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
    }

    /**
     * @brief A window on a coherence screen, as one atlas update found it.
     */
    struct atlas_source_t {
      HWND hwnd;
      RECT frame;  ///< Visible frame on the desktop.
      RECT source;  ///< The visible frame relative to the window rectangle, which adds borders.
      bool popup;  ///< Shown over the others: one host_windows() leaves out, or a shell panel.
      bool shell_panel;  ///< Start or Search, cropped to its panel.
    };

    struct atlas_scan_t {
      std::vector<HMONITOR> monitors;  ///< The coherence screens'.
      HWND atlas_window;
      std::vector<atlas_source_t> sources;  ///< Front to back.
    };

    /**
     * @brief A window drawn in the atlas.
     */
    struct atlas_entry_t {
      HTHUMBNAIL thumbnail;
      SIZE frame;  ///< Frame size at the last update.
      RECT slot;  ///< Reserved for it, with room to grow; empty when it has none.
      RECT destination;  ///< Where its picture is.
      RECT source;  ///< What DWM draws of it.
      RECT panel;  ///< A shell panel's visible part on the desktop, empty while unknown.
      int panel_age;  ///< Updates until the panel is asked for again.
      double scale {1.0};  ///< Of its picture: the atlas's, or less for a popup that did not fit.
    };

    struct atlas_state_t {
      HWND window;
      RECT bounds;  ///< Where the atlas window is on the desktop.
      double scale;  ///< Of every window's picture.
      std::map<HWND, atlas_entry_t> entries;
      std::vector<HWND> order;  ///< First seen first: repacking keeps older windows in place.
      com_ptr_t<IUIAutomation> automation;  ///< Null where UI Automation is unavailable.
      int updates_until_growth;  ///< While shrunk: updates until a repack tries a larger scale.
    };

    // A shrunk atlas tries to grow back this often: windows that moved or changed size since may
    // fit at a larger scale, which no window leaving would show
    constexpr int atlas_growth_updates {coherence_screen_fps};

    // Start and Search hold the foreground (Search's box, also with Start alone) while open.
    // Start stays uncloaked through its closing animation, drawn without its backdrop, which
    // the client would show as a dark panel for a frame or two
    bool shell_panel_in_front() {
      std::array<wchar_t, 64> class_name {};
      const auto foreground {GetForegroundWindow()};
      return foreground != nullptr && GetClassNameW(foreground, class_name.data(), static_cast<int>(class_name.size())) > 0 && std::wstring_view {class_name.data()} == shell_panel_class;
    }

    BOOL CALLBACK collect_atlas_source(HWND hwnd, LPARAM scan_param) {
      auto &scan {*reinterpret_cast<atlas_scan_t *>(scan_param)};
      std::array<wchar_t, 64> class_name {};
      GetClassNameW(hwnd, class_name.data(), static_cast<int>(class_name.size()));
      BOOL cloaked {};
      DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
      RECT frame {};
      RECT window_rect {};
      const auto ex_style {GetWindowLongPtrW(hwnd, GWL_EXSTYLE)};

      // Click-through overlays take no input on the host; a window for one would take the
      // client's clicks
      if (
        hwnd != scan.atlas_window && IsWindowVisible(hwnd) && !cloaked && !IsIconic(hwnd) && !(ex_style & WS_EX_TRANSPARENT) &&
        std::ranges::find(scan.monitors, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL)) != scan.monitors.end() &&
        std::ranges::find(shell_window_classes, std::wstring_view {class_name.data()}) == shell_window_classes.end() &&
        GetWindowRect(hwnd, &window_rect) && SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
        frame.right > frame.left && frame.bottom > frame.top
      ) {
        if (const auto shell_panel {std::wstring_view {class_name.data()} == shell_panel_class}; !shell_panel || shell_panel_in_front()) {
          scan.sources.push_back({
            hwnd,
            frame,
            RECT {frame.left - window_rect.left, frame.top - window_rect.top, frame.right - window_rect.left, frame.bottom - window_rect.top},
            shell_panel || (ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0 || GetWindowTextLengthW(hwnd) == 0,
            shell_panel,
          });
        }
      }
      return TRUE;
    }

    LONG scaled(LONG length, double scale) {
      return std::max<LONG>(1, std::lround(length * scale));
    }

    bool overlaps(const RECT &first, const RECT &second) {
      return first.left < second.right && second.left < first.right && first.top < second.bottom && second.top < first.bottom;
    }

    /**
     * @brief The topmost, then leftmost, free spot of a size that touches the atlas edge or a
     *        taken slot.
     */
    std::optional<RECT> free_slot(const std::vector<RECT> &taken, LONG width, LONG height, LONG atlas_width, LONG atlas_height) {
      std::vector<POINT> corners {{0, 0}};
      for (const auto &slot : taken) {
        corners.insert(corners.end(), {POINT {slot.right, slot.top}, POINT {slot.left, slot.bottom}, POINT {slot.right, 0}, POINT {0, slot.bottom}});
      }
      std::ranges::sort(corners, [](const POINT &first, const POINT &second) {
        return std::tie(first.y, first.x) < std::tie(second.y, second.x);
      });

      std::optional<RECT> slot;
      for (const auto &corner : corners) {
        if (const RECT candidate {corner.x, corner.y, corner.x + width, corner.y + height}; !slot && candidate.right <= atlas_width && candidate.bottom <= atlas_height && std::ranges::none_of(taken, [&candidate](const RECT &other) {
              return overlaps(candidate, other);
            })) {
          slot = candidate;
        }
      }
      return slot;
    }

    // With room to grow where there is space for it, else exactly the size
    std::optional<RECT> place(const std::vector<RECT> &taken, LONG width, LONG height, LONG atlas_width, LONG atlas_height) {
      auto slot {free_slot(taken, std::min(width + atlas_headroom, atlas_width), std::min(height + atlas_headroom, atlas_height), atlas_width, atlas_height)};
      if (!slot) {
        slot = free_slot(taken, width, height, atlas_width, atlas_height);
      }
      return slot;
    }

    /**
     * @brief Lay every window out afresh, in first-seen order, at the largest scale they fit at.
     * @return Whether they fit at any scale; windows keep no slot when they do not.
     */
    bool repack_atlas(atlas_state_t &state, LONG atlas_width, LONG atlas_height) {
      // Tallest first: in the order seen, a small window can take the one spot a maximized one
      // fits in, and every window shrinks for it
      auto order {state.order};
      std::ranges::stable_sort(order, [&state](HWND first, HWND second) {
        const auto &first_frame {state.entries.at(first).frame};
        const auto &second_frame {state.entries.at(second).frame};
        return std::tie(first_frame.cy, first_frame.cx) > std::tie(second_frame.cy, second_frame.cx);
      });

      auto packed {false};
      for (auto scale {1.0}; !packed && scale >= atlas_min_scale; scale *= atlas_scale_step) {
        // Room to grow is given up before size: the first windows' headroom alone can keep the
        // last from fitting
        std::vector<RECT> taken;
        for (const auto headroom : {true, false}) {
          if (taken.size() < order.size()) {
            taken.clear();
            for (const auto hwnd : order) {
              const auto &frame {state.entries.at(hwnd).frame};
              const auto width {scaled(frame.cx, scale)};
              const auto height {scaled(frame.cy, scale)};
              if (const auto slot {headroom ? place(taken, width, height, atlas_width, atlas_height) : free_slot(taken, width, height, atlas_width, atlas_height)}; slot) {
                taken.push_back(*slot);
              } else {
                break;
              }
            }
          }
        }
        if (taken.size() == order.size()) {
          if (scale != state.scale) {
            std::string sizes;
            for (const auto hwnd : order) {
              sizes += std::format(" {}x{}", state.entries.at(hwnd).frame.cx, state.entries.at(hwnd).frame.cy);
            }
            BOOST_LOG(info) << "Coherence atlas: "sv << order.size() << " windows at scale "sv << scale << " in "sv << atlas_width << 'x' << atlas_height << ':' << sizes;
          }
          for (std::size_t index {0}; index < taken.size(); ++index) {
            auto &entry {state.entries.at(order[index])};
            entry.slot = taken[index];
            entry.scale = scale;
          }
          state.scale = scale;
          packed = true;
        }
      }
      if (!packed) {
        for (auto &[hwnd, entry] : state.entries) {
          entry.slot = {};
        }
      }
      return packed;
    }

    /**
     * @brief Draw the coherence screen's windows into the atlas as they are now: new windows get
     *        a slot, gone ones free theirs, and one that outgrew its slot moves to a free one.
     */
    void update_atlas(atlas_t &atlas, atlas_state_t &state) {
      std::vector<coherence_display_t> displays;
      {
        std::lock_guard lock {atlas.mutex};
        displays = atlas.displays;
      }

      // The atlas display moves when other displays come and go. Windows lie within its work
      // area: the display's own taskbar would cover them below it
      if (const auto bounds {output_rect(atlas.output_name)}; bounds && !EqualRect(&*bounds, &state.bounds)) {
        state.bounds = *bounds;
        SetWindowPos(state.window, HWND_TOPMOST, bounds->left, bounds->top, bounds->right - bounds->left, bounds->bottom - bounds->top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
      }
      auto pack_width {static_cast<LONG>(atlas.width)};
      auto pack_height {static_cast<LONG>(atlas.height)};
      if (MONITORINFO info {.cbSize = sizeof(MONITORINFO)}; GetMonitorInfoW(MonitorFromRect(&state.bounds, MONITOR_DEFAULTTONEAREST), &info)) {
        pack_width = std::min(pack_width, info.rcWork.right - info.rcWork.left);
        pack_height = std::min(pack_height, info.rcWork.bottom - info.rcWork.top);
      }

      // FindWindowEx, not EnumWindows: EnumWindows leaves out the immersive windows of Start and
      // Search
      const auto placed {place_displays(displays)};
      atlas_scan_t scan {{}, state.window, {}};
      for (const auto &display : placed) {
        scan.monitors.push_back(display.monitor);
      }
      if (!scan.monitors.empty()) {
        for (auto hwnd {FindWindowExW(nullptr, nullptr, nullptr, nullptr)}; hwnd != nullptr; hwnd = FindWindowExW(nullptr, hwnd, nullptr, nullptr)) {
          collect_atlas_source(hwnd, reinterpret_cast<LPARAM>(&scan));
        }
      }

      // Gone windows free their slots, and a shrunken atlas may grow back
      auto repack {false};
      for (auto entry {state.entries.begin()}; entry != state.entries.end();) {
        if (std::ranges::none_of(scan.sources, [hwnd = entry->first](const atlas_source_t &source) {
              return source.hwnd == hwnd;
            })) {
          DwmUnregisterThumbnail(entry->second.thumbnail);
          std::erase(state.order, entry->first);
          entry = state.entries.erase(entry);
          repack = repack || state.scale < 1.0;
        } else {
          ++entry;
        }
      }

      for (auto &source : scan.sources) {
        auto entry {state.entries.find(source.hwnd)};
        if (HTHUMBNAIL thumbnail {}; entry == state.entries.end() && SUCCEEDED(DwmRegisterThumbnail(state.window, source.hwnd, &thumbnail))) {
          entry = state.entries.emplace(source.hwnd, atlas_entry_t {thumbnail}).first;
          state.order.push_back(source.hwnd);
        }
        if (entry != state.entries.end()) {
          auto &[hwnd, drawn] {*entry};
          if (source.shell_panel && state.automation) {
            if (drawn.panel_age-- <= 0) {
              drawn.panel = automation_children_bounds(*state.automation, source.hwnd).value_or(RECT {});
              if (!IsRectEmpty(&drawn.panel)) {
                drawn.panel.top -= shell_panel_padding_top;
                drawn.panel.bottom += shell_panel_padding_bottom;
              }
              IntersectRect(&drawn.panel, &drawn.panel, &source.frame);
              drawn.panel_age = shell_panel_refresh_updates;
            }
            if (!IsRectEmpty(&drawn.panel)) {
              source.source = RECT {source.source.left + drawn.panel.left - source.frame.left, source.source.top + drawn.panel.top - source.frame.top, source.source.left + drawn.panel.right - source.frame.left, source.source.top + drawn.panel.bottom - source.frame.top};
              source.frame = drawn.panel;
            }
          }
          drawn.frame = SIZE {source.frame.right - source.frame.left, source.frame.bottom - source.frame.top};
          if (IsRectEmpty(&drawn.slot) || drawn.slot.right - drawn.slot.left < scaled(drawn.frame.cx, drawn.scale) || drawn.slot.bottom - drawn.slot.top < scaled(drawn.frame.cy, drawn.scale)) {
            std::vector<RECT> taken;
            for (const auto &[other_hwnd, other] : state.entries) {
              if (other_hwnd != hwnd && !IsRectEmpty(&other.slot)) {
                taken.push_back(other.slot);
              }
            }

            // A popup that does not fit shrinks alone: shrinking every window for a menu would
            // blur them all while it shows
            drawn.scale = state.scale;
            auto slot {place(taken, scaled(drawn.frame.cx, drawn.scale), scaled(drawn.frame.cy, drawn.scale), pack_width, pack_height)};
            while (!slot && source.popup && drawn.scale * atlas_scale_step >= atlas_min_scale) {
              drawn.scale *= atlas_scale_step;
              slot = place(taken, scaled(drawn.frame.cx, drawn.scale), scaled(drawn.frame.cy, drawn.scale), pack_width, pack_height);
            }
            drawn.slot = slot.value_or(RECT {});
            repack = repack || IsRectEmpty(&drawn.slot);
          }
        }
      }
      if (state.scale < 1.0 && --state.updates_until_growth <= 0) {
        state.updates_until_growth = atlas_growth_updates;
        repack = true;
      }
      if (repack && !repack_atlas(state, pack_width, pack_height)) {
        BOOST_LOG(warning) << "Coherence atlas "sv << atlas.output_name << " cannot hold its "sv << state.order.size() << " windows"sv;
      }

      std::vector<atlas_region_t> regions;
      for (const auto &source : scan.sources) {
        if (const auto entry {state.entries.find(source.hwnd)}; entry != state.entries.end()) {
          auto &drawn {entry->second};
          RECT destination {};
          if (!IsRectEmpty(&drawn.slot) && !(source.shell_panel && drawn.frame.cy < shell_panel_min_height)) {
            destination = RECT {drawn.slot.left, drawn.slot.top, drawn.slot.left + scaled(drawn.frame.cx, drawn.scale), drawn.slot.top + scaled(drawn.frame.cy, drawn.scale)};
          }
          if (!EqualRect(&destination, &drawn.destination) || !EqualRect(&source.source, &drawn.source)) {
            DWM_THUMBNAIL_PROPERTIES properties {};
            properties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            properties.rcDestination = destination;
            properties.rcSource = source.source;
            properties.fVisible = !IsRectEmpty(&destination);
            properties.opacity = 255;
            properties.fSourceClientAreaOnly = FALSE;
            if (const auto status {DwmUpdateThumbnailProperties(drawn.thumbnail, &properties)}; FAILED(status)) {
              BOOST_LOG(warning) << "Couldn't draw window "sv << reinterpret_cast<std::uintptr_t>(source.hwnd) << " into the coherence atlas: "sv << util::hex(status).to_string_view();
            }
            drawn.destination = destination;
            drawn.source = source.source;
          }
          if (!IsRectEmpty(&destination)) {
            const auto [client_x, client_y] {client_point(placed, POINT {source.frame.left, source.frame.top}, MonitorFromWindow(source.hwnd, MONITOR_DEFAULTTONEAREST))};
            regions.push_back({
              static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(source.hwnd)),
              host_rect_t {client_x, client_y, drawn.frame.cx, drawn.frame.cy},
              host_rect_t {destination.left, destination.top, destination.right - destination.left, destination.bottom - destination.top},
              source.popup,
            });
          }
        }
      }

      std::lock_guard lock {atlas.mutex};
      atlas.regions = std::move(regions);
    }

    // DWM draws thumbnails into a window of the thread that registers them, which has to pump
    // that window's messages
    void run_atlas(atlas_t &atlas) {
      platf::set_thread_name("coherence::atlas");
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
      const auto instance {GetModuleHandleW(nullptr)};
      static const auto registered {[instance]() {
        WNDCLASSW atlas_class {};
        atlas_class.lpfnWndProc = DefWindowProcW;
        atlas_class.hInstance = instance;
        atlas_class.lpszClassName = atlas_class_name;
        atlas_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        return RegisterClassW(&atlas_class) != 0;
      }()};

      const auto com_initialized {SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))};
      if (atlas_state_t state {registered ? CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, atlas_class_name, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr) : nullptr, RECT {}, 1.0}; state.window) {
        state.automation = make_automation();
        while (!atlas.stop) {
          for (MSG message {}; PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);) {
            DispatchMessageW(&message);
          }
          update_atlas(atlas, state);
          MsgWaitForMultipleObjects(0, nullptr, FALSE, atlas_interval_ms, QS_ALLINPUT);
        }
        for (const auto &[hwnd, entry] : state.entries) {
          DwmUnregisterThumbnail(entry.thumbnail);
        }
        DestroyWindow(state.window);
      } else {
        BOOST_LOG(error) << "Couldn't create the coherence atlas window for "sv << atlas.output_name << ": "sv << GetLastError();
      }
      if (com_initialized) {
        CoUninitialize();
      }
    }

    std::unique_ptr<atlas_t> make_atlas(const std::string &client_id, int width, int height, const std::vector<coherence_display_t> &displays) {
      std::unique_ptr<atlas_t> atlas;
      const auto identity {display_device::virtual_display_identity(client_id, "coherence-atlas")};
      const auto atlas_width {width * atlas_screens_wide};
      const auto atlas_height {height * atlas_screens_high};
      if (const auto output_name {virtual_display_create(atlas_width, atlas_height, coherence_screen_fps, identity)}; !output_name.empty()) {
        BOOST_LOG(info) << "Coherence atlas "sv << output_name << ", "sv << atlas_width << 'x' << atlas_height;
        atlas = std::make_unique<atlas_t>();
        atlas->output_name = output_name;
        atlas->width = atlas_width;
        atlas->height = atlas_height;
        atlas->displays = displays;
        atlas->thread = std::thread {run_atlas, std::ref(*atlas)};
      }
      return atlas;
    }

    void destroy_atlas(atlas_t &atlas) {
      atlas.stop = true;
      if (atlas.thread.joinable()) {
        atlas.thread.join();
      }
      virtual_display_destroy(atlas.output_name);
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

  bool secure_desktop_active() {
    auto active {false};
    if (auto desktop {OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS)}; desktop != nullptr) {
      std::array<wchar_t, 64> name {};
      GetUserObjectInformationW(desktop, UOI_NAME, name.data(), static_cast<DWORD>(name.size() * sizeof(wchar_t)), nullptr);
      active = std::wstring_view {name.data()} != L"Default";
      CloseDesktop(desktop);
    } else {
      // A server running as the user may not open Winlogon's desktop at all
      active = GetLastError() == ERROR_ACCESS_DENIED;
    }
    return active;
  }

  std::string coherence_capture_output(const std::string &output_name) {
    auto capture {output_name};
    if (!output_name.empty() && secure_desktop_active()) {
      std::lock_guard lock {coherence_mutex};
      if (std::ranges::any_of(coherence_clients, [&output_name](const auto &client) {
            return client.second.atlas && client.second.atlas->output_name == output_name;
          })) {
        if (const auto primary {primary_display()}; !primary.empty()) {
          capture = primary;
          BOOST_LOG(info) << "The secure desktop shows: coherence atlas "sv << output_name << " captures the primary display "sv << capture;
        }
      }
    }
    return capture;
  }

  std::pair<int, int> desktop_origin() {
    return {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
  }

  bool host_cursor_hidden() {
    CURSORINFO info {.cbSize = sizeof(CURSORINFO)};
    return GetCursorInfo(&info) && !(info.flags & CURSOR_SHOWING);
  }

  bool close_host_window(std::uint64_t id) {
    const auto hwnd {window_handle(id)};
    return IsWindow(hwnd) && PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE;
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

  bool resize_host_window(std::uint64_t id, int width, int height, std::optional<std::pair<int, int>> position) {
    auto resized {false};
    const auto hwnd {window_handle(id)};
    RECT frame {};
    RECT window_rect {};

    // Sizes are what capture delivers, the visible frame; the window rectangle adds the
    // invisible resize borders around it. Asynchronous: a client resizing live sends many, and a
    // synchronous one waits for the window's own thread on the one HTTPS thread
    if (const auto area {host_window_work_area(id)}; area && width > 0 && height > 0 && GetWindowRect(hwnd, &window_rect) && SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
      const auto border_left {frame.left - window_rect.left};
      const auto border_top {frame.top - window_rect.top};
      const auto border_width {(window_rect.right - window_rect.left) - (frame.right - frame.left)};
      const auto border_height {(window_rect.bottom - window_rect.top) - (frame.bottom - frame.top)};
      if (IsZoomed(hwnd)) {
        ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE);
      }
      const auto left {position ? area->x + position->first - border_left : 0};
      const auto top {position ? area->y + position->second - border_top : 0};
      const UINT flags {SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | (position ? 0u : static_cast<UINT>(SWP_NOMOVE))};
      resized = SetWindowPos(hwnd, nullptr, left, top, width + border_width, height + border_height, flags) != FALSE;
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
    std::string png;
    if (const auto shortcut {pinned_app_shortcut(id)}; !shortcut.empty()) {
      png = file_icon_png(shortcut);
    }
    if (png.empty()) {
      BOOST_LOG(warning) << "Expected an icon for pinned app ["sv << id << "], found none"sv;
    }
    return png;
  }

  std::string host_window_icon(std::uint64_t id) {
    std::string png;
    DWORD process_id {};
    if (const auto hwnd {window_handle(id)}; IsWindow(hwnd) && GetWindowThreadProcessId(hwnd, &process_id) != 0) {
      if (auto process {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id)}; process != nullptr) {
        std::array<wchar_t, MAX_PATH> path {};
        if (auto size {static_cast<DWORD>(path.size())}; QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
          png = file_icon_png(std::filesystem::path {std::wstring {path.data(), size}});
        }
        CloseHandle(process);
      }
    }
    if (png.empty()) {
      BOOST_LOG(warning) << "Expected an icon for host window "sv << id << ", found none"sv;
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

  std::optional<host_rect_t> host_window_work_area(std::uint64_t id) {
    std::optional<host_rect_t> area;
    if (const auto hwnd {window_handle(id)}; IsWindow(hwnd)) {
      if (MONITORINFO info {.cbSize = sizeof(MONITORINFO)}; GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
        area = host_rect_t {info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top};
      }
    }
    return area;
  }

  std::optional<host_rect_t> host_window_position(std::uint64_t id) {
    std::optional<host_rect_t> position;
    if (const auto area {host_window_work_area(id)}; area) {
      if (RECT frame {}; SUCCEEDED(DwmGetWindowAttribute(window_handle(id), DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        position = host_rect_t {frame.left - area->x, frame.top - area->y, frame.right - frame.left, frame.bottom - frame.top};
      }
    }
    return position;
  }

  bool coherence_screens(const std::string &client_id, const std::vector<host_rect_t> &screens) {
    std::lock_guard lock {coherence_mutex};

    // A client whose screens changed (Dock shown, a screen added) gets screens of the new layout;
    // its windows keep the homes they had before the first ones, and the atlas stays, as the
    // client's stream of it does
    std::map<HWND, WINDOWPLACEMENT> home;
    std::unique_ptr<atlas_t> atlas;
    auto client {coherence_clients.find(client_id)};
    if (client != coherence_clients.end() && !std::ranges::equal(client->second.displays, screens, [](const coherence_display_t &display, const host_rect_t &screen) {
          return display.client.x == screen.x && display.client.y == screen.y && display.client.width == screen.width && display.client.height == screen.height;
        })) {
      home = std::move(client->second.home);
      atlas = std::move(client->second.atlas);
      release_primary_display(client->second.displays.front().output_name);
      for (const auto &display : client->second.displays) {
        virtual_display_destroy(display.output_name);
      }
      coherence_clients.erase(client);
      client = coherence_clients.end();
    }
    if (client == coherence_clients.end() && !screens.empty() && std::ranges::all_of(screens, [](const host_rect_t &screen) {
          return screen.width > 0 && screen.height > 0;
        })) {
      const auto taskbar_height {primary_taskbar_height()};
      std::vector<coherence_display_t> displays;
      for (const auto &screen : screens) {
        const auto profile {displays.empty() ? std::string {"coherence"} : std::format("coherence-{}", displays.size())};
        if (const auto output_name {virtual_display_create(screen.width, screen.height + taskbar_height, coherence_screen_fps, display_device::virtual_display_identity(client_id, profile))}; !output_name.empty()) {
          BOOST_LOG(info) << "Coherence screen "sv << output_name << " for a "sv << screen.width << 'x' << screen.height << " client screen at "sv << screen.x << ',' << screen.y;
          displays.push_back({output_name, screen});
        }
      }
      if (displays.size() == screens.size()) {
        arrange_displays(displays, taskbar_height);
        if (atlas) {
          std::lock_guard atlas_lock {atlas->mutex};
          atlas->displays = displays;
        } else {
          atlas = make_atlas(client_id, screens.front().width, screens.front().height, displays);
        }
        // After the atlas display: Windows lays out a new set of displays as it last saved it
        claim_primary_display(displays.front().output_name);
        client = coherence_clients.emplace(client_id, coherence_client_t {displays, std::move(home), std::move(atlas)}).first;
      } else {
        BOOST_LOG(warning) << "Expected a coherence screen for each of the client's "sv << screens.size() << " screens, made "sv << displays.size();
        for (const auto &display : displays) {
          virtual_display_destroy(display.output_name);
        }
      }
      encoders_unaffected_by_display_change();
    }
    if (atlas) {
      destroy_atlas(*atlas);
    }

    if (client != coherence_clients.end()) {
      client->second.last_request = std::chrono::steady_clock::now();
      const auto placed {place_displays(client->second.displays)};
      for (const auto &window : host_windows()) {
        gather_window(window_handle(window.id), placed, client->second.home);
      }
    }
    return client != coherence_clients.end();
  }

  void release_coherence_screen(const std::string &client_id) {
    std::lock_guard lock {coherence_mutex};
    if (const auto client {coherence_clients.find(client_id)}; client != coherence_clients.end()) {
      if (client->second.atlas) {
        destroy_atlas(*client->second.atlas);
      }
      send_windows_home(client->second);
      release_primary_display(client->second.displays.front().output_name);
      for (const auto &display : client->second.displays) {
        virtual_display_destroy(display.output_name);
      }
      coherence_clients.erase(client);
      encoders_unaffected_by_display_change();
    }
  }

  std::pair<int, int> coherence_client_point(const std::string &client_id, int x, int y) {
    auto point {std::make_pair(x, y)};
    std::lock_guard lock {coherence_mutex};
    if (const auto client {coherence_clients.find(client_id)}; client != coherence_clients.end()) {
      point = client_point(place_displays(client->second.displays), POINT {x, y});
    }
    return point;
  }

  std::optional<std::pair<int, int>> coherence_host_point(const std::string &client_id, int x, int y) {
    std::optional<std::pair<int, int>> point;
    std::lock_guard lock {coherence_mutex};
    if (const auto client {coherence_clients.find(client_id)}; client != coherence_clients.end()) {
      point = host_point(place_displays(client->second.displays), x, y);
    }
    return point;
  }

  std::optional<coherence_atlas_t> coherence_atlas(const std::string &client_id) {
    std::optional<coherence_atlas_t> layout;
    std::lock_guard lock {coherence_mutex};
    if (const auto client {coherence_clients.find(client_id)}; client != coherence_clients.end() && client->second.atlas) {
      auto &atlas {*client->second.atlas};
      std::lock_guard atlas_lock {atlas.mutex};
      layout = coherence_atlas_t {atlas.output_name, atlas.width, atlas.height, atlas.regions};
    }
    return layout;
  }

  std::optional<std::pair<float, float>> coherence_input_point(const std::string &output_name, float x, float y, float width, float height) {
    std::optional<std::pair<float, float>> point;
    if (!output_name.empty() && width > 0 && height > 0) {
      std::lock_guard lock {coherence_mutex};
      for (const auto &[client_id, client] : coherence_clients) {
        if (client.atlas && client.atlas->output_name == output_name && !client.displays.empty()) {
          // The client's reference size is that of all its screens together
          auto right {0};
          auto bottom {0};
          for (const auto &display : client.displays) {
            right = std::max(right, display.client.x + display.client.width);
            bottom = std::max(bottom, display.client.y + display.client.height);
          }
          const auto client_x {static_cast<int>(std::clamp(x, 0.0f, width) * right / width)};
          const auto client_y {static_cast<int>(std::clamp(y, 0.0f, height) * bottom / height)};

          // On the secure desktop the stream shows the primary display, fitted into the stream
          // and that again into the client's primary screen, each centered as large as it fits
          MONITORINFO info {.cbSize = sizeof(MONITORINFO)};
          if (secure_desktop_active() && GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info)) {
            const auto &screen {client.displays.front().client};
            const auto stream_width {static_cast<float>(client.atlas->width)};
            const auto stream_height {static_cast<float>(client.atlas->height)};
            const auto primary_width {static_cast<float>(info.rcMonitor.right - info.rcMonitor.left)};
            const auto primary_height {static_cast<float>(info.rcMonitor.bottom - info.rcMonitor.top)};
            const auto client_scale {std::min(screen.width / stream_width, screen.height / stream_height)};
            const auto frame_x {(client_x - screen.x - (screen.width - stream_width * client_scale) / 2) / client_scale};
            const auto frame_y {(client_y - screen.y - (screen.height - stream_height * client_scale) / 2) / client_scale};
            const auto stream_scale {std::min(stream_width / primary_width, stream_height / primary_height)};
            const auto host_x {(frame_x - (stream_width - primary_width * stream_scale) / 2) / stream_scale};
            const auto host_y {(frame_y - (stream_height - primary_height * stream_scale) / 2) / stream_scale};
            point = std::make_pair(info.rcMonitor.left + std::clamp(host_x, 0.0f, primary_width - 1), info.rcMonitor.top + std::clamp(host_y, 0.0f, primary_height - 1));
          } else if (const auto host {host_point(place_displays(client.displays), client_x, client_y)}; host) {
            point = std::make_pair(static_cast<float>(host->first), static_cast<float>(host->second));
          }
        }
      }
    }
    return point;
  }

  bool open_coherence_start_menu(const std::string &client_id) {
    auto opened {false};
    std::optional<RECT> screen_rect;
    {
      std::lock_guard lock {coherence_mutex};
      if (const auto client {coherence_clients.find(client_id)}; client != coherence_clients.end() && !client->second.displays.empty()) {
        screen_rect = output_rect(client->second.displays.front().output_name);
      }
    }

    const auto com_initialized {SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))};
    if (screen_rect) {
      const auto monitor {MonitorFromRect(&*screen_rect, MONITOR_DEFAULTTONULL)};
      // The coherence screen is primary, with the main taskbar, whose Start button UI Automation
      // does not list; the shell opens Start there on SC_TASKLIST, as on the Win key
      if (const auto tray {FindWindowW(L"Shell_TrayWnd", nullptr)}; tray != nullptr && MonitorFromWindow(tray, MONITOR_DEFAULTTONULL) == monitor) {
        opened = PostMessageW(tray, WM_SYSCOMMAND, SC_TASKLIST, 0) != FALSE;
      } else {
        HWND secondary {};
        for (auto candidate {FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr)}; candidate != nullptr; candidate = FindWindowExW(nullptr, candidate, L"Shell_SecondaryTrayWnd", nullptr)) {
          if (MonitorFromWindow(candidate, MONITOR_DEFAULTTONULL) == monitor) {
            secondary = candidate;
          }
        }
        if (const auto automation {secondary != nullptr ? make_automation() : nullptr}; automation) {
          if (const auto button {automation_element_bounds(*automation, secondary, L"StartButton")}; button) {
            opened = click_at({(button->left + button->right) / 2, (button->top + button->bottom) / 2}, false);
          }
        }
      }
    }
    if (com_initialized) {
      CoUninitialize();
    }
    if (!opened) {
      BOOST_LOG(warning) << "Expected the Start button of the client's coherence screen to press, found "sv << (screen_rect ? "none"sv : "no coherence screen"sv);
    }
    return opened;
  }

  std::vector<host_tray_icon_t> host_tray_icons() {
    // A window over the taskbar covers the icons the picture is taken of; their last picture
    // stands in then, by place
    static std::mutex cache_mutex;
    static std::vector<std::string> last_pictures;

    std::vector<host_tray_icon_t> icons;
    // UI Automation and WIC are COM, and an HTTP worker thread has no apartment of its own
    const auto com_initialized {SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))};
    if (const auto automation {make_automation()}; automation) {
      const auto tray {FindWindowW(L"Shell_TrayWnd", nullptr)};
      const auto buttons {tray_buttons(*automation)};
      std::lock_guard lock {cache_mutex};
      last_pictures.resize(buttons.size());
      for (std::size_t index {0}; index < buttons.size(); ++index) {
        const auto &image {buttons[index].image};
        if (const POINT center {(image.left + image.right) / 2, (image.top + image.bottom) / 2}; GetAncestor(WindowFromPoint(center), GA_ROOT) == tray) {
          last_pictures[index] = tray_image_png(image);
        }
        icons.push_back({utf_utils::to_utf8(buttons[index].name), last_pictures[index]});
      }
    }
    if (com_initialized) {
      CoUninitialize();
    }
    return icons;
  }

  bool click_host_tray_icon(std::size_t index, bool right) {
    auto clicked {false};
    const auto com_initialized {SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))};
    if (const auto automation {make_automation()}; automation) {
      if (const auto buttons {tray_buttons(*automation)}; index < buttons.size()) {
        const auto &button {buttons[index].button};
        const POINT center {(button.left + button.right) / 2, (button.top + button.bottom) / 2};
        auto shown_before {visible_windows()};
        clicked = click_at(center, right);
        if (clicked) {
          move_tray_popups_up(std::move(shown_before), MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST));
        }
      } else {
        BOOST_LOG(warning) << "Expected a pinned tray icon at "sv << index << ", the taskbar has "sv << buttons.size();
      }
    }
    if (com_initialized) {
      CoUninitialize();
    }
    return clicked;
  }

  std::vector<std::string> idle_coherence_clients(std::chrono::seconds timeout) {
    std::vector<std::string> clients;
    const auto now {std::chrono::steady_clock::now()};
    std::lock_guard lock {coherence_mutex};
    for (const auto &[client_id, client] : coherence_clients) {
      if (now - client.last_request > timeout) {
        clients.push_back(client_id);
      }
    }
    return clients;
  }
}  // namespace platf
