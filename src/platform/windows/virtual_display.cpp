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
#include <map>
#include <mutex>
#include <string>
#include <thread>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/windows/utf_utils.h"
#include "third-party/sudovda/sudovda.h"

using namespace std::literals;

namespace platf {
  namespace {
    constexpr auto max_virtual_displays = 16;

    std::mutex sudovda_mutex;
    HANDLE sudovda_handle = INVALID_HANDLE_VALUE;
    std::map<std::string, int> sudovda_displays;  ///< Slot by output name.

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

  std::string virtual_display_create(int width, int height, int fps) {
    std::string output_name;
    std::lock_guard lock {sudovda_mutex};

    auto slot = 0;
    while (slot < max_virtual_displays && std::ranges::any_of(sudovda_displays, [slot](const auto &display) { return display.second == slot; })) {
      ++slot;
    }

    if (slot == max_virtual_displays) {
      BOOST_LOG(error) << "SudoVDA: all "sv << max_virtual_displays << " virtual display slots are in use"sv;
    } else if (open_sudovda()) {
      const auto guid {slot_guid(slot)};
      SUDOVDA::VIRTUAL_DISPLAY_ADD_OUT added;
      if (SUDOVDA::AddVirtualDisplay(sudovda_handle, width, height, fps, guid, "Sunshine", "Sunshine", added)) {
        // The display appears in the display configuration a moment after the driver adds it
        wchar_t device_name[CCHDEVICENAME] {};
        for (auto delay = 20ms; !SUDOVDA::GetAddedDisplayName(added, device_name) && delay <= 640ms; delay *= 2) {
          std::this_thread::sleep_for(delay);
        }

        if (device_name[0] != L'\0') {
          output_name = utf_utils::to_utf8(device_name);
          sudovda_displays.emplace(output_name, slot);
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
      if (!SUDOVDA::RemoveVirtualDisplay(sudovda_handle, slot_guid(display->second))) {
        BOOST_LOG(warning) << "SudoVDA: couldn't remove virtual display ["sv << output_name << ']';
      }
      sudovda_displays.erase(display);
    } else {
      BOOST_LOG(warning) << "Not a SudoVDA virtual display: ["sv << output_name << ']';
    }
  }
}  // namespace platf
