/**
 * @file src/platform/linux/wlr_virtual_display.cpp
 * @brief Virtual displays on Sway and Hyprland as headless outputs, captured by wlr-screencopy.
 * @details Both compositors create and remove headless outputs over their IPC, and keep them
 *          until removed; the session then captures the output by name with the wlroots backend.
 */
// standard includes
#include <array>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

// lib includes
#include <lizardbyte/common/env.h>
#include <nlohmann/json.hpp>

// local includes
#include "misc.h"
#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace platf {
  namespace {
    /**
     * @brief The compositor whose IPC makes the headless outputs.
     */
    enum class compositor_e {
      none,  ///< Neither Sway nor Hyprland.
      sway,  ///< Sway, through swaymsg.
      hyprland,  ///< Hyprland, through hyprctl.
    };

    std::mutex wlr_mutex;
    std::set<std::string> wlr_outputs;  ///< Headless outputs we created.
    int hyprland_output_count = 0;

    compositor_e running_compositor() {
      auto compositor = compositor_e::none;
      if (!lizardbyte::common::get_env("SWAYSOCK").empty()) {
        compositor = compositor_e::sway;
      } else if (!lizardbyte::common::get_env("HYPRLAND_INSTANCE_SIGNATURE").empty()) {
        compositor = compositor_e::hyprland;
      }
      return compositor;
    }

    /**
     * @brief Run an IPC command line; every argument in it is generated here, never taken from a client.
     * @return Exit status, and the command's output in output.
     */
    int run(const std::string &command, std::string &output) {
      output.clear();
      auto status = -1;
      if (auto *pipe = popen((command + " 2>&1").c_str(), "r"); pipe != nullptr) {
        std::array<char, 4096> buffer;
        while (const auto read = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
          output.append(buffer.data(), read);
        }
        status = pclose(pipe);
      }
      return status;
    }

    std::set<std::string> sway_output_names() {
      std::set<std::string> names;
      std::string output;
      if (run("swaymsg -r -t get_outputs", output) == 0) {
        for (const auto &sway_output : nlohmann::json::parse(output, nullptr, false)) {
          if (sway_output.contains("name")) {
            names.insert(sway_output["name"].get<std::string>());
          }
        }
      }
      return names;
    }

    std::string sway_create(int width, int height, int fps) {
      std::string name;
      std::string output;
      // Sway numbers headless outputs from a counter it shares with its fallback output, so the
      // new one is whichever name was not there before
      const auto before = sway_output_names();
      if (run("swaymsg create_output", output) != 0) {
        BOOST_LOG(error) << "[wlr] swaymsg create_output failed: "sv << output;
      } else {
        for (const auto &candidate : sway_output_names()) {
          if (!before.contains(candidate)) {
            name = candidate;
          }
        }
        const auto mode {std::to_string(width) + 'x' + std::to_string(height) + '@' + std::to_string(fps) + "Hz"};
        if (name.empty()) {
          BOOST_LOG(error) << "[wlr] Sway created an output but it did not show up in get_outputs"sv;
        } else if (run("swaymsg -- output " + name + " mode --custom " + mode, output) != 0) {
          BOOST_LOG(error) << "[wlr] Sway could not set "sv << name << " to "sv << mode << ": "sv << output;
          run("swaymsg output " + name + " unplug", output);
          name.clear();
        }
      }
      return name;
    }

    std::string hyprland_create(int width, int height, int fps) {
      auto name {"sunshine-"s + std::to_string(++hyprland_output_count)};
      std::string output;
      const auto mode {std::to_string(width) + 'x' + std::to_string(height) + '@' + std::to_string(fps)};
      if (run("hyprctl output create headless " + name, output) != 0) {
        BOOST_LOG(error) << "[wlr] hyprctl output create failed: "sv << output;
        name.clear();
      } else {
        // A Lua config (hyprland.lua) rejects keyword and takes the rule through eval instead
        run("hyprctl keyword monitor " + name + ',' + mode + ",auto,1", output);
        if (output.find("non-legacy") != std::string::npos) {
          run("hyprctl eval 'hl.monitor({output=\"" + name + "\", mode=\"" + mode + "\", position=\"auto\", scale=1})'", output);
        }
        if (output.find("ok") == std::string::npos) {
          BOOST_LOG(warning) << "[wlr] Hyprland may not have set "sv << name << " to "sv << mode << ": "sv << output;
        }
      }
      return name;
    }
  }  // namespace

  bool wlr_virtual_display_available() {
    return running_compositor() != compositor_e::none;
  }

  std::string wlr_virtual_display_create(int width, int height, int fps) {
    std::lock_guard lock {wlr_mutex};
    std::string name;
    switch (running_compositor()) {
      case compositor_e::sway:
        name = sway_create(width, height, fps);
        break;
      case compositor_e::hyprland:
        name = hyprland_create(width, height, fps);
        break;
      case compositor_e::none:
        break;
    }
    if (!name.empty()) {
      wlr_outputs.insert(name);
      BOOST_LOG(info) << "[wlr] created headless output "sv << name << " ("sv << width << 'x' << height << '@' << fps << "Hz)"sv;
    }
    return name;
  }

  void wlr_virtual_display_destroy(const std::string &name) {
    std::lock_guard lock {wlr_mutex};
    std::string output;
    if (!wlr_outputs.contains(name)) {
      BOOST_LOG(warning) << "[wlr] not a headless output of ours: ["sv << name << ']';
    } else if (const auto compositor = running_compositor(); compositor == compositor_e::sway) {
      run("swaymsg output " + name + " unplug", output);
      wlr_outputs.erase(name);
    } else if (compositor == compositor_e::hyprland) {
      run("hyprctl output remove " + name, output);
      wlr_outputs.erase(name);
    }
  }
}  // namespace platf
