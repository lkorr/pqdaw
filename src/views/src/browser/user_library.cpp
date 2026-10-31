// T5.11: the User Library's files in the views (pq/views/browser/library.h "T5.11").

#include <algorithm>
#include <vector>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "pq/document/devices.h"
#include "pq/document/library_files.h"
#include "pq/library/index.h"
#include "pq/views/browser/library.h"

namespace pq::views {

namespace fs = std::filesystem;

namespace {

// Whether two files hold the same bytes (sizes first, then 1 MiB chunks).
bool sameFileBytes(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  if (!fs::exists(a, ec) || !fs::exists(b, ec)) return false;
  const auto sa = fs::file_size(a, ec);
  if (ec) return false;
  const auto sb = fs::file_size(b, ec);
  if (ec || sa != sb) return false;
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  if (!fa || !fb) return false;
  std::vector<char> ba(1u << 20), bb(1u << 20);
  for (;;) {
    fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
    fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
    const std::streamsize na = fa.gcount(), nb = fb.gcount();
    if (na != nb || std::memcmp(ba.data(), bb.data(), static_cast<size_t>(na)) != 0) return false;
    if (na == 0 || !fa || !fb) return na == nb && fa.eof() == fb.eof();
  }
}

fs::path pathOf(std::string_view utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8Of(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

std::string join(std::string_view dir, std::string_view name) {
  std::string s(dir);
  if (!s.empty() && s.back() != '\\' && s.back() != '/') s += '\\';
  s += name;
  return s;
}

// A file-name stem Windows accepts (reserved characters → '_', no trailing dots or spaces, not empty).
std::string safeStem(std::string_view name) {
  std::string s;
  for (const char c : name) s += (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
                                  c == '?' || c == '*' || (static_cast<unsigned char>(c) < 32))
                                     ? '_'
                                     : c;
  while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
  // L20: capped (a path segment past ~255 characters cannot be created) and never a reserved device name (CON, NUL,
  // COM1 …: ordinary files on this Windows 11 build, failures on Windows 10).
  if (s.size() > 120) {
    size_t n = 120;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;  // not inside a UTF-8 sequence
    s.resize(n);
  }
  std::string upper;
  for (const char c : s.substr(0, s.find('.'))) upper += static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
  static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
                                    "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  for (const char* r : kReserved)
    if (upper == r) {
      s.insert(0, "_");
      break;
    }
  return s.empty() ? std::string("Untitled") : s;
}

int64_t writeTime(std::string_view path) {
  std::error_code ec;
  const auto t = fs::last_write_time(pathOf(path), ec);
  return ec ? -1 : static_cast<int64_t>(t.time_since_epoch().count());
}

Error noLibrary() { return Error{ErrorCode::NotFound, "no User Library folder", {}}; }

// The device's "patch" value in a preset's first bytes ("" when not found).
std::string sniffPatch(std::string_view head) {
  const size_t k = head.find("\"patch\"");
  if (k == std::string_view::npos) return {};
  size_t q = head.find(':', k + 7);
  if (q == std::string_view::npos) return {};
  q = head.find('"', q + 1);
  if (q == std::string_view::npos) return {};
  const size_t e = head.find('"', q + 1);
  if (e == std::string_view::npos) return {};
  return std::string(head.substr(q + 1, e - q - 1));
}

}  // namespace

const std::string& Library::presetName(doc::DeviceId d) const {
  static const std::string kNone;
  const auto it = presetNames_.find(d.value);
  return it == presetNames_.end() ? kNone : it->second;
}

void Library::setPresetName(doc::DeviceId d, std::string name) {
  if (name.empty()) presetNames_.erase(d.value);
  else presetNames_[d.value] = std::move(name);
  ctx_.libraryChanged();
}

std::string Library::userPath(std::string_view sub) const {
  if (userLibrary_.empty()) return {};
  std::string s = join(userLibrary_, sub);
  std::replace(s.begin(), s.end(), '/', '\\');
  return s;
}

std::string Library::uniqueFile(std::string_view dir, std::string_view stem, std::string_view ext) {
  const std::string base = safeStem(stem);
  std::error_code ec;
  for (int n = 1;; ++n) {
    const std::string name = n == 1 ? base + std::string(ext) : base + " " + std::to_string(n) + std::string(ext);
    const std::string p = join(dir, name);
    if (!fs::exists(pathOf(p), ec)) return p;
  }
}

Result<std::string> Library::savePreset(const doc::Device& d, std::string_view deviceName, std::string_view name) {
  if (userLibrary_.empty()) return noLibrary();
  const std::string dir = d.isRack() ? userPath("Racks") : userPath("Presets\\" + safeStem(deviceName));
  const std::string stem(name.empty() ? deviceName : name);
  const std::string path = uniqueFile(dir, stem, doc::kPresetExtension);
  const std::string shown = utf8Of(pathOf(path).stem());
  auto text = doc::serializePreset(d, shown);
  if (!text) return std::move(text).error();
  if (Status s = doc::writeTextFileAtomic(path, *text); !s) return std::move(s).error();
  return path;
}

Result<std::string> Library::saveDefaultPreset(const doc::Device& d, std::string_view deviceName) {
  if (userLibrary_.empty()) return noLibrary();
  const std::string path = userPath("Defaults\\" + safeStem(deviceName) + std::string(doc::kPresetExtension));
  auto text = doc::serializePreset(d, deviceName);
  if (!text) return std::move(text).error();
  if (Status s = doc::writeTextFileAtomic(path, *text); !s) return std::move(s).error();
  presetCache_.erase(library::foldedPathKey(path));
  return path;
}

doc::DevicePtr Library::defaultPreset(std::string_view deviceName, std::string_view patch) {
  if (userLibrary_.empty()) return nullptr;
  const std::string path = userPath("Defaults\\" + safeStem(deviceName) + std::string(doc::kPresetExtension));
  const int64_t t = writeTime(path);
  const std::string key = library::foldedPathKey(path);
  if (t < 0) {
    presetCache_.erase(key);
    return nullptr;
  }
  CachedPreset& c = presetCache_[key];
  if (c.mtime != t || (!c.device && c.patch.empty())) {
    c = CachedPreset{t, nullptr, {}};
    if (auto text = doc::readTextFile(path))
      if (auto f = doc::parsePreset(*text, path)) {
        c.device = std::move(f->device);
        c.patch = c.device->patch;
      }
    if (!c.device) c.patch = "?";  // unreadable: not retried until the file changes
  }
  return c.device && c.device->patch == patch ? c.device : nullptr;
}

Result<std::string> Library::saveUserDevice(const doc::UserDeviceFile& f) {
  if (userLibrary_.empty()) return noLibrary();
  const std::string path = uniqueFile(userPath("Devices"), f.name, doc::kUserDeviceExtension);
  doc::UserDeviceFile g = f;
  g.name = utf8Of(pathOf(path).stem());
  auto text = doc::serializeUserDevice(g);
  if (!text) return std::move(text).error();
  if (Status s = doc::writeTextFileAtomic(path, *text); !s) return std::move(s).error();
  userDevicesFor_.reset();
  return path;
}

Result<std::string> Library::saveTuning(const doc::TuningFile& f) {
  if (userLibrary_.empty()) return noLibrary();
  const std::string path = uniqueFile(userPath("Tunings"), f.name, doc::kTuningExtension);
  auto text = doc::serializeTuning(f);
  if (!text) return std::move(text).error();
  if (Status s = doc::writeTextFileAtomic(path, *text); !s) return std::move(s).error();
  return path;
}

Result<std::string> Library::saveClip(const doc::Project& p, const doc::ClipSlot& c, std::string_view sampleFile,
                                     std::string_view dirIn) {
  std::string dir(dirIn);
  if (dir.empty()) {
    if (userLibrary_.empty()) return noLibrary();
    dir = userPath("Clips");
  }
  std::string name = c.name();
  if (name.empty()) name = c.audio ? "Audio Clip" : "MIDI Clip";
  doc::ClipFile f{name, c, std::nullopt};
  std::error_code ec;
  if (c.audio) {
    const doc::SampleRef* s = doc::findSample(p, c.audio->sample);
    if (!s) return Error{ErrorCode::NotFound, "the clip's sample is not in the project", {}};
    doc::SampleRef copy = *s;
    const fs::path src = pathOf(sampleFile.empty() ? std::string_view(s->path) : sampleFile);
    if (!fs::exists(src, ec)) return Error{ErrorCode::NotFound, "the clip's sample is missing: " + s->path, {}};
    const std::string srcUtf8 = utf8Of(src);
    std::string target = srcUtf8;
    const std::string rel = doc::relativeLibraryPath(dir, srcUtf8);
    const bool below = rel.find(':') == std::string::npos && rel.rfind("//", 0) != 0;
    if (!below) {
      const std::string samples = join(dir, "Samples");
      fs::create_directories(pathOf(samples), ec);
      // Reuse a copy with the same name and the same bytes (M5 review M18: name and size alone let two one-shots of
      // one name and length from different packs share a file); else a free name.
      const std::string same = join(samples, utf8Of(src.filename()));
      if (sameFileBytes(pathOf(same), src)) {
        target = same;
      } else {
        target = uniqueFile(samples, utf8Of(src.stem()), utf8Of(src.extension()));
        fs::copy_file(src, pathOf(target), fs::copy_options::none, ec);
        if (ec) return Error{ErrorCode::Io, "cannot copy " + srcUtf8 + ": " + ec.message(), {}};
      }
    }
    copy.path = doc::relativeLibraryPath(dir, target);
    f.sample = copy;
  }
  const std::string path = uniqueFile(dir, name, doc::kClipExtension);
  f.name = utf8Of(pathOf(path).stem());
  auto text = doc::serializeClip(f, &p);
  if (!text) return std::move(text).error();
  if (Status s = doc::writeTextFileAtomic(path, *text); !s) return std::move(s).error();
  return path;
}

Result<doc::ClipFile> Library::readClip(std::string_view path) const {
  auto text = doc::readTextFile(path);
  if (!text) return std::move(text).error();
  auto f = doc::parseClip(*text, path);
  if (!f) return std::move(f).error();
  if (f->sample) f->sample->path = doc::resolveLibraryPath(utf8Of(pathOf(path).parent_path()), f->sample->path);
  return f;
}

const std::vector<Library::UserDeviceEntry>& Library::userDevices() {
  const auto idx = index();
  if (idx == userDevicesFor_ && idx) return userDevices_;
  userDevicesFor_ = idx;
  userDevices_.clear();
  if (!idx) return userDevices_;
  for (uint32_t e = 0; e < idx->entryCount(); ++e) {
    if (idx->kind(e) != library::ItemKind::Device) continue;
    const std::string path = idx->path(e);
    auto text = doc::readTextFile(path);
    if (!text) continue;
    auto f = doc::parseUserDevice(*text, path);
    if (!f) continue;
    userDevices_.push_back({path, std::move(*f)});
  }
  std::sort(userDevices_.begin(), userDevices_.end(), [](const UserDeviceEntry& a, const UserDeviceEntry& b) {
    return library::compareFolded(a.file.name, b.file.name) < 0;
  });
  return userDevices_;
}

std::string Library::presetPatch(std::string_view path) {
  // The User Library's layout first (no I/O): ...\Presets\<Device>\x.pqpreset, ...\Defaults\<Device>.pqpreset.
  std::string folded;
  for (const char c : path) folded += c == '/' ? '\\' : library::foldChar(c);
  auto stock = [](std::string_view name) -> std::string {
    for (const doc::StockDevice& d : doc::stockDevices())
      if (library::compareFolded(d.name, name) == 0 || library::compareFolded(d.patch, name) == 0) return std::string(d.patch);
    return {};
  };
  constexpr std::string_view kPresets = "\\presets\\", kDefaults = "\\defaults\\";
  if (const size_t k = folded.rfind(kPresets); k != std::string::npos) {
    const size_t a = k + kPresets.size(), b = folded.find('\\', a);
    if (b != std::string::npos && folded.find('\\', b + 1) == std::string::npos)
      if (std::string p = stock(path.substr(a, b - a)); !p.empty()) return p;
  }
  if (const size_t k = folded.rfind(kDefaults); k != std::string::npos && folded.find('\\', k + kDefaults.size()) == std::string::npos)
    if (std::string p = stock(library::stemOf(path.substr(k + kDefaults.size()))); !p.empty()) return p;
  const int64_t t = writeTime(path);
  if (t < 0) return {};
  CachedPreset& c = presetCache_[library::foldedPathKey(path)];
  if (c.mtime == t && !c.patch.empty()) return c.patch == "?" ? std::string() : c.patch;
  c = CachedPreset{t, nullptr, {}};
  std::ifstream in(pathOf(path), std::ios::binary);
  char head[4096];
  in.read(head, sizeof head);
  c.patch = sniffPatch(std::string_view(head, static_cast<size_t>(in.gcount())));
  if (c.patch.empty()) c.patch = "?";
  return c.patch == "?" ? std::string() : c.patch;
}

}  // namespace pq::views
