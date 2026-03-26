/* Copyright (c) 2018-present, Fred Emmott
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file.
 */
#pragma once

#include <AudioDevices/AudioDevices.h>

#include <map>
#include <nlohmann/json.hpp>
#include <string>

using namespace FredEmmott::Audio;

enum class DeviceMatchStrategy {
  ID,
  Fuzzy,
};

struct ButtonSettings {
  AudioDeviceDirection direction = AudioDeviceDirection::INPUT;
  AudioDeviceRole role = AudioDeviceRole::DEFAULT;
  AudioDeviceRole secondaryRole = AudioDeviceRole::DEFAULT;
  std::vector<AudioDeviceInfo> devices;
  std::map<std::string, std::string> deviceIcons;// device ID -> icon name
  std::map<std::string, std::string> customImages;// icon name -> base64 data
  DeviceMatchStrategy matchStrategy = DeviceMatchStrategy::ID;
  bool setBothRoles = false;

  // Get volatile ID for a device (handles fuzzy matching)
  std::string GetVolatileDeviceID(size_t index) const;
  // Get icon preference for a device
  std::string GetDeviceIcon(const std::string& deviceId, size_t index) const;
};

void from_json(const nlohmann::json&, ButtonSettings&);
void to_json(nlohmann::json&, const ButtonSettings&);
