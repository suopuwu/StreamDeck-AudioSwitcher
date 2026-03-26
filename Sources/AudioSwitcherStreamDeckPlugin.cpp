//==============================================================================
/**
@file       AudioSwitcherStreamDeckPlugin.cpp

@brief      CPU plugin

@copyright  (c) 2018, Corsair Memory, Inc.
@copyright  (c) 2018-present, Fred Emmott.
      This source code is licensed under the MIT-style license found in the
LICENSE file.

**/
//==============================================================================

#include "AudioSwitcherStreamDeckPlugin.h"

#include <AudioDevices/AudioDevices.h>
#include <StreamDeckSDK/EPLJSONUtils.h>
#include <StreamDeckSDK/ESDConnectionManager.h>
#include <StreamDeckSDK/ESDLogger.h>

#include <atomic>
#include <functional>
#include <mutex>

#ifdef _MSC_VER
#include <objbase.h>
#endif

#include <cctype>
#include <fstream>
#include <sstream>

#include "audio_json.h"

// URL decode helper function
std::string URLDecode(const std::string& encoded) {
  std::string decoded;
  for (size_t i = 0; i < encoded.length(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.length()) {
      std::string hexStr = encoded.substr(i + 1, 2);
      char* end;
      long value = std::strtol(hexStr.c_str(), &end, 16);
      if (end == hexStr.c_str() + 2) {
        decoded += static_cast<char>(value);
        i += 2;
        continue;
      }
    }
    if (encoded[i] == '+') {
      decoded += ' ';
    } else {
      decoded += encoded[i];
    }
  }
  return decoded;
}

using namespace FredEmmott::Audio;
using json = nlohmann::json;

namespace {
constexpr std::string_view SET_ACTION_ID{
  "com.fredemmott.audiooutputswitch.set"};
constexpr std::string_view TOGGLE_ACTION_ID{
  "com.fredemmott.audiooutputswitch.toggle"};

bool FillAudioDeviceInfo(AudioDeviceInfo& di) {
  if (di.id.empty()) {
    return false;
  }
  if (!di.displayName.empty()) {
    return false;
  }

  const auto devices = GetAudioDeviceList(di.direction);
  if (!devices.contains(di.id)) {
    return false;
  }
  di = devices.at(di.id);
  return true;
}

}// namespace

AudioSwitcherStreamDeckPlugin::AudioSwitcherStreamDeckPlugin() {
#ifdef _MSC_VER
  CoInitializeEx(
    NULL, COINIT_MULTITHREADED);// initialize COM for the main thread
#endif
  mCallbackHandle = AddDefaultAudioDeviceChangeCallback(
    std::bind_front(
      &AudioSwitcherStreamDeckPlugin::OnDefaultDeviceChanged, this));
}

AudioSwitcherStreamDeckPlugin::~AudioSwitcherStreamDeckPlugin() {
  mCallbackHandle = {};
}

void AudioSwitcherStreamDeckPlugin::OnDefaultDeviceChanged(
  AudioDeviceDirection direction,
  AudioDeviceRole role,
  const std::string& device) {
  std::scoped_lock lock(mVisibleContextsMutex);
  for (const auto& [context, button] : mButtons) {
    if (button.settings.direction != direction) {
      continue;
    }
    if (button.settings.setBothRoles) {
      // Match if either role matches
      if (
        button.settings.role != role && button.settings.secondaryRole != role) {
        continue;
      }
    } else if (button.settings.role != role) {
      continue;
    }
    UpdateState(context, device);
  }
}

void AudioSwitcherStreamDeckPlugin::KeyDownForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  // No action on key down
}

void AudioSwitcherStreamDeckPlugin::KeyUpForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  ESDDebug("{}: {}", __FUNCTION__, inPayload.dump());
  std::scoped_lock lock(mVisibleContextsMutex);

  if (!inPayload.contains("settings")) {
    return;
  }
  auto& settings = mButtons[inContext].settings;
  settings = inPayload.at("settings");
  FillButtonDeviceInfo(inContext);

  // Get list of configured devices
  const auto& deviceList = settings.devices;
  if (deviceList.empty()) {
    ESDDebug("No devices configured");
    return;
  }

  // Find the next device in the list
  auto currentDevice
    = GetDefaultAudioDeviceID(settings.direction, settings.role);
  if (currentDevice.empty() && settings.setBothRoles) {
    currentDevice
      = GetDefaultAudioDeviceID(settings.direction, settings.secondaryRole);
  }

  // Find current device index
  int currentIndex = 0;
  for (size_t i = 0; i < deviceList.size(); ++i) {
    if (deviceList[i].id == currentDevice) {
      currentIndex = i;
      break;
    }
  }

  // Cycle through devices until we find one that's connected
  int nextIndex = (currentIndex + 1) % deviceList.size();
  int startIndex = nextIndex;
  std::string deviceID;

  while (true) {
    deviceID = settings.GetVolatileDeviceID(nextIndex);

    if (!deviceID.empty()) {
      const auto deviceState = GetAudioDeviceState(deviceID);
      if (deviceState == AudioDeviceState::CONNECTED) {
        // Found a connected device
        break;
      }
    }

    // Move to next device and check if we've cycled through all
    nextIndex = (nextIndex + 1) % deviceList.size();
    if (nextIndex == startIndex) {
      // We've cycled through all devices and none are connected
      ESDDebug("No connected devices available");
      mConnectionManager->ShowAlertForContext(inContext);
      return;
    }
  }

  if (deviceID.empty()) {
    ESDDebug("Doing nothing, no device ID");
    return;
  }

  ESDDebug(
    "Found next connected device at index {}. Setting device to {}",
    nextIndex,
    deviceID);
  SetDefaultAudioDeviceID(settings.direction, settings.role, deviceID);
  if (settings.setBothRoles) {
    SetDefaultAudioDeviceID(
      settings.direction, settings.secondaryRole, deviceID);
  }
}

void AudioSwitcherStreamDeckPlugin::WillAppearForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  std::scoped_lock lock(mVisibleContextsMutex);
  // Remember the context
  mVisibleContexts.insert(inContext);
  auto& button = mButtons[inContext];
  button = {inAction, inContext};

  if (!inPayload.contains("settings")) {
    return;
  }
  button.settings = inPayload.at("settings");

  UpdateState(inContext);
  FillButtonDeviceInfo(inContext);
}

void AudioSwitcherStreamDeckPlugin::FillButtonDeviceInfo(
  const std::string& context) {
  auto& settings = mButtons.at(context).settings;

  bool needsUpdate = false;
  for (auto& device : settings.devices) {
    if (FillAudioDeviceInfo(device)) {
      needsUpdate = true;
    }
  }

  if (needsUpdate) {
    ESDDebug("Backfilling settings to {}", json(settings).dump());
    mConnectionManager->SetSettings(settings, context);
  }
}

void AudioSwitcherStreamDeckPlugin::WillDisappearForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  // Remove the context
  std::scoped_lock lock(mVisibleContextsMutex);
  mVisibleContexts.erase(inContext);
  mButtons.erase(inContext);
}

void AudioSwitcherStreamDeckPlugin::SendToPlugin(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  json outPayload;

  const auto event = EPLJSONUtils::GetStringByName(inPayload, "event");
  ESDDebug("Received event {}", event);

  if (event == "getDeviceList") {
    const auto outputList = GetAudioDeviceList(AudioDeviceDirection::OUTPUT);
    const auto inputList = GetAudioDeviceList(AudioDeviceDirection::INPUT);
    mConnectionManager->SendToPropertyInspector(
      inAction,
      inContext,
      json({
        {"event", event},
        {"outputDevices", outputList},
        {"inputDevices", inputList},
      }));
    return;
  }

  if (event == "addDevice") {
    std::scoped_lock lock(mVisibleContextsMutex);
    if (mButtons.contains(inContext)) {
      auto& settings = mButtons[inContext].settings;
      if (inPayload.contains("device")) {
        AudioDeviceInfo newDevice = inPayload.at("device");
        FillAudioDeviceInfo(newDevice);
        settings.devices.push_back(newDevice);
        mConnectionManager->SetSettings(settings, inContext);
      }
    }
    return;
  }

  if (event == "removeDevice") {
    std::scoped_lock lock(mVisibleContextsMutex);
    if (mButtons.contains(inContext) && inPayload.contains("index")) {
      auto& settings = mButtons[inContext].settings;
      size_t index = inPayload.at("index");
      if (index < settings.devices.size()) {
        settings.devices.erase(settings.devices.begin() + index);
        mConnectionManager->SetSettings(settings, inContext);
        UpdateState(inContext);
      }
    }
    return;
  }

  if (event == "readFile") {
    // Handle file upload from property inspector
    if (!inPayload.contains("fileName")) {
      return;
    }

    const auto fileName = EPLJSONUtils::GetStringByName(inPayload, "fileName");
    const auto decodedFileName = URLDecode(fileName);
    ESDDebug("Reading file: {} (decoded: {})", fileName, decodedFileName);

    // Read file and convert to base64
    std::ifstream file(decodedFileName, std::ios::binary);
    if (!file.is_open()) {
      ESDDebug("Failed to open file: {}", decodedFileName);
      return;
    }

    std::vector<uint8_t> fileData(
      (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    // Simple base64 encoding
    const char* base64_chars
      = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string base64Data;

    for (size_t i = 0; i < fileData.size(); i += 3) {
      uint32_t b1 = fileData[i];
      uint32_t b2 = (i + 1 < fileData.size()) ? fileData[i + 1] : 0;
      uint32_t b3 = (i + 2 < fileData.size()) ? fileData[i + 2] : 0;

      uint32_t bits = (b1 << 16) | (b2 << 8) | b3;

      base64Data += base64_chars[(bits >> 18) & 0x3F];
      base64Data += base64_chars[(bits >> 12) & 0x3F];
      base64Data
        += (i + 1 < fileData.size()) ? base64_chars[(bits >> 6) & 0x3F] : '=';
      base64Data += (i + 2 < fileData.size()) ? base64_chars[bits & 0x3F] : '=';
    }

    mConnectionManager->SendToPropertyInspector(
      inAction,
      inContext,
      json({
        {"event", "fileData"},
        {"fileName", fileName},
        {"base64Data", base64Data},
      }));
    return;
  }

  if (event == "updateState") {
    // Update the button state when icon selection changes
    // The UI sends the new icon and device index directly to avoid timing
    // issues
    {
      std::scoped_lock lock(mVisibleContextsMutex);
      if (mButtons.contains(inContext)) {
        if (inPayload.contains("icon") && inPayload.contains("deviceIndex")) {
          const auto newIcon = EPLJSONUtils::GetStringByName(inPayload, "icon");
          const auto deviceIndex = inPayload.at("deviceIndex").get<size_t>();
          ESDDebug(
            "updateState: device index {}, changing icon to: {}",
            deviceIndex,
            newIcon);

          auto& settings = mButtons[inContext].settings;
          if (deviceIndex < settings.devices.size()) {
            const auto& device = settings.devices[deviceIndex];
            // Update the deviceIcons map with the new icon
            settings.deviceIcons[device.id] = newIcon;
            ESDDebug(
              "updateState: mapped device.id='{}' to icon='{}'",
              device.id,
              newIcon);
            ESDDebug(
              "updateState: deviceIcons now has {} entries",
              settings.deviceIcons.size());

            // Verify the mapping exists
            if (
              settings.customImages.find(newIcon)
              != settings.customImages.end()) {
              ESDDebug(
                "updateState: custom image '{}' is present in customImages",
                newIcon);
            } else {
              ESDDebug(
                "updateState: WARNING - custom image '{}' NOT FOUND in "
                "customImages!",
                newIcon);
            }
          }
        }
      }
    }
    // Persist the updated settings back to StreamDeck
    const auto button = mButtons[inContext];
    mConnectionManager->SetSettings(button.settings, inContext);
    // Call UpdateState without holding the lock (it will acquire its own)
    UpdateState(inContext);
    return;
  }

  if (event == "deleteImage") {
    // Handle custom image deletion
    if (!inPayload.contains("imageName")) {
      return;
    }

    const auto imageName
      = EPLJSONUtils::GetStringByName(inPayload, "imageName");
    ESDDebug("deleteImage event: removing custom image: {}", imageName);

    {
      std::scoped_lock lock(mVisibleContextsMutex);
      if (mButtons.contains(inContext)) {
        auto& settings = mButtons[inContext].settings;
        // Remove from customImages map
        settings.customImages.erase(imageName);
        ESDDebug("Deleted custom image: {}", imageName);
      }
    }

    // Also notify the property inspector that deletion is complete
    mConnectionManager->SendToPropertyInspector(
      inAction,
      inContext,
      json({
        {"event", "imageDeleted"},
        {"imageName", imageName},
      }));
    return;
  }
}

void AudioSwitcherStreamDeckPlugin::UpdateState(
  const std::string& context,
  const std::string& optionalDefaultDevice) {
  const auto button = mButtons[context];
  const auto action = button.action;
  const auto settings = button.settings;
  auto activeDevice = optionalDefaultDevice.empty()
    ? GetDefaultAudioDeviceID(settings.direction, settings.role)
    : optionalDefaultDevice;

  if (activeDevice.empty() && settings.setBothRoles) {
    activeDevice
      = GetDefaultAudioDeviceID(settings.direction, settings.secondaryRole);
  }

  const auto& deviceList = settings.devices;
  if (deviceList.empty()) {
    mConnectionManager->ShowAlertForContext(context);
    return;
  }

  // Find which device is active and set state accordingly
  std::scoped_lock lock(mVisibleContextsMutex);
  for (size_t i = 0; i < deviceList.size(); ++i) {
    if (deviceList[i].id == activeDevice) {
      // Get the user-selected icon for this device
      const auto icon = settings.GetDeviceIcon(deviceList[i].id, i);
      ESDDebug(
        "UpdateState: Device {} (index {}) icon: {}",
        deviceList[i].id,
        i,
        icon);

      // Check if this is a custom image
      const auto customIt = settings.customImages.find(icon);
      if (customIt != settings.customImages.end()) {
        // Custom image - use SetImage directly
        const auto& base64Data = customIt->second;
        ESDDebug(
          "Setting custom image: {} (size: {})", icon, base64Data.size());
        mConnectionManager->SetImage(
          base64Data, context, kESDSDKTarget_HardwareAndSoftware);
        return;
      }

      // For built-in icons, map device index directly to state (0-9)
      // This ensures consistent cycling without duplicates
      int state = static_cast<int>(i % 10);
      ESDDebug("Setting state to {} for device at index {}", state, i);
      mConnectionManager->SetState(state, context);
      return;
    }
  }

  // Active device not in list
  mConnectionManager->ShowAlertForContext(context);
}

void AudioSwitcherStreamDeckPlugin::DeviceDidConnect(
  const std::string& inDeviceID,
  const json& inDeviceInfo) {
  // Nothing to do
}

void AudioSwitcherStreamDeckPlugin::DeviceDidDisconnect(
  const std::string& inDeviceID) {
  // Nothing to do
}

void AudioSwitcherStreamDeckPlugin::DidReceiveGlobalSettings(
  const json& inPayload) {
}

void AudioSwitcherStreamDeckPlugin::DidReceiveSettings(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  WillAppearForAction(inAction, inContext, inPayload, inDeviceID);
}
