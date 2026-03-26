/* Copyright (c) 2018-present, Fred Emmott
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file.
 */

#include "ButtonSettings.h"

#include <StreamDeckSDK/ESDLogger.h>

#include <regex>

#include "audio_json.h"

using json = nlohmann::json;

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
    const auto& devicesJson = j.at("devices");
    if (devicesJson.is_array()) {
      for (const auto& deviceJson : devicesJson) {
        ConfiguredDevice cd;
        if (deviceJson.is_string()) {
          cd.info.id = deviceJson;
        } else {
          cd.info = deviceJson.get<AudioDeviceInfo>();
          if (deviceJson.contains("icon")) {
            cd.icon = deviceJson.at("icon").get<std::string>();
          }
        }
        bs.devices.push_back(cd);
      }
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

  // Serialize devices with per-device icon fields so icon assignments
  // survive round-trips through C++ SetSettings (AudioDeviceInfo::to_json
  // doesn't write icon, so we add it here).
  json devicesJson = json::array();
  for (const auto& cd : bs.devices) {
    json deviceJson = cd.info;// uses AudioDeviceInfo::to_json
    if (!cd.icon.empty()) {
      deviceJson["icon"] = cd.icon;
    }
    devicesJson.push_back(deviceJson);
  }

  j = {
    {"direction", bs.direction},
    {"role", roleStr},
    {"devices", devicesJson},
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
  return GetVolatileID(devices[index].info, matchStrategy);
}
