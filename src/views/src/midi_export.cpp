// MF.3: note clips → .mid files (pq/views/midi_export.h).

#include "pq/views/midi_export.h"

#include <filesystem>

#include "pq/document/library_files.h"
#include "pq/views/view_context.h"

namespace pq::views {

std::vector<doc::ClipRef> noteClipsOf(const doc::Project& p, std::span<const doc::ClipRef> clips) {
  std::vector<doc::ClipRef> out;
  for (const doc::ClipRef& r : clips) {
    const doc::Track* t = doc::findTrackPtr(p, r.track);
    const int32_t i = t ? doc::findClipIndex(*t, r.clip) : -1;
    if (i >= 0 && t->clips[static_cast<size_t>(i)].clip) out.push_back(r);
  }
  return out;
}

Result<std::string> writeMidiClips(const doc::Project& p, std::span<const doc::ClipRef> clips, doc::MidiExportMode mode,
                                   std::string_view pathUtf8) {
  doc::ExportMidiReport rep;
  doc::ExportMidiOptions opt;
  opt.mode = mode;
  Result<std::vector<uint8_t>> bytes = doc::exportMidi(p, clips, opt, &rep);
  if (!bytes) return std::move(bytes).error();
  std::string path(pathUtf8);
  if (std::filesystem::path(std::u8string(path.begin(), path.end())).extension().empty()) path += ".mid";
  const std::string_view data(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  if (Status s = doc::writeTextFileAtomic(path, data); !s) return std::move(s).error();
  return doc::describeMidiExport(rep, mode);
}

void exportMidiClips(ViewContext& ctx, std::vector<doc::ClipRef> clips, doc::MidiExportMode mode) {
  ViewPrefs prefs = ctx.viewPrefs();
  prefs.midiExportMode = mode;
  ctx.setViewPrefs(prefs);
  clips = noteClipsOf(*ctx.doc(), clips);
  if (clips.empty()) {
    ctx.setStatus("Export MIDI: select a MIDI clip");
    return;
  }
  FileDialogs* dialogs = ctx.fileDialogs();
  if (dialogs == nullptr) {
    ctx.setStatus("Export MIDI: no file dialog");
    return;
  }
  const doc::Track* t = doc::findTrackPtr(*ctx.doc(), clips.front().track);
  const int32_t i = t ? doc::findClipIndex(*t, clips.front().clip) : -1;
  std::string name = i >= 0 ? t->clips[static_cast<size_t>(i)].name() : std::string();
  if (name.empty()) name = "Clip";
  dialogs->chooseMidiFileToSave(name + ".mid", [&ctx, clips, mode](std::string path) {
    if (path.empty()) return;  // cancelled
    const std::vector<doc::ClipRef> now = noteClipsOf(*ctx.doc(), clips);
    if (now.empty()) {
      ctx.setStatus("Export MIDI: the clips are gone");
      return;
    }
    Result<std::string> r = writeMidiClips(*ctx.doc(), now, mode, path);
    ctx.setStatus(r ? *r + " to " + path : "Export MIDI: " + r.error().message);
  });
}

}  // namespace pq::views
