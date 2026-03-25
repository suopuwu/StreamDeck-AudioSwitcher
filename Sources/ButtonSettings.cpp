/* Copyright (c) 2018-present, Fred Emmott
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file.
 */

#include "ButtonSettings.h"

#include <StreamDeckSDK/ESDLogger.h>

#include <regex>

#include "audio_json.h"

NLOHMANN_JSON_SERIALIZE_ENUM(
  DeviceMatchStrategy,
  {
    {DeviceMatchStrategy::ID, "ID"},
    {DeviceMatchStrategy::Fuzzy, "Fuzzy"},
  });

void from_json(const nlohmann::json& j, ButtonSettings& bs) {
  if (!j.contains("direction")) {
    return;
  }

  bs.direction = j.at("direction");

  if (j.contains("role")) {
    if (j.at("role") == "both") {
      bs.setBothRoles = true;
      bs.role = AudioDeviceRole::DEFAULT;
      bs.secondaryRole = AudioDeviceRole::COMMUNICATION;
    } else {
      bs.role = j.at("role");
    }
  }

  if (j.contains("devices")) {
    bs.devices = j.at("devices").get<std::vector<AudioDeviceInfo>>();
  } else {
    // Migrate from old primary/secondary format
    if (j.contains("primary")) {
      const auto& primary = j.at("primary");
      AudioDeviceInfo device;
      if (primary.is_string()) {
        device.id = primary;
      } else {
        device = primary;
      }
      bs.devices.push_back(device);
    }
    if (j.contains("secondary")) {
      const auto& secondary = j.at("secondary");
      AudioDeviceInfo device;
      if (secondary.is_string()) {
        device.id = secondary;
      } else {
        device = secondary;
      }
      bs.devices.push_back(device);
    }
  }

  if (j.contains("matchStrategy")) {
    bs.matchStrategy = j.at("matchStrategy");
  }
}

void to_json(nlohmann::json& j, const ButtonSettings& bs) {
  std::string roleStr;
  if (bs.setBothRoles) {
    roleStr = "both";
  } else if (bs.role == AudioDeviceRole::COMMUNICATION) {
    roleStr = "communication";
  } else {
    roleStr = "default";
  }

  j = {
    {"direction", bs.direction},
    {"role", roleStr},
    {"devices", bs.devices},
    {"matchStrategy", bs.matchStrategy},
  };
}

namespace {

std::string FuzzifyInterface(const std::string& name) {
  // Windows likes to replace "Foo" with "2- Foo"
  const std::regex pattern{"^([0-9]+- )?(.+)$"};
  std::smatch captures;
  if (!std::regex_match(name, captures, pattern)) {
    return name;
  }
  return captures[2];
}

std::string GetVolatileID(
  const AudioDeviceInfo& device,
  DeviceMatchStrategy strategy) {
  if (device.id.empty()) {
    return {};
  }

  if (strategy == DeviceMatchStrategy::ID) {
    return device.id;
  }

  if (GetAudioDeviceState(device.id) == AudioDeviceState::CONNECTED) {
    return device.id;
  }

  const auto fuzzyInterface = FuzzifyInterface(device.interfaceName);
  ESDDebug(
    "Looking for a fuzzy match: {} -> {}",
    device.interfaceName,
    fuzzyInterface);

  for (const auto& [otherID, other] : GetAudioDeviceList(device.direction)) {
    if (other.state != AudioDeviceState::CONNECTED) {
      continue;
    }
    const auto otherFuzzyInterface = FuzzifyInterface(other.interfaceName);
    ESDDebug("Trying {} -> {}", other.interfaceName, otherFuzzyInterface);
    if (
      fuzzyInterface == otherFuzzyInterface
      && device.endpointName == other.endpointName) {
      ESDDebug(
        "Fuzzy device match for {}/{}",
        device.interfaceName,
        device.endpointName);
      return otherID;
    }
  }
  ESDDebug(
    "Failed fuzzy match for {}/{}", device.interfaceName, device.endpointName);
  return device.id;
}
}// namespace

std::string ButtonSettings::GetVolatileDeviceID(size_t index) const {
  if (index >= devices.size()) {
    return {};
  }
  return GetVolatileID(devices[index], matchStrategy);
}
