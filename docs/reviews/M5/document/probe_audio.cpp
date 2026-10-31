// M5 review probe (document, audio clips): split of a reversed warp-Off clip, tempo edits on adjacent Off clips,
// the .pqclip reader's validation, placeClip of a crafted library clip.
// Build: build_probe.ps1 next to this file (links the release pq_doc / pq_graph / pq_tuning / pq_dsp / pq_core libs).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "pq/document/audio_clip.h"
#include "pq/document/audio_sequence.h"
#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/document/library_files.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"

using namespace pq;
using namespace pq::doc;

static ops::Edit must(Result<ops::Edit> e, DocumentStore& s, const char* what) {
  if (!e) {
    std::printf("  %s: builder refused: %s\n", what, e.error().message.c_str());
    std::exit(2);
  }
  const Status st = s.dispatch(e->command);
  if (!st) {
    std::printf("  %s: dispatch refused: %s\n", what, st.error().message.c_str());
    std::exit(2);
  }
  return *e;
}

struct Fixture {
  DocumentStore store;
  TrackId audio;
  SampleId sample;
  Fixture() {
    audio = must(ops::createTrack(*store.current(), store.ids(), TrackKind::Audio), store, "track").tracks.at(0);
    SampleRef s;
    s.id = store.ids().sample();
    s.path = "C:/samples/loop.wav";
    s.size = 384044;
    s.frames = 96000;
    s.rate = 48000.0;
    s.channels = 2;
    s.displayName = "loop";
    sample = s.id;
    if (!store.dispatch(AddSample{s})) std::exit(2);
  }
  const Project& doc() const { return *store.current(); }
  ClipRef drop(Ticks start) {
    const ops::NewAudioClip nc{audio, start, sample, {}};
    return must(ops::addAudioClips(doc(), store.ids(), {}, std::span<const ops::NewAudioClip>(&nc, 1)), store, "drop")
        .clips.at(0);
  }
  AudioTrackDesc lowered() const { return lowerAudioTrack(doc(), *findTrack(doc(), audio)); }
  const Track& track() const { return *findTrackPtr(doc(), audio); }
};

// Max |frame difference| between two lowerings over [from, to).
static double maxFrameDiff(const AudioTrackDesc& a, const AudioTrackDesc& b, const TempoMap& tm, double from, double to) {
  double worst = 0.0;
  for (int i = 0; i < 20000; ++i) {
    const double tau = from + (to - from) * (i + 0.5) / 20000.0;
    std::optional<double> fa, fb;
    for (const AudioClipDesc& c : a.clips)
      if (auto f = sourceFrameAt(c, tm, tau)) fa = f;
    for (const AudioClipDesc& c : b.clips)
      if (auto f = sourceFrameAt(c, tm, tau)) fb = f;
    if (fa && fb) worst = std::max(worst, std::abs(*fa - *fb));
  }
  return worst;
}

static void probeReverseSplit() {
  std::printf("probe 1: split of a REVERSED warp-Off clip that was never trimmed (tempo ramp):\n");
  for (const bool reverse : {false, true}) {
    Fixture f;
    if (!f.store.dispatch(SetTempoMap{{TempoPoint{0, 137.5, TempoCurve::Linear}, TempoPoint{4 * kTicksPerWhole, 97.0, TempoCurve::Step}}}))
      std::exit(2);
    const ClipRef r = f.drop(kTicksPerWhole / 3);
    const std::span<const ClipRef> one(&r, 1);
    if (reverse) must(ops::setAudioClipsReverse(f.doc(), one, true), f.store, "reverse");
    const AudioClip c0 = *f.track().clips[0].audio;
    const TempoMap& tm = f.doc().timeline->tempoMap;
    std::printf("  reverse=%d: tau(end tick) - (tau(start) + seconds) = %.3e s\n", reverse,
                tm.ticksToSeconds(clipEnd(c0)) - (tm.ticksToSeconds(c0.start) + c0.seconds));
    const AudioTrackDesc before = f.lowered();
    const Ticks cut = c0.start + c0.length / 3 + 17;
    must(ops::splitClips(f.doc(), f.store.ids(), one, std::span<const Ticks>(&cut, 1)), f.store, "split");
    const double t0 = tm.ticksToSeconds(c0.start), t1 = tm.ticksToSeconds(clipEnd(c0));
    std::printf("  reverse=%d: clips after split %zu, max |frame diff| vs before = %.3e frames\n", reverse,
                f.track().clips.size(), maxFrameDiff(before, f.lowered(), tm, t0, t1));
  }
}

static void probeTempoOverlap() {
  std::printf("probe 2: two adjacent warp-Off clips with a crossfade, then the tempo doubles:\n");
  Fixture f;
  const ClipRef a = f.drop(0);
  const ClipRef b = f.drop(kTicksPerWhole);  // 2 s at 120 BPM = one bar: adjacent
  must(ops::setAudioClipCrossfade(f.doc(), b, kPpq / 2), f.store, "xfade");
  if (!f.store.dispatch(SetTempoMap{{TempoPoint{0, 240.0, TempoCurve::Step}}})) std::exit(2);
  const Track& t = f.track();
  std::printf("  clip a [%lld, %lld)  clip b [%lld, %lld)  overlap = %lld ticks (%.3f s); b.crossfadeIn = %lld\n",
              (long long)t.clips[0].start(), (long long)t.clips[0].end(), (long long)t.clips[1].start(),
              (long long)t.clips[1].end(), (long long)(t.clips[0].end() - t.clips[1].start()),
              f.doc().timeline->tempoMap.ticksToSeconds(t.clips[0].end()) -
                  f.doc().timeline->tempoMap.ticksToSeconds(t.clips[1].start()),
              (long long)t.clips[1].audio->crossfadeIn);
  const AudioTrackDesc d = f.lowered();
  int both = 0;
  for (int i = 0; i < 1000; ++i) {
    const double tau = 4.0 * (i + 0.5) / 1000.0;
    int n = 0;
    for (const AudioClipDesc& c : d.clips)
      if (sourceFrameAt(c, f.doc().timeline->tempoMap, tau)) ++n;
    both += n > 1;
  }
  std::printf("  probes (of 1000 over 4 s) where both clips play at once: %d\n", both);
  // Back to 120 BPM: the crossfade is gone for good (the undo of the tempo edit restores it; a new tempo edit does not).
  if (!f.store.dispatch(SetTempoMap{{TempoPoint{0, 120.0, TempoCurve::Step}}})) std::exit(2);
  std::printf("  tempo back to 120 (new edit): adjacent again = %d, crossfadeIn = %lld\n",
              f.track().clips[0].end() == f.track().clips[1].start(), (long long)f.track().clips[1].audio->crossfadeIn);
}

static void probeClipFile() {
  std::printf("probe 3: .pqclip reader validation of the audio clip:\n");
  Fixture f;
  const ClipRef r = f.drop(0);
  ClipFile cf;
  cf.name = "x";
  cf.clip = f.track().clips[0];
  cf.sample = *findSample(f.doc(), f.sample);
  auto text = serializeClip(cf, &f.doc());
  if (!text) std::exit(2);
  struct Mut {
    const char* what;
    const char* from;
    const char* to;
  } muts[] = {
      {"gainDb 1e30", "\"gainDb\": 0.0", "\"gainDb\": 1e30"},
      {"seconds -5", "\"seconds\": 2.0", "\"seconds\": -5.0"},
      {"sourceBpm 0", "\"sourceBpm\": 120.0", "\"sourceBpm\": 0.0"},
      {"detune 1e9", "\"detuneCents\": 0.0", "\"detuneCents\": 1e9"},
      {"fade -7", "\"fadeIn\": {\"length\": 0", "\"fadeIn\": {\"length\": -7"},
      {"xfade -1", "\"crossfadeIn\": 0", "\"crossfadeIn\": -1"},
      {"cents 1e300", "\"cents\": 0.0", "\"cents\": 1e300"},
  };
  for (const Mut& m : muts) {
    std::string t = *text;
    const size_t at = t.find(m.from);
    if (at == std::string::npos) {
      std::printf("  [%s] pattern not found\n", m.what);
      continue;
    }
    t.replace(at, std::string(m.from).size(), m.to);
    auto parsed = parseClip(t);
    if (!parsed) {
      std::printf("  [%s] parseClip refused: %s\n", m.what, parsed.error().message.c_str());
      continue;
    }
    SampleRef s = *parsed->sample;
    s.id = f.store.ids().sample();
    auto placed = ops::placeClip(f.doc(), f.store.ids(), f.audio, parsed->clip, 10 * kTicksPerWhole, s);
    if (!placed) {
      std::printf("  [%s] parseClip ACCEPTED; placeClip refused: %s\n", m.what, placed.error().message.c_str());
      continue;
    }
    const Status st = f.store.dispatch(placed->command);
    std::printf("  [%s] parseClip ACCEPTED; placeClip built; dispatch: %s\n", m.what,
                st ? "ok (INVALID CLIP IN DOCUMENT)" : st.error().message.c_str());
  }
  (void)r;
}

static void probeSchema6Audio() {
  std::printf("probe 4: a schema-6 project with an audio track loads?\n");
  Fixture f;
  f.drop(0);
  auto text = serializeProject(f.doc());
  if (!text) std::exit(2);
  std::string t = *text;
  const size_t at = t.find("\"schemaVersion\": 7");
  t.replace(at, 18, "\"schemaVersion\": 6");
  auto p = parseProject(t);
  std::printf("  %s\n", p ? "ACCEPTED (schema 6 had no audio)" : p.error().message.c_str());
}

int main() {
  probeReverseSplit();
  probeTempoOverlap();
  probeClipFile();
  probeSchema6Audio();
  return 0;
}
