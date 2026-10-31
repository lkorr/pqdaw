// --render (T3.17): `pqdaw --project <dir> --render out.wav [--seconds N]`, the project rendered offline through
// exactly the playback path (ProjectLink → Engine → ProjectProgram + Timeline), deterministic.
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "pq/app/commands.h"
#include "pq/app/project_link.h"
#include "pq/document/clip_time.h"
#include "pq/document/serialize.h"
#include "pq/engine/wav_file.h"

namespace pq::app {

namespace {

class WavSink final : public engine::AudioSink {
public:
  explicit WavSink(std::filesystem::path path) : path_(std::move(path)) {}
  Status begin(uint32_t ch, uint32_t sr, uint64_t) override { return w_.open(path_, ch, sr); }
  Status write(const float* const* ch, uint32_t frames) override { return w_.write(ch, frames); }
  Status end() override { return w_.close(); }

private:
  std::filesystem::path path_;
  engine::WavWriter w_;
};

std::filesystem::path pathFromUtf8(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// M3 review L5: a project's default length is refused above this (a clip near the end of the tick range made
// --render write ~10^10 s); --seconds renders any length up to its own 24 h limit.
constexpr double kMaxDefaultRenderSeconds = 4.0 * 3600.0;

double defaultRenderSeconds(const doc::Project& p) {
  Ticks end = 0;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::ClipSlot& c : t.clips) end = std::max(end, c.end());
  });
  const double s = p.timeline->tempoMap.ticksToSeconds(end) + 2.0;  // + release tails
  return std::max(s, 2.0);
}

}  // namespace

int runRender(const CliOptions& o) {
  auto loaded = doc::loadProject(o.project);
  if (!loaded) {
    std::fprintf(stderr, "pqdaw: %s\n", loaded.error().message.c_str());
    return 1;
  }
  const double seconds = o.secondsGiven ? o.seconds : defaultRenderSeconds(*loaded->project);
  if (!o.secondsGiven && !(seconds <= kMaxDefaultRenderSeconds)) {
    std::fprintf(stderr, "pqdaw: the project is %.0f s long (its last clip ends there): pass --seconds to render that much "
                         "(the default is at most %.0f s)\n", seconds, kMaxDefaultRenderSeconds);
    return 1;
  }
  LinkConfig cfg;
  cfg.compileThread = false;
  cfg.sleepAfterMs = 0.0f;  // an offline render never sleeps
  cfg.chanceSeed = o.seed;  // seeded: the same file and seed render the same way (M3 review M16)
  cfg.projectDir = o.project;  // T5.4: project-relative sample paths resolve here
  auto link = ProjectLink::create(cfg, loaded->project);
  if (!link) {
    std::fprintf(stderr, "pqdaw: %s\n", link.error().message.c_str());
    return 1;
  }
  const auto frames = static_cast<uint64_t>(std::llround(seconds * cfg.sampleRate));
  WavSink sink{pathFromUtf8(o.outPath)};
  auto st = (*link)->renderOffline(0, frames, sink);
  if (!st) {
    std::fprintf(stderr, "pqdaw: %s\n", st.error().message.c_str());
    return 1;
  }
  const LinkStats& ls = (*link)->stats();
  std::printf("rendered: %s, %.3f s (%llu frames, %u ch, %.0f Hz) in %.3f s (%.1fx realtime); %llu units compiled\n",
              o.outPath.c_str(), seconds, static_cast<unsigned long long>(st->frames), st->numChannels, cfg.sampleRate,
              st->wallSeconds, st->realtimeFactor, static_cast<unsigned long long>(ls.unitCompiles));
  return 0;
}

}  // namespace pq::app
