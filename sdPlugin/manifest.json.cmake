{
  "Actions": [
    {
      "States": [
        {"Image": "headphones"},
        {"Image": "speakers"},
        {"Image": "active"},
        {"Image": "inactive"}
      ],
      "SupportedInMultiActions": true,
      "Icon": "headphones",
      "Name": "Cycle Audio Device",
      "Tooltip": "Cycle between audio devices.",
      "UUID": "com.suop.audioswitcher.cycle"
    }
  ],
  "Author": "suop",
  "CodePathMac": "sdaudioswitch",
  "CodePathWin": "sdaudioswitch.exe",
  "Description": "Cycle between multiple audio devices. Fork of Fred Emmott's Audio Switcher.",
  "Name": "Audio Switcher (suop)",
  "PropertyInspectorPath": "propertyinspector/index.html",
  "Icon": "headphones",
  "Category": "Audio Devices",
  "CategoryIcon": "glyphicons-basic-140-adjust",
  "Version": "${CMAKE_PROJECT_VERSION}",
  "OS": [
    {
      "Platform": "windows",
      "MinimumVersion": "10"
    },
    {
      "Platform": "mac",
      "MinimumVersion": "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    }
  ],
  "SDKVersion": 2,
  "Software": {
    "MinimumVersion": "4.1"
  }
}
