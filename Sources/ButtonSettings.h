/* Copyright (c) 2018-present, Fred Emmott
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file.
 */
#pragma once

#include <AudioDevices/AudioDevices.h>

#include <nlohmann/json.hpp>
#include <string>

using namespace FredEmmott::Audio;

enum class DeviceMatchStrategy {
  ID,
  Fuzzy,
};

struct ConfiguredDevice {
  AudioDeviceInfo info;
  std::string icon;
};

struct ButtonSettings {
  AudioDeviceDirection direction = AudioDeviceDirection::INPUT;
  AudioDeviceRole role = AudioDeviceRole::DEFAULT;
  AudioDeviceRole secondaryRole = AudioDeviceRole::DEFAULT;
  std::vector<ConfiguredDevice> devices;
  DeviceMatchStrategy matchStrategy = DeviceMatchStrategy::ID;
  bool setBothRoles = false;

  // Get volatile ID for a device (handles fuzzy matching)
  std::string GetVolatileDeviceID(size_t index) const;
};

void from_json(const nlohmann::json&, ButtonSettings&);
void to_json(nlohmann::json&, const ButtonSettings&);
