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
//     },
//     "views": { "circuitShowInactive": false },         (T4.15: views::ViewPrefs)
//     "library": { "folders": ["C:\\Samples"], "userLibrary": "D:\\pqdaw User Library" }   (T5.10, T5.x-library-prefs)
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
#include "pq/views/view_context.h"

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

// T4.15: the "views" section (views::ViewPrefs). Loading is lenient like the audio section (a missing file or section
// gives the defaults, a field of the wrong type keeps its default); saving keeps every other section.
views::ViewPrefs loadViewPrefs(const std::string& path);
Status saveViewPrefs(const std::string& path, const views::ViewPrefs& prefs);

// T5.10: the "library" section: the folders the user added to the browser's Places ({"folders": ["C:\\…", …]}).
// Lenient like the others (a missing file or section: none). Each save keeps the section's other fields.
std::vector<std::string> loadLibraryFolders(const std::string& path);
Status saveLibraryFolders(const std::string& path, const std::vector<std::string>& folders);
// T5.x-library-prefs: the User Library location chosen in Preferences → Library ({"userLibrary": "D:\\…"} in the same
// section); "" = none chosen.
std::string loadUserLibrarySetting(const std::string& path);
Status saveUserLibrarySetting(const std::string& path, const std::string& rootUtf8);

// T5.10: where the browser keeps its index and tags.json: $PQ_LIBRARY_DIR, else <settings dir>/library when
// $PQ_SETTINGS_DIR is set (test runs of the exe never touch the user's %LOCALAPPDATA%), else
// %LOCALAPPDATA%\pqdaw\library. Empty when none can be made.
std::string libraryDataDir();
// The User Library (created when missing): $PQ_USER_LIBRARY, else the location saved in the settings file
// (loadUserLibrarySetting; skipped when it cannot be created), else Documents\pqdaw\User Library (QUESTIONS Q4 / plan
// answer 4). Under $PQ_SETTINGS_DIR (test runs) there is no default: empty unless one of the first two names one.
std::string userLibraryDir();

}  // namespace pq::app
