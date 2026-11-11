#pragma once

// User-library files (T5.11; SPEC §9.1, §12; docs/plans/M5.md T5.11): one JSON format per kind, each written by the
// project serializer's own writers (serialize.h: the same device, clip, sample and tuning objects, the same strict
// reader, the same schema version), so a preset and a project never disagree about what a device is.
//
//   .pqpreset   {"format": "pqpreset", "schemaVersion", "name", "devices": [one device]}: a device preset (its patch,
//               kind, params and settings: a native engine, an atom or value block). The device object comes right
//               after the name, so its "kind" sits in the first bytes of the file, where the browser index sniffs the
//               category (QUESTIONS Q153).
//               GU.2: a GROUP PRESET is the same file with "devices": [the group, everything inside it at any depth]
//               and "wires": [the wires among them] (a block set, model.h BlockSet; spawning it = pasting it with fresh
//               ids, ops::pasteBlocks). GU.3 adds the group's panel to its device object (no new key here).
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
inline constexpr std::string_view kTuningExtension = ".pqtuning";
inline constexpr std::string_view kClipExtension = ".pqclip";

struct PresetFile {
  std::string name;
  DevicePtr device;  // the preset's device (a group preset: its group)
  BlockSet blocks;   // GU.2: every device of the file (the device first) and the wires among them
  bool groupPreset() const noexcept { return blocks.devices.size() > 1 || !blocks.wires.empty(); }
};
Result<std::string> serializePreset(const Device& device, std::string_view name);
// GU.2: a GROUP PRESET (a block set whose first device is its one top-level block, a group: ops::copyBlocks of a group).
Result<std::string> serializeGroupPreset(const BlockSet& blocks, std::string_view name);
Result<PresetFile> parsePreset(std::string_view text, std::string_view fileName = {});

struct TuningFile {
  std::string name;
  Colour colour;
  TuningDef def;
};
Result<std::string> serializeTuning(const TuningFile& f);
Result<TuningFile> parseTuning(std::string_view text, std::string_view fileName = {});

struct ClipFile {
  std::string name;
  ClipSlot clip;                  // exactly one of clip.clip / clip.audio; its tuning assignment is Inherit
  std::optional<SampleRef> sample;  // an audio clip's sample (its id = clip.audio->sample)
  // TU.3: the definition of the named tuning the clip was assigned to (file key "tuning"), so its pitches survive the
  // move to another project: loading assigns the project's tuning with this definition, or adds one (library_load).
  std::optional<TuningDef> tuning;
};
// A clip assigned to a named tuning is written Inherit with that tuning's definition in ClipFile::tuning (the file has
// no project tunings); `p` resolves it (nullptr: the assignment is dropped).
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
