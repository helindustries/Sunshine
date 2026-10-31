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
#include <mutex>
#include <set>
#include <string>
#include <thread>

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
     * @brief Make a display the primary one, keeping the arrangement of the others around it.
     * @details The primary display is the one at 0,0, so every attached display moves by the new
     *          primary's offset; the changes are staged and applied together.
     */
    void make_primary(const wchar_t *device_name) {
      DEVMODEW mode {};
      mode.dmSize = sizeof(mode);
      if (EnumDisplaySettingsW(device_name, ENUM_CURRENT_SETTINGS, &mode)) {
        const auto offset = mode.dmPosition;
        DISPLAY_DEVICEW device {};
        device.cb = sizeof(device);
        for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index) {
          if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && wcscmp(device.DeviceName, device_name) != 0) {
            DEVMODEW other {};
            other.dmSize = sizeof(other);
            if (EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &other)) {
              other.dmPosition.x -= offset.x;
              other.dmPosition.y -= offset.y;
              other.dmFields = DM_POSITION;
              ChangeDisplaySettingsExW(device.DeviceName, &other, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
            }
          }
          device.cb = sizeof(device);
        }
        mode.dmPosition = {0, 0};
        mode.dmFields = DM_POSITION;
        const auto staged = ChangeDisplaySettingsExW(device_name, &mode, nullptr, CDS_SET_PRIMARY | CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        const auto applied = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        if (staged != DISP_CHANGE_SUCCESSFUL || applied != DISP_CHANGE_SUCCESSFUL) {
          BOOST_LOG(warning) << "SudoVDA: couldn't make the new display primary ("sv << staged << ", "sv << applied << ')';
        }
      }
    }

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
          if (watchdog.Timeout) {
            std::thread {[interval = std::chrono::milliseconds {watchdog.Timeout * 1000 / 3}]() {
              while (SUDOVDA::PingDriver(sudovda_handle)) {
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
}  // namespace platf
