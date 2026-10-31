#pragma once

// User-library files (T5.11; SPEC §9.1, §12; docs/plans/M5.md T5.11): one JSON format per kind, each written by the
// project serializer's own writers (serialize.h: the same device, clip, sample and tuning objects, the same strict
// reader, the same schema version), so a preset and a project never disagree about what a device is.
//
//   .pqpreset   {"format": "pqpreset", "schemaVersion", "name", "devices": [one device]}: a device preset (its patch,
//               kind, params and settings), a rack preset (the rack with its nested chains, mappings, macro names,
//               variations and the texts of the user devices inside it) or a user device's state (its "source").
//               The device object comes right after the name, so its "kind" (and a rack's "rack" key) sits in the
//               first bytes of the file, where the browser index sniffs the category (QUESTIONS Q153).
//   .pqdevice   {"format": "pqdevice", "schemaVersion", "name", "kind", "patch", "origin", "crlf"?, "source": [lines],
//               "panel"?: [lines]}: a user device ("Save as New Device"): its patch text (the device patch is the patch
//               of the text named `patch`, the stock device it was forked from), the stdlib file it was forked from
//               (`origin`, relative to stdlib/, so its imports resolve the same way: DeviceLibrary::resolve) and an
//               optional panel description (T4.6 panel JSON).
//   .pqtuning   {"format": "pqtuning", "schemaVersion", "name", "colour", "def": {tuning definition}}: EDO, MOS and
//               Scala tunings exactly as a project embeds a main tuning.
//   .pqclip     {"format": "pqclip", "schemaVersion", "name", "clip": {a note or audio clip}, "samples"?: [one]}: a
//               clip; an audio clip carries its sample's table entry (its path relative to the .pqclip's folder when
//               the file lies below it, e.g. "Samples/Kick.wav", else absolute: resolvePath below).
//
// Ids inside a file are the ones the entity had when it was saved; whoever loads it gives everything fresh ids
// (ops::cloneDevice, ops::placeClip). Writing never fails except for values JSON cannot hold (as serializeProject).
// Not RT code.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "pq/core/result.h"
#include "pq/document/model.h"

namespace pq::doc {

inline constexpr std::string_view kPresetExtension = ".pqpreset";
inline constexpr std::string_view kUserDeviceExtension = ".pqdevice";
inline constexpr std::string_view kTuningExtension = ".pqtuning";
inline constexpr std::string_view kClipExtension = ".pqclip";

struct PresetFile {
  std::string name;
  DevicePtr device;
};
Result<std::string> serializePreset(const Device& device, std::string_view name);
Result<PresetFile> parsePreset(std::string_view text, std::string_view fileName = {});

struct UserDeviceFile {
  std::string name;                       // what the browser and the device menu show
  std::string patch;                      // the device patch's name in `text` (the stock device it was forked from)
  DeviceKind kind = DeviceKind::AudioEffect;
  std::string origin;                     // the stock file, relative to stdlib/ ("devices/saturator.pqp"); may be ""
  std::string text;                       // the whole patch text
  std::string panel;                      // a T4.6 panel description (JSON text) or ""
  friend bool operator==(const UserDeviceFile&, const UserDeviceFile&) = default;
};
Result<std::string> serializeUserDevice(const UserDeviceFile& f);
Result<UserDeviceFile> parseUserDevice(std::string_view text, std::string_view fileName = {});
// A new device instance playing a user device: `patch` / `kind` with the text as its own source (model.h UserPatch), so
// a project using it embeds the text and opens without the library (T4.4-fix1).
DevicePtr makeUserDevice(DeviceId id, const UserDeviceFile& f);

struct TuningFile {
  std::string name;
  Colour colour;
  TuningDef def;
};
Result<std::string> serializeTuning(const TuningFile& f);
Result<TuningFile> parseTuning(std::string_view text, std::string_view fileName = {});

struct ClipFile {
  std::string name;
  ClipSlot clip;                  // exactly one of clip.clip / clip.audio
  std::optional<SampleRef> sample;  // an audio clip's sample (its id = clip.audio->sample)
};
// A clip assigned to a main tuning is written with that tuning as a private one (the file has no main tunings), so its
// pitches survive the move to another project; `p` resolves it (nullptr: Main assignments become Inherit).
Result<std::string> serializeClip(const ClipFile& f, const Project* p = nullptr);
Result<ClipFile> parseClip(std::string_view text, std::string_view fileName = {});

// Paths inside library files: `stored` relative to the library file's folder `baseDirUtf8` unless absolute ("C:/…",
// "\\\\server\\…"). '/' separators either way.
std::string resolveLibraryPath(std::string_view baseDirUtf8, std::string_view stored);
// The path to store for `fileUtf8`: relative to `baseDirUtf8` when it lies below it, else `fileUtf8` (with '/').
std::string relativeLibraryPath(std::string_view baseDirUtf8, std::string_view fileUtf8);

// Reads a whole UTF-8 file (`pathUtf8`). Errors: NotFound, Io.
Result<std::string> readTextFile(std::string_view pathUtf8);
// Writes `text` atomically (a temporary file next to it, flushed, renamed over `pathUtf8`), creating the folder. Errors:
// Io.
Status writeTextFileAtomic(std::string_view pathUtf8, std::string_view text);

}  // namespace pq::doc
