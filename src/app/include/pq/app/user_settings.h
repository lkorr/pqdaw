#pragma once

// User settings (T3.18): what pqdaw remembers between runs that is not part of a project. One JSON file,
// %APPDATA%\pqdaw\settings.json ($PQ_SETTINGS_DIR replaces the directory: tests, portable setups):
//
//   {
//     "schemaVersion": 1,
//     "audio": {
//       "driver": "asio" | "wasapi-shared" | "wasapi-exclusive",
//       "outputDevice": "Focusrite USB ASIO",  "outputName": "Focusrite USB ASIO",
//       "inputDevice": "",                      "inputName": "",
//       "sampleRate": 48000, "bufferFrames": 64, "inputChannels": 2, "outputChannels": 2
//     }
//   }
//
// Saving keeps every other top-level key (later settings sections), writes a temporary file, flushes it and
// replaces the old one atomically (MoveFileExW), so a crash never leaves half a file. Reading is lenient per field:
// a missing or invalid field keeps its default (a settings file must never stop the DAW from starting), and the
// warnings say which fields were ignored.
//
// Not RT-safe (file I/O). Any thread, but the app calls it on the UI thread only.

#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/views/prefs/audio_prefs.h"

namespace pq::app {

// The settings directory ($PQ_SETTINGS_DIR, else %APPDATA%\pqdaw) and file (UTF-8 paths).
std::string userSettingsDir();
std::string userSettingsPath();

struct LoadedAudioSettings {
  views::AudioSettings settings;
  std::vector<std::string> warnings;  // fields that were present but invalid (their defaults were used)
};
// Errors: ErrorCode::NotFound when there is no file (or no "audio" section), ErrorCode::Parse when it is not JSON.
Result<LoadedAudioSettings> loadAudioSettings(const std::string& path);
Status saveAudioSettings(const std::string& path, const views::AudioSettings& s);

// The JSON text of one settings object (tests).
std::string audioSettingsJson(const views::AudioSettings& s);

}  // namespace pq::app
