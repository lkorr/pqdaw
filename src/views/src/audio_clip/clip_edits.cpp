// SX.2: the audio clip edits shared by the clip panel and the radial clip menu: see pq/views/audio_clip/clip_edits.h.

#include "pq/views/audio_clip/clip_edits.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <span>

#include "pq/document/audio_clip.h"
#include "pq/document/pitch_resolve.h"
#include "pq/media/probe.h"
#include "pq/views/audio_clip/audio_clip_view.h"
#include "pq/views/sample_analysis.h"

namespace pq::views::clip_edits {

namespace {

constexpr double kConfident = 0.6;  // a detected tempo the warp commands trust (plan: auto-warp threshold)

std::span<const doc::ClipRef> one(const doc::ClipRef& ref) { return {&ref, 1}; }

bool dispatch(ViewContext& ctx, const doc::ops::EditResult& e, std::string_view what) {
  if (!e) {
    ctx.setStatus(std::string(what) + ": " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status s = ctx.store().dispatch(e->command); !s) {
    ctx.setStatus(std::string(what) + ": " + s.error().message);
    return false;
  }
  return true;
}

doc::AudioTranspose stepsOf(int64_t n) {
  doc::AudioTranspose t;
  t.unit = doc::TransposeUnit::Steps;
  t.steps = static_cast<int32_t>(std::clamp<int64_t>(n, -doc::kMaxTransposeSteps, doc::kMaxTransposeSteps));
  return t;
}

// Degree n of the tuning in cents above degree 0 (no tuning: 12-EDO semitones).
double stepCentsOf(const std::optional<tuning::Tuning>& tun, int64_t n) {
  if (!tun) return 100.0 * static_cast<double>(n);
  return doc::transposeCents(stepsOf(n), &*tun);
}

int64_t degreesPerEquave(const std::optional<tuning::Tuning>& tun) {
  return tun ? std::max<int64_t>(1, doc::degreesPerEquave(*tun)) : 12;
}

}  // namespace

// --- lookup ---

const doc::AudioClip* findAudioClip(const doc::Project& p, const doc::ClipRef& ref) noexcept {
  if (!ref.clip.valid()) return nullptr;
  const doc::Track* t = doc::findTrackPtr(p, ref.track);
  if (!t) return nullptr;
  const int32_t i = doc::findClipIndex(*t, ref.clip);
  return i >= 0 ? t->clips[static_cast<size_t>(i)].audio.get() : nullptr;
}

std::optional<tuning::Tuning> clipTuning(const doc::Project& p, const doc::ClipRef& ref) {
  const doc::AudioClip* c = findAudioClip(p, ref);
  if (!c) return std::nullopt;
  if (const std::optional<doc::TrackPath> tp = doc::findTrack(p, ref.track))
    if (const doc::TuningDef* def = doc::audioClipTuning(p, *tp, *c))
      if (Result<tuning::Tuning> t = doc::buildTuning(*def)) return std::move(*t);
  return std::nullopt;
}

double fileTempo(ViewContext& ctx, const doc::AudioClip& c) {
  const std::string path = ctx.samplePath(c.sample);
  if (path.empty()) return 0.0;
  const std::u8string u8(path.begin(), path.end());
  if (Result<media::SampleInfo> info = media::probe(std::filesystem::path(u8))) return info->bpm;
  return 0.0;
}

double detectedTempo(ViewContext& ctx, const doc::AudioClip& c) {
  const SampleAnalysisResult* an = ctx.sampleAnalysis().get(c.sample);
  if (!an || !an->tempo || an->tempo->estimate.confidence < kConfident) return 0.0;
  return an->tempo->estimate.bpm;
}

// --- transpose ---

double transposeCents(const std::optional<tuning::Tuning>& tun, const doc::AudioTranspose& t) {
  if (t.unit == doc::TransposeUnit::Steps) {
    if (tun) return doc::transposeCents(t, &*tun);
    return 100.0 * static_cast<double>(t.steps);  // no tuning resolves: semitones
  }
  return doc::transposeCents(t, nullptr);
}

double meanStepCents(const std::optional<tuning::Tuning>& tun) {
  const int64_t dpe = degreesPerEquave(tun);
  const double period = stepCentsOf(tun, dpe);
  return period > 0.0 ? period / static_cast<double>(dpe) : 100.0;
}

doc::AudioTranspose nearestStep(const std::optional<tuning::Tuning>& tun, double cents) {
  const int64_t dpe = degreesPerEquave(tun);
  const auto n0 = static_cast<int64_t>(std::llround(cents / meanStepCents(tun)));
  int64_t best = n0;
  double bd = 1e300;
  const int64_t reach = std::min<int64_t>(64, dpe / 2 + 2);  // an uneven scale's degree sits within half a period
  for (int64_t n = n0 - reach; n <= n0 + reach; ++n) {
    const double d = std::abs(stepCentsOf(tun, n) - cents);
    if (d < bd - 1e-9) {
      bd = d;
      best = n;
    }
  }
  return stepsOf(best);
}

std::optional<doc::AudioTranspose> steppedTranspose(const std::optional<tuning::Tuning>& tun, const doc::AudioTranspose& cur,
                                                    int dir, bool octave, int count) {
  if (dir == 0 || count <= 0) return std::nullopt;
  const int64_t dpe = degreesPerEquave(tun);
  const int64_t sign = dir > 0 ? 1 : -1;
  doc::AudioTranspose t = cur;
  if (octave) {
    // AC.fix: an octave (2/1, 12 semitones) whatever the tuning, the unit kept where it can be: Steps ± the degrees
    // per equave when the equave is the octave, else cents; a Ratio × or ÷ 2 (cents if that overflows).
    constexpr double kOctave = 1200.0;
    const double octaves = static_cast<double>(sign * count);
    switch (cur.unit) {
      case doc::TransposeUnit::Steps:
        if (std::abs(stepCentsOf(tun, dpe) - kOctave) < 1e-6) {
          t = stepsOf(static_cast<int64_t>(cur.steps) + sign * count * dpe);
        } else {
          t = doc::AudioTranspose{};
          t.cents = transposeCents(tun, cur) + octaves * kOctave;
        }
        break;
      case doc::TransposeUnit::Cents: t.cents = cur.cents + octaves * kOctave; break;
      case doc::TransposeUnit::Ratio: {
        std::optional<tuning::Rational> r = cur.ratio;
        for (int i = 0; r && i < count; ++i) r = sign > 0 ? tuning::mul(*r, tuning::Rational(2)) : tuning::div(*r, tuning::Rational(2));
        if (r) {
          t.ratio = *r;
        } else {
          t = doc::AudioTranspose{};
          t.cents = transposeCents(tun, cur) + octaves * kOctave;
        }
        break;
      }
    }
  } else if (cur.unit == doc::TransposeUnit::Steps) {
    t = stepsOf(static_cast<int64_t>(cur.steps) + sign * count);
  } else {
    // The next degree strictly above (below) the current pitch, then count − 1 more.
    const double c = transposeCents(tun, cur);
    int64_t n = nearestStep(tun, c).steps;
    for (int guard = 0; guard < 4096 && (sign > 0 ? stepCentsOf(tun, n) <= c + 1e-6 : stepCentsOf(tun, n) >= c - 1e-6); ++guard)
      n += sign;
    for (int guard = 0; guard < 4096 && (sign > 0 ? stepCentsOf(tun, n - 1) > c + 1e-6 : stepCentsOf(tun, n + 1) < c - 1e-6); ++guard)
      n -= sign;
    t = stepsOf(n + sign * (count - 1));
  }
  if (std::abs(transposeCents(tun, t)) > doc::kMaxTransposeCents + 1e-6) return std::nullopt;
  return t;
}

doc::AudioTranspose nearestRatio(double cents, int64_t maxDen) {
  cents = std::clamp(cents, -doc::kMaxTransposeCents, doc::kMaxTransposeCents);
  const double x = std::exp2(cents / 1200.0);
  int64_t bn = 1, bd = 1;
  double best = 1e300;
  for (int64_t d = 1; d <= std::max<int64_t>(1, maxDen); ++d) {
    for (const int64_t n : {static_cast<int64_t>(std::floor(x * static_cast<double>(d))), static_cast<int64_t>(std::ceil(x * static_cast<double>(d)))}) {
      if (n <= 0) continue;
      const double c = 1200.0 * std::log2(static_cast<double>(n) / static_cast<double>(d));
      if (std::abs(c) > doc::kMaxTransposeCents + 1e-9) continue;
      const double err = std::abs(c - cents);
      if (err < best - 1e-9) {  // ties: the smaller denominator (found first)
        best = err;
        bn = n;
        bd = d;
      }
    }
  }
  doc::AudioTranspose t;
  t.unit = doc::TransposeUnit::Ratio;
  t.ratio = tuning::Rational(bn, bd);
  return t;
}

doc::AudioTranspose inUnit(const std::optional<tuning::Tuning>& tun, const doc::AudioTranspose& t, doc::TransposeUnit unit) {
  if (t.unit == unit) return t;
  const double c = transposeCents(tun, t);
  switch (unit) {
    case doc::TransposeUnit::Cents: {
      doc::AudioTranspose r;
      r.cents = c;
      return r;
    }
    case doc::TransposeUnit::Steps: return nearestStep(tun, c);
    case doc::TransposeUnit::Ratio: return nearestRatio(c, 32);
  }
  return t;
}

// --- edits ---

bool setGain(ViewContext& ctx, const doc::ClipRef& ref, float db) {
  const doc::AudioClip* c = findAudioClip(*ctx.doc(), ref);
  if (!c || db == c->gainDb) return false;
  return dispatch(ctx, doc::ops::setAudioClipGain(*ctx.doc(), one(ref), db), "Clip Gain");
}

bool setTranspose(ViewContext& ctx, const doc::ClipRef& ref, const doc::AudioTranspose& t) {
  const doc::AudioClip* c = findAudioClip(*ctx.doc(), ref);
  if (!c || t == c->transpose) return false;
  return dispatch(ctx, doc::ops::setAudioClipTranspose(*ctx.doc(), one(ref), t), "Transpose");
}

bool setTransposeText(ViewContext& ctx, const doc::ClipRef& ref, std::string_view text) {
  if (!findAudioClip(*ctx.doc(), ref)) return false;
  const std::optional<doc::AudioTranspose> t = parseTranspose(text);
  if (!t) {
    ctx.setStatus("Transpose: type 7 (steps), 386.3c (cents), +7st (semitones) or 5/4 (a ratio)");
    return false;
  }
  return setTranspose(ctx, ref, *t);
}

bool setDetune(ViewContext& ctx, const doc::ClipRef& ref, float cents) {
  const doc::AudioClip* c = findAudioClip(*ctx.doc(), ref);
  if (!c || cents == c->detuneCents) return false;
  return dispatch(ctx, doc::ops::setAudioClipDetune(*ctx.doc(), one(ref), cents), "Detune");
}

bool setReverse(ViewContext& ctx, const doc::ClipRef& ref, bool on) {
  const doc::AudioClip* c = findAudioClip(*ctx.doc(), ref);
  if (!c || on == c->reverse) return false;
  return dispatch(ctx, doc::ops::setAudioClipsReverse(*ctx.doc(), one(ref), on), "Reverse");
}

bool setWarp(ViewContext& ctx, const doc::ClipRef& ref, doc::AudioWarp warp, double fileBpm) {
  const doc::AudioClip* c = findAudioClip(*ctx.doc(), ref);
  if (!c || warp == c->warp) return false;
  double bpm = c->sourceBpm;
  if (!doc::isWarped(c->warp) && doc::isWarped(warp)) {  // from Off: seed the source tempo
    const doc::SampleRef* s = doc::findSample(*ctx.doc(), c->sample);
    const double detected = fileBpm >= kMinBpm && fileBpm <= kMaxBpm ? 0.0 : detectedTempo(ctx, *c);
    bpm = sourceBpmFor(detected > 0.0 ? detected : fileBpm, s ? static_cast<double>(s->frames) / std::max(1.0, s->rate) : 0.0);
  }
  return dispatch(ctx, doc::ops::setAudioClipWarp(*ctx.doc(), one(ref), warp, bpm), "Warp");
}

bool setSourceBpm(ViewContext& ctx, const doc::ClipRef& ref, double bpm) {
  const doc::AudioClip* c = findAudioClip(*ctx.doc(), ref);
  if (!c || !doc::isWarped(c->warp) || bpm == c->sourceBpm) return false;
  return dispatch(ctx, doc::ops::setAudioClipWarp(*ctx.doc(), one(ref), c->warp, bpm), "Clip Tempo");
}

bool scaleSourceBpm(ViewContext& ctx, const doc::ClipRef& ref, double factor) {
  const doc::AudioClip* c = findAudioClip(*ctx.doc(), ref);
  if (!c || !doc::isWarped(c->warp)) return false;
  const double bpm = c->sourceBpm * factor;
  if (bpm < kMinBpm || bpm > kMaxBpm) return false;
  return setSourceBpm(ctx, ref, bpm);
}

bool setLoopEnabled(ViewContext& ctx, const doc::ClipRef& ref, bool on) {
  const doc::AudioClip* c = findAudioClip(*ctx.doc(), ref);
  if (!c || !doc::isWarped(c->warp) || on == c->loop.enabled) return false;
  doc::ClipLoop l = c->loop;
  l.enabled = on;
  if (l.end <= l.start) {  // a fresh loop: the clip's own span of content
    l.start = c->contentStart;
    l.end = c->contentStart + c->length;
  }
  return dispatch(ctx, doc::ops::setAudioClipLoop(*ctx.doc(), ref, l), "Loop");
}

bool setFadesMs(ViewContext& ctx, const doc::ClipRef& ref, double inMs, double outMs, std::optional<float> curveIn,
                std::optional<float> curveOut) {
  const doc::AudioClip* c = findAudioClip(*ctx.doc(), ref);
  if (!c) return false;
  const doc::AudioClip& a = *c;
  const TempoMap& tm = ctx.doc()->timeline->tempoMap;
  const double t0 = tm.ticksToSeconds(a.start), t1 = tm.ticksToSeconds(a.start + a.length);
  doc::AudioFade fi = a.fadeIn, fo = a.fadeOut;
  fi.length = std::clamp<Ticks>(static_cast<Ticks>(std::llround(tm.secondsToTicks(t0 + std::max(0.0, inMs) / 1000.0))) - a.start, 0, a.length);
  fo.length = std::clamp<Ticks>(a.start + a.length - static_cast<Ticks>(std::llround(tm.secondsToTicks(t1 - std::max(0.0, outMs) / 1000.0))),
                                0, a.length);
  if (curveIn) fi.curve = std::clamp(*curveIn, -1.0f, 1.0f);
  if (curveOut) fo.curve = std::clamp(*curveOut, -1.0f, 1.0f);
  if (fi == a.fadeIn && fo == a.fadeOut) return false;
  return dispatch(ctx, doc::ops::setAudioClipFades(*ctx.doc(), ref, fi, fo), "Clip Fade");
}

bool setWarpParams(ViewContext& ctx, const doc::ClipRef& ref, const doc::WarpParams& p) {
  const doc::AudioClip* c = findAudioClip(*ctx.doc(), ref);
  if (!c || p == c->warpParams) return false;
  return dispatch(ctx, doc::ops::setAudioClipWarpParams(*ctx.doc(), one(ref), p), "Warp Settings");
}

FadesMs fadesMs(const doc::Project& p, const doc::AudioClip& a) {
  const TempoMap& tm = p.timeline->tempoMap;
  const double t0 = tm.ticksToSeconds(a.start), t1 = tm.ticksToSeconds(a.start + a.length);
  FadesMs f;
  f.clipMs = std::max(1.0, std::floor((t1 - t0) * 1000.0));
  const Ticks fi = std::clamp<Ticks>(a.fadeIn.length, 0, a.length), fo = std::clamp<Ticks>(a.fadeOut.length, 0, a.length - fi);
  f.in = std::round((tm.ticksToSeconds(a.start + fi) - t0) * 1000.0);
  f.out = std::round((t1 - tm.ticksToSeconds(a.start + a.length - fo)) * 1000.0);
  return f;
}

// --- specs ---

ui::ValueSpec transposeSpec() {
  ui::ValueSpec s;
  s.min = -doc::kMaxTransposeCents;
  s.max = doc::kMaxTransposeCents;
  s.def = 0.0;
  s.dragPixels = 768.0;  // ~8 px per 100 c
  s.fineFactor = 0.1;
  s.decimals = 1;
  s.unit = "\xC2\xA2";
  return s;
}

ui::ValueSpec detuneSpec() {
  ui::ValueSpec s;
  s.min = -doc::kMaxDetuneCents;
  s.max = doc::kMaxDetuneCents;
  s.def = 0.0;
  s.dragPixels = 300.0;
  s.keyStep = 1.0;
  s.decimals = 0;
  s.unit = "\xC2\xA2";
  return s;
}

ui::ValueSpec gainSpec() {
  ui::ValueSpec s;
  s.min = doc::kMinClipGainDb;
  s.max = doc::kMaxClipGainDb;
  s.def = 0.0;
  s.dragPixels = 400.0;
  s.keyStep = 0.5;
  s.decimals = 1;
  s.unit = "dB";
  return s;
}

ui::ValueSpec fadeSpec(double clipMs) {
  ui::ValueSpec s;
  s.min = 0.0;
  s.max = clipMs;  // a fade cannot be longer than the clip, so a fill is the fade's share of it
  s.def = 0.0;
  s.dragPixels = 200.0;
  s.keyStep = 1.0;
  s.decimals = 0;
  s.unit = "ms";
  return s;
}

ui::ValueSpec bpmSpec() {
  ui::ValueSpec s;
  s.min = 20.0;
  s.max = 999.0;
  s.def = 120.0;
  s.dragPixels = 1000.0;
  s.keyStep = 1.0;
  s.decimals = 2;
  return s;
}

namespace {
ui::ValueSpec percent(double def) {
  ui::ValueSpec s;
  s.min = 0.0;
  s.max = 100.0;
  s.def = def;
  s.dragPixels = 300.0;
  s.keyStep = 1.0;
  s.decimals = 0;
  s.unit = "%";
  return s;
}
}  // namespace

ui::ValueSpec beatsEnvelopeSpec() { return percent(100.0); }
ui::ValueSpec fluxSpec() { return percent(0.0); }
ui::ValueSpec formantsSpec() { return percent(100.0); }

ui::ValueSpec grainSpec() {
  ui::ValueSpec s;
  s.min = doc::kMinGrainMs;
  s.max = doc::kMaxGrainMs;
  s.def = 30.0;
  s.dragPixels = 400.0;
  s.keyStep = 1.0;
  s.decimals = 0;
  s.unit = "ms";
  return s;
}

ui::ValueSpec envelopeOrderSpec() {
  ui::ValueSpec s;
  s.min = doc::kMinWarpEnvelope;
  s.max = doc::kMaxWarpEnvelope;
  s.def = 128.0;
  s.dragPixels = 400.0;
  s.keyStep = 1.0;
  s.decimals = 0;
  s.unit = "bins";  // M6 review L19: a count, not Beats' "Envelope" percentage
  return s;
}

std::string formatDetune(double v) {
  char b[32];
  if (std::round(v) == 0.0) std::snprintf(b, sizeof b, "0 \xC2\xA2");
  else std::snprintf(b, sizeof b, "%+.0f \xC2\xA2", v);
  return b;
}

std::string formatGain(double v) {
  if (v <= doc::kMinClipGainDb) return "-inf dB";
  char b[32];
  std::snprintf(b, sizeof b, "%.1f dB", v);
  return b;
}

int32_t granulationIndex(Ticks g) noexcept {
  for (int32_t i = 0; i < static_cast<int32_t>(std::size(kGranulations)); ++i)
    if (kGranulations[i] == g) return i;
  return 3;
}

}  // namespace pq::views::clip_edits
