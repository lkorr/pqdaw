// User settings (T3.18): pq/app/user_settings.h describes the file.
#include "pq/app/user_settings.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <nlohmann/json.hpp>

#include "pq/meta/registry.h"
#include "pq/meta/user_defaults.h"

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
  a["outputChannels"] = s.outputChannels;
  a["cueOutput"] = s.cueOutput;  // T5.10
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
  num("outputChannels", s.outputChannels, 1, 64);
  num("cueOutput", s.cueOutput, 0, 31);  // T5.10
  return out;
}

namespace {

// Replaces one top-level section of the settings file (the others are kept), atomically. With `field`, replaces only
// that field of the section (its other fields are kept).
Status saveSection(const std::string& path, const char* key, json section, const char* field = nullptr) {
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
  if (field) {
    if (!root[key].is_object()) root[key] = json::object();
    root[key][field] = std::move(section);
  } else {
    root[key] = std::move(section);
  }
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

}  // namespace

Status saveAudioSettings(const std::string& path, const views::AudioSettings& s) {
  return saveSection(path, "audio", json::parse(audioSettingsJson(s)));
}

namespace {
// T6.8: the default warp mode in the settings file (the project file's names, serialize.cpp).
constexpr const char* kWarpNames[] = {"off", "rePitch", "beats", "tones", "texture", "complex", "complexPro"};
static_assert(std::size(kWarpNames) == static_cast<size_t>(doc::kLastAudioWarp) + 1);
}  // namespace

namespace {
// EX.2: the Export Audio window's choices (views "export" object).
constexpr const char* kExportTrackNames[] = {"master", "allTracks", "selected"};
constexpr const char* kExportRangeNames[] = {"song", "loop", "timeSelection"};
constexpr const char* kExportBitNames[] = {"pcm16", "pcm24", "float32"};
constexpr const char* kExportDitherNames[] = {"none", "tpdf", "shaped"};
constexpr const char* kExportFormatNames[] = {"wav", "flac"};

template <class E, size_t N>
void readEnum(const json& o, const char* key, const char* const (&names)[N], E& out) {
  const auto f = o.find(key);
  if (f == o.end() || !f->is_string()) return;
  const std::string v = f->get<std::string>();
  for (size_t i = 0; i < N; ++i)
    if (v == names[i]) out = static_cast<E>(i);
}

views::ExportPrefs exportPrefsOf(const json& o) {
  views::ExportPrefs p;
  readEnum(o, "tracks", kExportTrackNames, p.tracks);
  readEnum(o, "range", kExportRangeNames, p.range);
  readEnum(o, "bits", kExportBitNames, p.bits);
  readEnum(o, "dither", kExportDitherNames, p.dither);
  readEnum(o, "format", kExportFormatNames, p.format);
  if (p.format == views::ExportFileFormat::Flac && p.bits == views::ExportBitDepth::Float32)
    p.bits = views::ExportBitDepth::Pcm24;  // EX.3: FLAC is 16 or 24-bit
  if (const auto f = o.find("includeReturnsMaster"); f != o.end() && f->is_boolean()) p.includeReturnsMaster = f->get<bool>();
  if (const auto f = o.find("renderAsLoop"); f != o.end() && f->is_boolean()) p.renderAsLoop = f->get<bool>();
  if (const auto f = o.find("normalize"); f != o.end() && f->is_boolean()) p.normalize = f->get<bool>();
  if (const auto f = o.find("sampleRate"); f != o.end() && f->is_number_unsigned()) {
    const auto r = f->get<uint64_t>();
    if (r == 0 || (r >= 8000 && r <= 768000)) p.sampleRate = static_cast<uint32_t>(r);
  }
  if (const auto f = o.find("folder"); f != o.end() && f->is_string()) p.folder = f->get<std::string>();
  return p;
}

json exportPrefsJson(const views::ExportPrefs& p) {
  json o = json::object();
  o["tracks"] = kExportTrackNames[static_cast<size_t>(p.tracks)];
  o["range"] = kExportRangeNames[static_cast<size_t>(p.range)];
  o["includeReturnsMaster"] = p.includeReturnsMaster;
  o["renderAsLoop"] = p.renderAsLoop;
  o["format"] = kExportFormatNames[static_cast<size_t>(p.format)];
  o["sampleRate"] = p.sampleRate;
  o["bits"] = kExportBitNames[static_cast<size_t>(p.bits)];
  o["dither"] = kExportDitherNames[static_cast<size_t>(p.dither)];
  o["normalize"] = p.normalize;
  o["folder"] = p.folder;
  return o;
}
}  // namespace

views::ViewPrefs loadViewPrefs(const std::string& path) {
  views::ViewPrefs prefs;
  std::ifstream in(utf8Path(path), std::ios::binary);
  if (!in) return prefs;
  std::ostringstream ss;
  ss << in.rdbuf();
  const json root = json::parse(ss.str(), nullptr, false);
  if (root.is_discarded() || !root.is_object()) return prefs;
  const auto it = root.find("views");
  if (it == root.end() || !it->is_object()) return prefs;
  if (const auto f = it->find("circuitShowInactive"); f != it->end() && f->is_boolean()) prefs.circuitShowInactive = f->get<bool>();
  if (const auto f = it->find("browserWidth"); f != it->end() && f->is_number()) prefs.browserWidth = f->get<float>();
  if (const auto f = it->find("dragOut24Bit"); f != it->end() && f->is_boolean()) prefs.dragOut24Bit = f->get<bool>();
  // TX.free-rack: the Device view's free-device rack.
  if (const auto f = it->find("freeRackOpen"); f != it->end() && f->is_boolean()) prefs.freeRackOpen = f->get<bool>();
  if (const auto f = it->find("freeRackHeight"); f != it->end() && f->is_number()) prefs.freeRackHeight = f->get<float>();
  // T6.8: Preferences → Warp.
  if (const auto f = it->find("autoWarpLongSamples"); f != it->end() && f->is_boolean()) prefs.autoWarpLongSamples = f->get<bool>();
  if (const auto f = it->find("defaultWarpMode"); f != it->end() && f->is_string()) {
    const std::string m = f->get<std::string>();
    for (uint8_t w = 1; w <= static_cast<uint8_t>(doc::kLastAudioWarp); ++w)
      if (m == kWarpNames[w]) prefs.defaultWarpMode = static_cast<doc::AudioWarp>(w);
  }
  // T6.7b: Preferences → Record.
  if (const auto f = it->find("recordOffsetMs"); f != it->end() && f->is_number())
    prefs.recordOffsetMs = std::clamp(f->get<float>(), -views::ViewPrefs::kMaxRecordOffsetMs, views::ViewPrefs::kMaxRecordOffsetMs);
  if (const auto f = it->find("autosave"); f != it->end() && f->is_boolean()) prefs.autosave = f->get<bool>();  // AS.1
  if (const auto f = it->find("export"); f != it->end() && f->is_object()) prefs.exportAudio = exportPrefsOf(*f);  // EX.2
  if (const auto f = it->find("midiExportMode"); f != it->end() && f->is_string()) {  // MF.3
    const std::string m = f->get<std::string>();
    for (size_t i = 0; i < doc::kMidiExportModeNames.size(); ++i)
      if (m == doc::kMidiExportModeNames[i]) prefs.midiExportMode = static_cast<doc::MidiExportMode>(i);
  }
  return prefs;
}

Status saveViewPrefs(const std::string& path, const views::ViewPrefs& prefs) {
  json v = json::object();
  v["circuitShowInactive"] = prefs.circuitShowInactive;
  v["browserWidth"] = prefs.browserWidth;
  v["dragOut24Bit"] = prefs.dragOut24Bit;
  v["freeRackOpen"] = prefs.freeRackOpen;  // TX.free-rack
  v["freeRackHeight"] = prefs.freeRackHeight;
  v["autoWarpLongSamples"] = prefs.autoWarpLongSamples;  // T6.8
  v["defaultWarpMode"] = kWarpNames[static_cast<size_t>(prefs.defaultWarpMode)];
  v["recordOffsetMs"] = prefs.recordOffsetMs;  // T6.7b
  v["autosave"] = prefs.autosave;              // AS.1
  v["export"] = exportPrefsJson(prefs.exportAudio);  // EX.2
  v["midiExportMode"] = std::string(doc::kMidiExportModeNames[static_cast<size_t>(prefs.midiExportMode)]);  // MF.3
  return saveSection(path, "views", std::move(v));
}

namespace {
// The "library" section's `field` (null when the file, the section or the field is missing).
json libraryField(const std::string& path, const char* field) {
  std::ifstream in(utf8Path(path), std::ios::binary);
  if (!in) return nullptr;
  std::ostringstream ss;
  ss << in.rdbuf();
  const json root = json::parse(ss.str(), nullptr, false);
  if (root.is_discarded() || !root.is_object()) return nullptr;
  const auto it = root.find("library");
  if (it == root.end() || !it->is_object()) return nullptr;
  const auto f = it->find(field);
  return f == it->end() ? json(nullptr) : *f;
}
}  // namespace

std::vector<std::string> loadLibraryFolders(const std::string& path) {
  std::vector<std::string> out;
  const json f = libraryField(path, "folders");
  if (!f.is_array()) return out;
  for (const json& v : f)
    if (v.is_string()) out.push_back(v.get<std::string>());
  return out;
}

Status saveLibraryFolders(const std::string& path, const std::vector<std::string>& folders) {
  return saveSection(path, "library", json(folders), "folders");
}

std::string loadUserLibrarySetting(const std::string& path) {
  const json f = libraryField(path, "userLibrary");
  return f.is_string() ? f.get<std::string>() : std::string();
}

Status saveUserLibrarySetting(const std::string& path, const std::string& rootUtf8) {
  return saveSection(path, "library", json(rootUtf8), "userLibrary");
}

std::vector<MidiPortSetting> loadMidiSettings(const std::string& path) {  // T6.4
  std::vector<MidiPortSetting> out;
  std::ifstream in(utf8Path(path), std::ios::binary);
  if (!in) return out;
  std::ostringstream ss;
  ss << in.rdbuf();
  const json root = json::parse(ss.str(), nullptr, false);
  if (root.is_discarded() || !root.is_object()) return out;
  const auto midi = root.find("midi");
  if (midi == root.end() || !midi->is_object()) return out;
  const auto inputs = midi->find("inputs");
  if (inputs == midi->end() || !inputs->is_array()) return out;
  for (const json& v : *inputs) {
    if (!v.is_object()) continue;
    const auto name = v.find("name");
    if (name == v.end() || !name->is_string() || name->get<std::string>().empty()) continue;
    MidiPortSetting s;
    s.name = name->get<std::string>();
    if (const auto f = v.find("enabled"); f != v.end() && f->is_boolean()) s.enabled = f->get<bool>();
    if (const auto f = v.find("bend"); f != v.end() && f->is_number() && f->get<double>() > 0.0 && f->get<double>() <= 96.0)
      s.bendSemitones = f->get<float>();
    out.push_back(std::move(s));
  }
  return out;
}

Status saveMidiSettings(const std::string& path, const std::vector<MidiPortSetting>& ports) {
  json a = json::array();
  for (const MidiPortSetting& s : ports) a.push_back({{"name", s.name}, {"enabled", s.enabled}, {"bend", s.bendSemitones}});
  return saveSection(path, "midi", std::move(a), "inputs");
}

std::string loadSamplesFolderSetting(const std::string& path) {
  const json f = libraryField(path, "samplesFolder");
  return f.is_string() ? f.get<std::string>() : std::string();
}

Status saveSamplesFolderSetting(const std::string& path, const std::string& rootUtf8) {
  return saveSection(path, "library", json(rootUtf8), "samplesFolder");
}

namespace {
std::string envUtf8(const wchar_t* name) {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return pathUtf8(fs::path(buf));
}
std::string ensureDir(const fs::path& p) {
  std::error_code ec;
  fs::create_directories(p, ec);
  return fs::is_directory(p, ec) ? pathUtf8(p) : std::string();
}
}  // namespace

std::string libraryDataDir() {
  if (std::string d = envUtf8(L"PQ_LIBRARY_DIR"); !d.empty()) return ensureDir(utf8Path(d));
  if (std::string s = envUtf8(L"PQ_SETTINGS_DIR"); !s.empty()) return ensureDir(utf8Path(s) / "library");
  const std::string local = envUtf8(L"LOCALAPPDATA");
  if (local.empty()) return {};
  return ensureDir(utf8Path(local) / "pqdaw" / "library");
}

std::string userLibraryDir() {
  if (std::string d = envUtf8(L"PQ_USER_LIBRARY"); !d.empty()) return ensureDir(utf8Path(d));
  if (const std::string set = loadUserLibrarySetting(userSettingsPath()); !set.empty())
    if (std::string d = ensureDir(utf8Path(set)); !d.empty()) return d;  // unreachable (a drive gone): the default
  if (!envUtf8(L"PQ_SETTINGS_DIR").empty()) return {};
  PWSTR docs = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) || !docs) return {};
  const fs::path p = fs::path(docs) / L"pqdaw" / L"User Library";
  CoTaskMemFree(docs);
  return ensureDir(p);
}

std::string samplesFolderDir() {
  if (std::string d = envUtf8(L"PQ_SAMPLES_DIR"); !d.empty()) return ensureDir(utf8Path(d));
  if (const std::string set = loadSamplesFolderSetting(userSettingsPath()); !set.empty()) return ensureDir(utf8Path(set));
  return {};
}

void loadMetaDefaults(const std::string& path) {
  std::ifstream in(utf8Path(path), std::ios::binary);
  if (!in) return;
  std::ostringstream ss;
  ss << in.rdbuf();
  const json root = json::parse(ss.str(), nullptr, false);
  if (root.is_discarded() || !root.is_object()) return;
  const auto it = root.find("metaDefaults");
  if (it == root.end() || !it->is_object()) return;
  for (const auto& [type, rec] : it->items()) {
    const meta::Type* t = meta::findType(type);
    if (t == nullptr || !rec.is_object()) continue;
    std::vector<std::pair<std::string, meta::DataValue>> f;
    for (const auto& [k, v] : rec.items())
      if (v.is_number()) f.emplace_back(k, meta::DataValue::num(v.get<double>()));
    auto d = std::make_shared<const meta::DataValue>(meta::DataValue::record(std::move(f)));
    if (t->dataProblem == nullptr || !t->dataProblem(*d).empty()) continue;
    meta::setUserDefaultData(t->id, std::move(d));
  }
}

Status saveMetaDefault(const std::string& path, std::string_view type, const meta::DataValue& data) {
  json rec = json::object();
  if (data.kind == meta::DataValue::Kind::Record)
    for (const auto& [k, v] : data.fields)
      if (v.kind == meta::DataValue::Kind::Number) rec[k] = v.number;
  const std::string key(type);
  return saveSection(path, "metaDefaults", std::move(rec), key.c_str());
}

}  // namespace pq::app
