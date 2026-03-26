//==============================================================================
/**
@file       AudioSwitcherStreamDeckPlugin.h

@copyright  (c) 2018, Corsair Memory, Inc.
@copyright  (c) 2018-present, Fred Emmott.
      This source code is licensed under the MIT-style license found in the
LICENSE file.

**/
//==============================================================================

#include <AudioDevices/AudioDevices.h>
#include <StreamDeckSDK/ESDBasePlugin.h>

#include <mutex>

#include "ButtonSettings.h"

using json = nlohmann::json;
using namespace FredEmmott::Audio;

class AudioSwitcherStreamDeckPlugin : public ESDBasePlugin {
 public:
  AudioSwitcherStreamDeckPlugin();
  virtual ~AudioSwitcherStreamDeckPlugin();

  void KeyDownForAction(
    const std::string& inAction,
    const std::string& inContext,
    const json& inPayload,
    const std::string& inDeviceID) override;
  void KeyUpForAction(
    const std::string& inAction,
    const std::string& inContext,
    const json& inPayload,
    const std::string& inDeviceID) override;

  void WillAppearForAction(
    const std::string& inAction,
    const std::string& inContext,
    const json& inPayload,
    const std::string& inDeviceID) override;
  void WillDisappearForAction(
    const std::string& inAction,
    const std::string& inContext,
    const json& inPayload,
    const std::string& inDeviceID) override;

  void SendToPlugin(
    const std::string& inAction,
    const std::string& inContext,
    const json& inPayload,
    const std::string& inDeviceID) override;

  void DeviceDidConnect(const std::string& inDeviceID, const json& inDeviceInfo)
    override;
  void DeviceDidDisconnect(const std::string& inDeviceID) override;

  void DidReceiveGlobalSettings(const json& inPayload) override;
  void DidReceiveSettings(
    const std::string& inAction,
    const std::string& inContext,
    const json& inPayload,
    const std::string& inDeviceID) override;

 private:
  struct Button {
    ButtonSettings settings;
  };

  std::recursive_mutex mVisibleContextsMutex;
  std::map<std::string, Button> mButtons;
  DefaultChangeCallbackHandle mCallbackHandle;

  // Global custom image store (shared across all buttons).
  // Keyed by short name (e.g. "img1") → base64 data URL.
  std::map<std::string, std::string> mCustomImages;
  std::string mCustomImagesPath;// path to the JSON file on disk

  void OnDefaultDeviceChanged(
    AudioDeviceDirection direction,
    AudioDeviceRole role,
    const std::string& activeAudioDeviceID);
  void UpdateState(const std::string& context, const std::string& device = "");
  void FillButtonDeviceInfo(const std::string& context);

  void LoadCustomImages();
  void SaveCustomImages();
};
