// User settings (T3.18): pq/app/user_settings.h describes the file.
#include "pq/app/user_settings.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace pq::app {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

fs::path utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string pathUtf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

Error settingsError(ErrorCode c, const std::string& m) { return Error{c, "settings: " + m, {}}; }

}  // namespace

std::string userSettingsDir() {
  if (const char* env = std::getenv("PQ_SETTINGS_DIR"); env && *env) return env;
  wchar_t buf[32768];
  const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, static_cast<DWORD>(std::size(buf)));
  if (n > 0 && n < std::size(buf)) return pathUtf8(fs::path(buf) / "pqdaw");
  return "pqdaw-settings";
}

std::string userSettingsPath() { return pathUtf8(utf8Path(userSettingsDir()) / "settings.json"); }

std::string audioSettingsJson(const views::AudioSettings& s) {
  json a;
  a["driver"] = views::audioDriverTypeKey(s.driver);
  a["outputDevice"] = s.outputDevice;
  a["outputName"] = s.outputName;
  a["inputDevice"] = s.inputDevice;
  a["inputName"] = s.inputName;
  a["sampleRate"] = s.sampleRate;
  a["bufferFrames"] = s.bufferFrames;
  a["inputChannels"] = s.inputChannels;
  a["outputChannels"] = s.outputChannels;
  return a.dump(2, ' ', false, json::error_handler_t::replace);  // never throws on a bad driver-name byte
}

Result<LoadedAudioSettings> loadAudioSettings(const std::string& path) {
  std::ifstream in(utf8Path(path), std::ios::binary);
  if (!in) return settingsError(ErrorCode::NotFound, "no settings file at " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  json root = json::parse(ss.str(), nullptr, false);
  if (root.is_discarded() || !root.is_object()) return settingsError(ErrorCode::Parse, path + " is not a JSON object");
  auto it = root.find("audio");
  if (it == root.end() || !it->is_object()) return settingsError(ErrorCode::NotFound, path + " has no \"audio\" section");
  const json& a = *it;
  LoadedAudioSettings out;
  views::AudioSettings& s = out.settings;
  auto str = [&](const char* key, std::string& dst) {
    auto f = a.find(key);
    if (f == a.end()) return;
    if (f->is_string()) dst = f->get<std::string>();
    else out.warnings.push_back(std::string("audio.") + key + " is not a string");
  };
  auto num = [&](const char* key, uint32_t& dst, uint32_t lo, uint32_t hi) {
    auto f = a.find(key);
    if (f == a.end()) return;
    if (f->is_number_unsigned() && f->get<uint64_t>() >= lo && f->get<uint64_t>() <= hi) dst = f->get<uint32_t>();
    else out.warnings.push_back(std::string("audio.") + key + " is out of range");
  };
  std::string driver;
  str("driver", driver);
  if (!driver.empty() && !views::parseAudioDriverType(driver, s.driver)) out.warnings.push_back("audio.driver '" + driver + "' is unknown");
  str("outputDevice", s.outputDevice);
  str("outputName", s.outputName);
  str("inputDevice", s.inputDevice);
  str("inputName", s.inputName);
  num("sampleRate", s.sampleRate, 8000, 768000);
  num("bufferFrames", s.bufferFrames, 16, 8192);
  num("inputChannels", s.inputChannels, 0, 64);
  num("outputChannels", s.outputChannels, 1, 64);
  return out;
}

Status saveAudioSettings(const std::string& path, const views::AudioSettings& s) {
  const fs::path p = utf8Path(path);
  json root = json::object();
  {
    std::ifstream in(p, std::ios::binary);
    if (in) {
      std::ostringstream ss;
      ss << in.rdbuf();
      json old = json::parse(ss.str(), nullptr, false);
      if (!old.is_discarded() && old.is_object()) root = std::move(old);  // keep the other sections
    }
  }
  root["schemaVersion"] = 1;
  root["audio"] = json::parse(audioSettingsJson(s));
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const fs::path tmp = fs::path(p).concat(L".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return settingsError(ErrorCode::Io, "cannot write " + pathUtf8(tmp));
    out << root.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    out.flush();
    if (!out) return settingsError(ErrorCode::Io, "cannot write " + pathUtf8(tmp));
  }
  {  // flush to disk before the rename, so the replacement is never an empty file after a power loss
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      FlushFileBuffers(h);
      CloseHandle(h);
    }
  }
  if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return settingsError(ErrorCode::Io, "cannot replace " + path + " (error " + std::to_string(GetLastError()) + ")");
  return Ok{};
}

}  // namespace pq::app
