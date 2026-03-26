//==============================================================================
/**
@file       AudioSwitcherStreamDeckPlugin.cpp

@brief      Audio Switcher StreamDeck Plugin

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

#include <functional>
#include <mutex>

#ifdef _MSC_VER
#include <objbase.h>
#endif

#include <cctype>
#include <fstream>

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

  // Determine the plugin directory (next to the executable).
  {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    auto pos = dir.find_last_of("\\\/");
    if (pos != std::string::npos) {
      dir = dir.substr(0, pos);
    }
    mPluginDir = dir;
    mCustomImagesPath = dir + "\\custom_images.json";
  }
  LoadCustomImages();
  LoadBuiltInIcons();

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
  // Preserve in-memory deviceIcons before overwriting,
  // as they may not yet be persisted back from the most recent SetSettings
  const auto savedDeviceIcons = settings.deviceIcons;
  settings = inPayload.at("settings");
  if (settings.deviceIcons.empty() && !savedDeviceIcons.empty()) {
    settings.deviceIcons = savedDeviceIcons;
  }
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

  // Update the button visual BEFORE changing the audio device.
  // This prevents the "flash" of the default icon — otherwise the
  // visual only updates when the async OnDefaultDeviceChanged callback
  // fires, which races with DidReceiveSettings overwriting state.
  // Use the stored device ID (not the volatile one) so UpdateState
  // can match it against deviceList entries.
  UpdateState(inContext, deviceList[nextIndex].id);

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
  std::scoped_lock lock(mVisibleContextsMutex);
  mButtons.erase(inContext);
}

void AudioSwitcherStreamDeckPlugin::SendToPlugin(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
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
    // Update the button state when icon selection changes.
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
            settings.deviceIcons[device.id] = newIcon;
          }
        }
      }
    }
    UpdateState(inContext);
    return;
  }

  if (event == "storeCustomImage") {
    // Store a custom image sent from the property inspector.
    // Images are stored globally (not per-button) and saved to disk.
    if (inPayload.contains("name") && inPayload.contains("data")) {
      const auto name = EPLJSONUtils::GetStringByName(inPayload, "name");
      const auto data = EPLJSONUtils::GetStringByName(inPayload, "data");
      ESDDebug("storeCustomImage: {} ({} bytes)", name, data.size());
      mCustomImages[name] = data;
      SaveCustomImages();
    }
    return;
  }

  if (event == "getCustomImages") {
    // Send the full custom images catalog to the property inspector.
    // Each image is sent individually to avoid a single huge message.
    for (const auto& [name, data] : mCustomImages) {
      mConnectionManager->SendToPropertyInspector(
        inAction,
        inContext,
        json({
          {"event", "customImage"},
          {"name", name},
          {"data", data},
        }));
    }
    // Signal that all images have been sent
    mConnectionManager->SendToPropertyInspector(
      inAction,
      inContext,
      json({
        {"event", "customImagesDone"},
      }));
    return;
  }

  if (event == "deleteImage") {
    if (!inPayload.contains("imageName")) {
      return;
    }
    const auto imageName
      = EPLJSONUtils::GetStringByName(inPayload, "imageName");
    ESDDebug("deleteImage: removing {}", imageName);
    mCustomImages.erase(imageName);
    SaveCustomImages();

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
  // Snapshot the button state under the mutex before doing anything else.
  std::scoped_lock lock(mVisibleContextsMutex);
  if (!mButtons.contains(context)) {
    return;
  }
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

  // Find which device is active and set its icon via SetImage.
  // We always use SetImage (never SetState) so that the StreamDeck's
  // internal state auto-cycling on keyUp cannot cause icon flashes.
  // Match against both stored ID and volatile ID to handle fuzzy matching.
  for (size_t i = 0; i < deviceList.size(); ++i) {
    const bool idMatch = (deviceList[i].id == activeDevice);
    const bool volatileMatch
      = !idMatch && (settings.GetVolatileDeviceID(i) == activeDevice);
    if (idMatch || volatileMatch) {
      // Get the user-selected icon for this device
      const auto icon = settings.GetDeviceIcon(deviceList[i].id, i);
      ESDDebug(
        "UpdateState: Device {} (index {}) icon: {} (volatile={})",
        deviceList[i].id,
        i,
        icon,
        volatileMatch);

      // Check custom images first, then built-in icons
      const auto customIt = mCustomImages.find(icon);
      if (customIt != mCustomImages.end()) {
        ESDDebug(
          "Setting custom image: {} (size: {})", icon, customIt->second.size());
        mConnectionManager->SetImage(
          customIt->second, context, kESDSDKTarget_HardwareAndSoftware);
        return;
      }

      const auto builtInIt = mBuiltInIcons.find(icon);
      if (builtInIt != mBuiltInIcons.end()) {
        ESDDebug("Setting built-in icon image: {}", icon);
        mConnectionManager->SetImage(
          builtInIt->second, context, kESDSDKTarget_HardwareAndSoftware);
        return;
      }

      // Fallback: use headphones icon if available
      const auto fallback = mBuiltInIcons.find("headphones");
      if (fallback != mBuiltInIcons.end()) {
        ESDDebug("Falling back to headphones icon for: {}", icon);
        mConnectionManager->SetImage(
          fallback->second, context, kESDSDKTarget_HardwareAndSoftware);
      }
      return;
    }
  }

  // Active device not in list
  ESDDebug("UpdateState: device {} not found in list", activeDevice);
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
  std::scoped_lock lock(mVisibleContextsMutex);
  if (!mButtons.contains(inContext) || !inPayload.contains("settings")) {
    return;
  }
  auto& settings = mButtons[inContext].settings;
  const auto savedDeviceIcons = settings.deviceIcons;
  settings = inPayload.at("settings");
  if (settings.deviceIcons.empty() && !savedDeviceIcons.empty()) {
    settings.deviceIcons = savedDeviceIcons;
  }
  UpdateState(inContext);
  FillButtonDeviceInfo(inContext);
}

void AudioSwitcherStreamDeckPlugin::LoadCustomImages() {
  std::ifstream file(mCustomImagesPath);
  if (!file.is_open()) {
    ESDDebug("No custom images file found at {}", mCustomImagesPath);
    return;
  }
  try {
    json j;
    file >> j;
    if (j.is_object()) {
      for (auto& [name, data] : j.items()) {
        mCustomImages[name] = data.get<std::string>();
      }
    }
    ESDDebug("Loaded {} custom images from disk", mCustomImages.size());
  } catch (const std::exception& e) {
    ESDDebug("Failed to load custom images: {}", e.what());
  }
}

void AudioSwitcherStreamDeckPlugin::LoadBuiltInIcons() {
  const std::vector<std::string> iconNames
    = {"headphones", "speakers", "active", "inactive"};
  const char* base64_chars
    = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  for (const auto& name : iconNames) {
    // Prefer @2x version for high-res StreamDeck display
    std::string filePath = mPluginDir + "\\" + name + "@2x.png";
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
      filePath = mPluginDir + "\\" + name + ".png";
      file.open(filePath, std::ios::binary);
    }
    if (!file.is_open()) {
      ESDDebug("Failed to open built-in icon: {}", filePath);
      continue;
    }

    std::vector<uint8_t> fileData(
      (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

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

    mBuiltInIcons[name] = "data:image/png;base64," + base64Data;
    ESDDebug("Loaded built-in icon: {} ({} bytes)", name, base64Data.size());
  }
}

void AudioSwitcherStreamDeckPlugin::SaveCustomImages() {
  std::ofstream file(mCustomImagesPath);
  if (!file.is_open()) {
    ESDDebug("Failed to open custom images file for writing");
    return;
  }
  json j(mCustomImages);
  file << j.dump();
  ESDDebug("Saved {} custom images to disk", mCustomImages.size());
}
