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

#include "audio_json.h"

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
  const auto state = EPLJSONUtils::GetIntByName(inPayload, "state");
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

  const auto state = EPLJSONUtils::GetIntByName(inPayload, "state");

  // Find the next device in the list
  auto currentDevice
    = GetDefaultAudioDeviceID(settings.direction, settings.role);
  if (currentDevice.empty() && settings.setBothRoles) {
    currentDevice
      = GetDefaultAudioDeviceID(settings.direction, settings.secondaryRole);
  }
  int nextIndex = 0;

  for (size_t i = 0; i < deviceList.size(); ++i) {
    if (deviceList[i].id == currentDevice) {
      nextIndex = (i + 1) % deviceList.size();
      break;
    }
  }

  const auto& nextDevice = deviceList[nextIndex];
  const auto deviceID = settings.GetVolatileDeviceID(nextIndex);

  if (deviceID.empty()) {
    ESDDebug("Doing nothing, no device ID");
    return;
  }

  const auto deviceState = GetAudioDeviceState(deviceID);
  if (deviceState != AudioDeviceState::CONNECTED) {
    mConnectionManager->ShowAlertForContext(inContext);
    return;
  }

  ESDDebug("Setting device to {}", deviceID);
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
      mConnectionManager->SetState(static_cast<int>(i), context);
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
