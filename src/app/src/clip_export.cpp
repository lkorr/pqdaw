// T5.12: audio clips as WAV files (pq/app/clip_export.h).

#include "pq/app/clip_export.h"

#include <system_error>

#include "pq/app/project_link.h"
#include "pq/media/wav_file.h"
#include "pq/views/midi_export.h"

namespace pq::app {

namespace fs = std::filesystem;

namespace {

const doc::ClipSlot* findSlot(const doc::Project& p, doc::ClipRef ref) {
  const doc::Track* t = doc::findTrackPtr(p, ref.track);
  const int32_t i = t ? doc::findClipIndex(*t, ref.clip) : -1;
  return i < 0 ? nullptr : &t->clips[static_cast<size_t>(i)];
}

std::string safeStem(std::string name) {
  for (char& c : name)
    if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' ||
        static_cast<unsigned char>(c) < 32)
      c = '_';
  while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
  return name.empty() ? std::string("Clip") : name;
}

}  // namespace

double clipSeconds(const doc::Project& p, doc::ClipRef clip) {
  const doc::ClipSlot* c = findSlot(p, clip);
  if (c == nullptr) return 0.0;
  const TempoMap& tm = p.timeline->tempoMap;
  return tm.ticksToSeconds(c->end()) - tm.ticksToSeconds(c->start());
}

namespace {
fs::path freeClipPath(const doc::Project& p, doc::ClipRef clip, const fs::path& folder, std::string_view ext) {
  const doc::ClipSlot* c = findSlot(p, clip);
  const std::string stem = safeStem(c != nullptr ? c->name() : std::string());
  std::error_code ec;
  for (int n = 1;; ++n) {
    const std::string name = (n == 1 ? stem : stem + " " + std::to_string(n)) + std::string(ext);
    const fs::path f = folder / fs::path(std::u8string(name.begin(), name.end()));
    if (!fs::exists(f, ec)) return f;
  }
}

std::string utf8Of(const fs::path& f) {
  const std::u8string u = f.u8string();
  return std::string(u.begin(), u.end());
}
}  // namespace

fs::path clipWavPath(const doc::Project& p, doc::ClipRef clip, const fs::path& folder) {
  return freeClipPath(p, clip, folder, ".wav");
}

fs::path clipMidiPath(const doc::Project& p, doc::ClipRef clip, const fs::path& folder) {
  return freeClipPath(p, clip, folder, ".mid");
}

Status writeClipWav(ProjectLink& link, const doc::Project& p, doc::ClipRef clip, const fs::path& path, bool pcm24) {
  auto audio = link.renderClip(p, clip);
  if (!audio) return std::move(audio).error();
  if (audio->empty() || audio->front().empty()) return Error{ErrorCode::InvalidArgument, "the clip has no audio", {}};
  std::error_code ec;
  if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
  const auto rate = static_cast<uint32_t>(link.config().sampleRate);
  return pcm24 ? media::writeWavPcm24(path, *audio, rate) : media::writeWav(path, *audio, rate);
}

ClipDragOut dragOutClips(ProjectLink* link, const doc::Project& p, std::span<const doc::ClipRef> clips,
                         const fs::path& folder, bool pcm24, doc::MidiExportMode midiMode) {
  ClipDragOut out;
  for (const doc::ClipRef& ref : clips) {
    const doc::ClipSlot* c = findSlot(p, ref);
    if (c == nullptr) continue;
    if (c->clip) {  // MF.3: a note clip as a .mid file
      const fs::path f = clipMidiPath(p, ref, folder);
      Result<std::string> r = views::writeMidiClips(p, std::span<const doc::ClipRef>(&ref, 1), midiMode, utf8Of(f));
      if (!r) {
        out.refused = r.error().message;
        continue;
      }
      out.files.push_back(utf8Of(f));
      continue;
    }
    if (link == nullptr) {
      out.refused = "no playback engine to render the clip";
      continue;
    }
    if (clipSeconds(p, ref) > kLazyRenderSeconds) {
      out.lazy.push_back(ref);
      continue;
    }
    const fs::path f = clipWavPath(p, ref, folder);
    if (Status s = writeClipWav(*link, p, ref, f, pcm24); !s) {
      out.refused = s.error().message;
      continue;
    }
    out.files.push_back(utf8Of(f));
  }
  return out;
}

}  // namespace pq::app
