/**
 * @file src/platform/windows/virtual_display.cpp
 * @brief Virtual displays through the SudoVDA driver, one per streaming session.
 */
// platform includes
#include <initguid.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

// standard includes
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/utf_utils.h"
#include "third-party/sudovda/sudovda.h"

using namespace std::literals;

namespace platf {
  namespace {
    constexpr auto max_virtual_displays = 16;
    constexpr auto display_appear_timeout = 5s;
    constexpr auto display_extend_after = 1500ms;

    std::mutex sudovda_mutex;
    HANDLE sudovda_handle = INVALID_HANDLE_VALUE;
    /**
     * @brief A virtual display we made: its slot and the mode it was made for.
     */
    struct sudovda_display_t {
      int slot;  ///< Index of its fixed monitor GUID.
      int width;  ///< Requested width in pixels.
      int height;  ///< Requested height in pixels.
      int fps;  ///< Requested refresh rate.
    };

    std::map<std::string, sudovda_display_t> sudovda_displays;  ///< By output name.

    std::map<std::uint32_t, int> slot_of_identity;  ///< The slot each screen identity had, kept in slots_file().
    bool slot_records_loaded {false};

    /**
     * @brief Where a profile's slot is recorded: its slot is its monitor GUID, and Windows keeps a
     *        monitor's arrangement by it, so the record must outlive restarts.
     */
    std::filesystem::path slots_file() {
      return std::filesystem::path {config::sunshine.config_file}.parent_path() / "virtual_display_slots.txt";
    }

    void load_slot_records() {
      if (!slot_records_loaded) {
        slot_records_loaded = true;
        std::ifstream file {slots_file()};
        std::uint32_t identity {};
        int slot {};
        while (file >> identity >> slot) {
          slot_of_identity[identity] = slot;
        }
      }
    }

    void save_slot_records() {
      std::ofstream file {slots_file(), std::ios::trunc};
      for (const auto &[identity, slot] : slot_of_identity) {
        file << identity << ' ' << slot << '\n';
      }
      if (!file) {
        BOOST_LOG(warning) << "SudoVDA: couldn't record the profiles' display slots in "sv << slots_file().string();
      }
    }

    /**
     * @brief The monitor GUID of a slot. Fixed GUIDs let a restart remove displays a crashed run left
     *        behind: SudoVDA's watchdog is driver-wide, so another server's pings would keep them.
     */
    GUID slot_guid(int slot) {
      // {5c1e7a30-2b6d-4f0e-9a4b-53756e5348xx}, xx = slot
      return {0x5c1e7a30, 0x2b6d, 0x4f0e, {0x9a, 0x4b, 0x53, 0x75, 0x6e, 0x53, 0x48, static_cast<unsigned char>(slot)}};
    }

    /**
     * @brief Point SudoVDA's rendering at the configured adapter, so capture and encode share it.
     */
    void set_render_adapter(const std::string &adapter_name) {
      Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
      if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        const auto wide_name {utf_utils::from_utf8(adapter_name)};
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        auto found = false;
        for (UINT index = 0; !found && SUCCEEDED(factory->EnumAdapters(index, &adapter)); ++index) {
          if (DXGI_ADAPTER_DESC desc; SUCCEEDED(adapter->GetDesc(&desc)) && wide_name == desc.Description) {
            found = SUDOVDA::SetRenderAdapter(sudovda_handle, desc.AdapterLuid);
          }
        }
        if (!found) {
          BOOST_LOG(warning) << "SudoVDA: couldn't render on adapter ["sv << adapter_name << "], it keeps its default"sv;
        }
      }
    }

    /**
     * @brief Give a virtual display the mode it was created for, where Windows restored another.
     * @details Extending the desktop brings back the mode Windows saved for the monitor, not the
     *          one SudoVDA was asked for.
     */
    void set_display_mode(const wchar_t *device_name, int width, int height, int fps) {
      DEVMODEW mode {};
      mode.dmSize = sizeof(mode);
      if (EnumDisplaySettingsW(device_name, ENUM_CURRENT_SETTINGS, &mode) &&
          (mode.dmPelsWidth != static_cast<DWORD>(width) || mode.dmPelsHeight != static_cast<DWORD>(height) || mode.dmDisplayFrequency != static_cast<DWORD>(fps))) {
        mode.dmPelsWidth = width;
        mode.dmPelsHeight = height;
        mode.dmDisplayFrequency = fps;
        mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        auto status = ChangeDisplaySettingsExW(device_name, &mode, nullptr, CDS_UPDATEREGISTRY, nullptr);
        if (status == DISP_CHANGE_BADMODE) {
          mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
          status = ChangeDisplaySettingsExW(device_name, &mode, nullptr, CDS_UPDATEREGISTRY, nullptr);
        }
        if (status != DISP_CHANGE_SUCCESSFUL) {
          std::string offered;
          DEVMODEW listed {};
          listed.dmSize = sizeof(listed);
          for (DWORD index = 0; offered.size() < 400 && EnumDisplaySettingsW(device_name, index, &listed); ++index) {
            offered += ' ' + std::to_string(listed.dmPelsWidth) + 'x' + std::to_string(listed.dmPelsHeight) + '@' + std::to_string(listed.dmDisplayFrequency);
          }
          BOOST_LOG(warning) << "SudoVDA: couldn't set "sv << width << 'x' << height << '@' << fps << " on the new display ("sv << status << "), it offers:"sv << offered;
        }
      }
    }

    /**
     * @brief Move the active displays in one SetDisplayConfig: ChangeDisplaySettingsExW fails with
     *        DISP_CHANGE_FAILED for SudoVDA displays. Not saved to the database, where Windows
     *        would take a virtual display's place, primary too, whenever it reappears.
     * @param place A display's new position by its device name and current position.
     * @return The SetDisplayConfig status.
     */
    LONG move_active_displays(const std::function<POINTL(const std::wstring &, POINTL)> &place) {
      UINT32 path_count = 0;
      UINT32 mode_count = 0;
      auto status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      if (status == ERROR_SUCCESS) {
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
      }
      for (UINT32 index = 0; status == ERROR_SUCCESS && index < path_count; ++index) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), paths[index].sourceInfo.adapterId, paths[index].sourceInfo.id};
        if (const auto mode_index = paths[index].sourceInfo.modeInfoIdx; mode_index < mode_count && DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) {
          auto &position = modes[mode_index].sourceMode.position;
          position = place(source.viewGdiDeviceName, position);
        }
      }
      if (status == ERROR_SUCCESS) {
        status = SetDisplayConfig(path_count, paths.data(), mode_count, modes.data(), SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
      }
      return status;
    }

    /**
     * @brief Make a display the primary one, keeping the arrangement of the others around it.
     * @details The primary display is the one at 0,0, so every display moves by its offset.
     */
    void make_primary(const wchar_t *device_name) {
      DEVMODEW mode {};
      mode.dmSize = sizeof(mode);
      if (!EnumDisplaySettingsW(device_name, ENUM_CURRENT_SETTINGS, &mode)) {
        BOOST_LOG(warning) << "Couldn't make display "sv << utf_utils::to_utf8(device_name) << " primary: it is not active"sv;
      } else if (const auto status = move_active_displays([offset = mode.dmPosition](const std::wstring &, POINTL position) {
                   return POINTL {position.x - offset.x, position.y - offset.y};
                 });
                 status != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "Couldn't make display "sv << utf_utils::to_utf8(device_name) << " primary ("sv << status << ')';
      }
    }

    constexpr auto primary_attempts = 10;
    constexpr auto primary_attempt_delay = 200ms;

    std::mutex primary_mutex;
    std::wstring home_primary;  ///< The primary display before the first claim, primary again after the last.
    std::vector<std::wstring> primary_claims;  ///< Displays streamed from, newest last; the newest is primary.

    /**
     * @brief Open the driver once and keep its watchdog fed; SudoVDA removes its displays when it is not pinged.
     * @return Whether the driver is open.
     */
    bool open_sudovda() {
      if (sudovda_handle == INVALID_HANDLE_VALUE) {
        if (const auto handle = SUDOVDA::OpenDevice(&SUDOVDA::SUVDA_INTERFACE_GUID); handle == INVALID_HANDLE_VALUE) {
          BOOST_LOG(error) << "SudoVDA: driver not found, is the SudoVDA virtual display driver installed?"sv;
        } else if (!SUDOVDA::CheckProtocolCompatible(handle)) {
          BOOST_LOG(error) << "SudoVDA: the installed driver speaks an incompatible protocol version"sv;
          CloseHandle(handle);
        } else if (SUDOVDA::VIRTUAL_DISPLAY_GET_WATCHDOG_OUT watchdog; !SUDOVDA::GetWatchdogTimeout(handle, watchdog)) {
          BOOST_LOG(error) << "SudoVDA: couldn't read the driver's watchdog timeout"sv;
          CloseHandle(handle);
        } else {
          sudovda_handle = handle;
          for (auto slot = 0; slot < max_virtual_displays; ++slot) {
            SUDOVDA::RemoveVirtualDisplay(sudovda_handle, slot_guid(slot));
          }
          // SudoVDA opens its handle overlapped but passes no OVERLAPPED, so two calls in flight on
          // one handle wake each other's waits and the kernel then writes into a returned frame
          // (access violation at KERNELBASE!DeviceIoControl+0x9e). The watchdog pings on its own
          // handle; the others are used under sudovda_mutex, one call at a time.
          if (watchdog.Timeout) {
            std::thread {[interval = std::chrono::milliseconds {watchdog.Timeout * 1000 / 3}, ping_handle = SUDOVDA::OpenDevice(&SUDOVDA::SUVDA_INTERFACE_GUID)]() {
              while (SUDOVDA::PingDriver(ping_handle)) {
                std::this_thread::sleep_for(interval);
              }
              BOOST_LOG(error) << "SudoVDA: driver ping failed, its virtual displays will be removed"sv;
            }}.detach();
          }
          if (!config::video.adapter_name.empty()) {
            set_render_adapter(config::video.adapter_name);
          }
        }
      }
      return sudovda_handle != INVALID_HANDLE_VALUE;
    }
  }  // namespace

  std::string virtual_display_create(int width, int height, int fps, std::uint32_t identity) {
    std::string output_name;
    std::lock_guard lock {sudovda_mutex};

    load_slot_records();
    std::set<int> used;
    for (const auto &[name, display] : sudovda_displays) {
      used.insert(display.slot);
    }
    const auto records {slot_of_identity.size()};
    const auto slot {display_device::choose_virtual_display_slot(identity, used, slot_of_identity, max_virtual_displays)};
    if (slot_of_identity.size() != records) {
      save_slot_records();
    }
    BOOST_LOG(debug) << "SudoVDA: screen identity "sv << identity << " takes slot "sv << slot;

    if (slot < 0) {
      BOOST_LOG(error) << "SudoVDA: all "sv << max_virtual_displays << " virtual display slots are in use"sv;
    } else if (open_sudovda()) {
      const auto guid {slot_guid(slot)};
      // Windows keeps a monitor's arrangement by its EDID serial (the monitor ID in
      // GraphicsDrivers\Configuration), not by the GUID or the device instance, which SudoVDA
      // numbers in creation order. SudoVDA keeps 13 characters of it.
      const auto serial {identity != 0 ? std::format("{:08X}", identity) : std::format("Slot{:02}", slot)};
      SUDOVDA::VIRTUAL_DISPLAY_ADD_OUT added;
      if (SUDOVDA::AddVirtualDisplay(sudovda_handle, width, height, fps, guid, "Sunshine", serial.c_str(), added)) {
        // The display reaches the active display configuration a while after the driver adds it,
        // about 1.1 s on an Intel Iris Xe alone. Windows keeps it detached instead when the
        // topology it saved for this set of displays has no place for it, so after a while the
        // topology is extended, as Win+P "Extend" would
        wchar_t device_name[CCHDEVICENAME] {};
        const auto started = std::chrono::steady_clock::now();
        auto extended = false;
        while (!SUDOVDA::GetAddedDisplayName(added, device_name) && std::chrono::steady_clock::now() - started < display_appear_timeout) {
          if (!extended && std::chrono::steady_clock::now() - started >= display_extend_after) {
            extended = true;
            const auto status = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
            BOOST_LOG(info) << "SudoVDA: the new display stayed detached, extending the desktop to it: "sv << (status == ERROR_SUCCESS ? "done"sv : "failed"sv) << " ("sv << status << ')';
          }
          std::this_thread::sleep_for(50ms);
        }

        if (device_name[0] != L'\0') {
          set_display_mode(device_name, width, height, fps);
          // Extending also put back the saved modes of the displays other sessions stream
          if (extended) {
            for (const auto &[name, display] : sudovda_displays) {
              set_display_mode(utf_utils::from_utf8(name).c_str(), display.width, display.height, display.fps);
            }
          }
          // Windows draws the sign-in and lock screens on the primary display only, so a client
          // connecting there would see an empty desktop
          if (platf::console_awaits_sign_in()) {
            BOOST_LOG(info) << "SudoVDA: the console awaits sign-in, making the new display primary"sv;
            make_primary(device_name);
          }
          output_name = utf_utils::to_utf8(device_name);
          sudovda_displays.emplace(output_name, sudovda_display_t {slot, width, height, fps});
          BOOST_LOG(debug) << "SudoVDA: "sv << output_name << " appeared after "sv << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << " ms"sv;
        } else {
          BOOST_LOG(error) << "SudoVDA: the new virtual display never appeared, removing it"sv;
          SUDOVDA::RemoveVirtualDisplay(sudovda_handle, guid);
        }
      } else {
        BOOST_LOG(error) << "SudoVDA: couldn't add a "sv << width << 'x' << height << '@' << fps << " virtual display"sv;
      }
    }
    return output_name;
  }

  void virtual_display_destroy(const std::string &output_name) {
    std::lock_guard lock {sudovda_mutex};
    if (const auto display = sudovda_displays.find(output_name); display != sudovda_displays.end()) {
      if (!SUDOVDA::RemoveVirtualDisplay(sudovda_handle, slot_guid(display->second.slot))) {
        BOOST_LOG(warning) << "SudoVDA: couldn't remove virtual display ["sv << output_name << ']';
      }
      sudovda_displays.erase(display);
    } else {
      BOOST_LOG(warning) << "Not a SudoVDA virtual display: ["sv << output_name << ']';
    }
  }

  bool move_displays(const std::map<std::string, POINTL> &positions) {
    // The primary display stays at 0,0, or Windows refuses the layout (ERROR_INVALID_PARAMETER):
    // moving it moves everything else the other way
    const auto primary = positions.find(primary_display());
    const auto shift = primary != positions.end() ? primary->second : POINTL {0, 0};
    const auto status = move_active_displays([&positions, shift](const std::wstring &name, POINTL position) {
      const auto wanted = positions.find(utf_utils::to_utf8(name));
      const auto moved = wanted != positions.end() ? wanted->second : position;
      return POINTL {moved.x - shift.x, moved.y - shift.y};
    });
    if (status != ERROR_SUCCESS) {
      BOOST_LOG(warning) << "Couldn't move "sv << positions.size() << " displays ("sv << status << ')';
    }
    return status == ERROR_SUCCESS;
  }

  std::string primary_display() {
    std::string name;
    if (MONITORINFOEXW primary {}; (primary.cbSize = sizeof(MONITORINFOEXW), GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &primary))) {
      name = utf_utils::to_utf8(primary.szDevice);
    }
    return name;
  }

  void claim_primary_display(const std::string &output_name) {
    std::lock_guard lock {primary_mutex};
    const auto name {utf_utils::from_utf8(output_name)};
    if (primary_claims.empty()) {
      // A virtual display a saved layout made primary is gone with the stream, no place to return to
      const auto primary = primary_display();
      std::lock_guard sudovda_lock {sudovda_mutex};
      home_primary = sudovda_displays.contains(primary) ? std::wstring {} : utf_utils::from_utf8(primary);
    }
    std::erase(primary_claims, name);
    primary_claims.push_back(name);
    if (primary_display() != output_name) {
      BOOST_LOG(info) << "Making streamed display "sv << output_name << " primary"sv;
      make_primary(name.c_str());
      if (const auto primary = primary_display(); primary != output_name) {
        BOOST_LOG(warning) << "Expected "sv << output_name << " to be the primary display, it is "sv << primary;
      }
    }
  }

  void release_primary_display(const std::string &output_name) {
    std::lock_guard lock {primary_mutex};
    if (std::erase(primary_claims, utf_utils::from_utf8(output_name)) != 0) {
      const auto next {primary_claims.empty() ? home_primary : primary_claims.back()};
      if (const auto next_name {utf_utils::to_utf8(next)}; !next.empty() && primary_display() != next_name) {
        BOOST_LOG(info) << "Making display "sv << next_name << " primary again"sv;
        // Right after a display is removed the set is still changing (ERROR_GEN_FAILURE), and
        // Windows then lays out the displays left as it saved them, its own primary included
        make_primary(next.c_str());
        for (auto attempt = 1; attempt < primary_attempts && primary_display() != next_name; ++attempt) {
          std::this_thread::sleep_for(primary_attempt_delay);
          make_primary(next.c_str());
        }
        if (const auto primary = primary_display(); primary != next_name) {
          BOOST_LOG(warning) << "Expected "sv << next_name << " to be the primary display again, it is "sv << primary;
        }
      }
      if (primary_claims.empty()) {
        home_primary.clear();
      }
    }
  }
}  // namespace platf
