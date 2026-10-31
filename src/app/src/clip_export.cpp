// T5.12: audio clips as WAV files (pq/app/clip_export.h).

#include "pq/app/clip_export.h"

#include <system_error>

#include "pq/app/project_link.h"
#include "pq/media/wav_file.h"

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

fs::path clipWavPath(const doc::Project& p, doc::ClipRef clip, const fs::path& folder) {
  const doc::ClipSlot* c = findSlot(p, clip);
  const std::string stem = safeStem(c != nullptr ? c->name() : std::string());
  std::error_code ec;
  for (int n = 1;; ++n) {
    const std::string name = n == 1 ? stem + ".wav" : stem + " " + std::to_string(n) + ".wav";
    const fs::path f = folder / fs::path(std::u8string(name.begin(), name.end()));
    if (!fs::exists(f, ec)) return f;
  }
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

}  // namespace pq::app
