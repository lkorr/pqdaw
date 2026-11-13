// The Noise engine (NZ.1): pq/meta/noise.h.
#include "pq/meta/noise.h"

#include <array>
#include <cmath>
#include <string>

#include "pq/dsp/noise_step.h"

namespace pq::meta {

namespace {

constexpr std::array<InputSpec, 1> kInputs = {{
    {"clock", "Clock in", InputDefault::Unwired, graph::Kind::Real, Reads::Signal},
}};
constexpr std::array<ParamSpec, kNoiseParams> kParams = {{
    {"rate", "Rate", kNoiseRateTop, 0.01, kNoiseRateTop, graph::Unit::Hz, dsp::CurvePreset::Octave, 0.0, false, false},
    {"sync", "Sync", static_cast<double>(kNoiseSyncDefault), 0.0, kNoiseSyncCount - 1.0, graph::Unit::None,
     dsp::CurvePreset::Linear, 0.0, false, false, false, kNoiseSyncCount},
    {"smooth", "Smooth", 0.0, 0.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"colour", "Colour", 0.0, dsp::kNoiseColourMin, dsp::kNoiseColourMax, graph::Unit::None, dsp::CurvePreset::Linear, 0.0,
     false, false},
    {"density", "Density", dsp::kNoiseDensityContinuous, 0.1, dsp::kNoiseDensityContinuous, graph::Unit::Hz,
     dsp::CurvePreset::Octave, 0.0, false, false},
    {"steps", "Steps", 0.0, 0.0, 64.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false, false, 65},
    {"min", "Min", -1.0, -1.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"max", "Max", 1.0, -1.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"seed", "Seed", 0.0, 0.0, 999.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false, false, 1000},
}};
constexpr std::array<SettingSpec, kNoiseSettings> kSettings = {{
    {"clock", "Clock", 0.0, 0.0, 2.0, 3},      // Rate, Sync, Note
    {"dist", "Distribution", 0.0, 0.0, 1.0, 2},  // Uniform, Gaussian
    {"restart", "Restart", 0.0, 0.0, 2.0, 3},  // On play, Free, On note
    {"voices", "Voices", 0.0, 0.0, 1.0, 2},    // Per voice, Shared
}};
constexpr std::array<OutputSpec, 1> kOutputs = {{{"out", graph::kWidthMono}}};

// Body node ids: the stateful nodes fixed (their state keys survive every re-expansion, a setting change included);
// the rest in build order from kSeq.
enum : uint32_t { kStep, kTap, kSeq = 16 };

class Body {
public:
  using P = Builder::PortRef;
  explicit Body(Builder& b) : b_(b) {}
  P op(std::string_view atom, std::initializer_list<P> ins) { return b_.atom(atom, ins, next_++); }
  P mux(std::initializer_list<P> ins) { return b_.atom("mux", ins, next_++, static_cast<uint16_t>(ins.size() - 1)); }
  P k(double v) { return b_.konst(v, next_++); }
  P named(P v, const std::string& name) {
    b_.bind(v, name);
    return v;
  }

private:
  Builder& b_;
  uint32_t next_ = kSeq;
};

uint32_t choice(double v, uint32_t n) noexcept {
  if (!std::isfinite(v) || v < 0.0) return 0;
  const auto i = static_cast<uint32_t>(std::lround(v));
  return i < n ? i : n - 1;
}

Status body(Builder& bld) {
  using P = Builder::PortRef;
  const P none{graph::kNone, 0};
  Body b(bld);
  const auto mode = static_cast<NoiseClock>(choice(bld.setting(kNoiseClock), 3));
  const bool gauss = choice(bld.setting(kNoiseDist), 2) == 1;
  const auto restart = static_cast<NoiseRestart>(choice(bld.setting(kNoiseRestart), 3));
  const bool shared = choice(bld.setting(kNoiseVoices), 2) == 1;
  const bool voiced = bld.context().voice;

  // A wired clock overrides the clock setting: `wired` folds to false while the input reads its default.
  const P clock = bld.input(0);
  const P wired = b.named(b.op("gt", {clock, b.k(kUnwiredInput * 0.5)}), "wired");
  const P clockHigh = b.op("gt", {clock, b.k(0.0)});  // the atom steps on its rising edges
  // A note-on: the voice's own, or (Shared) any voice's.
  P noteOn = none;
  if (voiced) {
    noteOn = b.named(b.op("voice.trigger", {}), "note_on");
    if (shared) noteOn = b.named(b.op("voice_sum", {noteOn}), "any_note_on");
  }

  P rateIn = none, beatIn = none, gridIn = none;  // (rate unconnected reads the atom's default, every frame: set below)
  if (mode == NoiseClock::Rate) {
    // The knob's top is a new value every frame (any sample rate).
    const P rate = bld.param(kNoiseRate);
    const P eff = b.op("select", {b.op("ge", {rate, b.k(kNoiseRateTop * (1.0 - 1e-6))}), b.k(1.0e9), rate});
    rateIn = b.op("select", {wired, b.k(0.0), eff});
  } else if (mode == NoiseClock::Sync) {
    // The grid in beats: 1/16 · 2^⌊i/3⌋ · (2/3, 1, 3/2)[i mod 3] (consts through muxes: param rate, never glided).
    const P sync = bld.param(kNoiseSync);
    const P straight = b.named(b.op("floor", {b.op("mul", {sync, b.k(1.0 / 3.0)})}), "sync_straight");
    const P variant = b.op("sub", {sync, b.op("mul", {straight, b.k(3.0)})});
    const P base = b.mux({straight, b.k(1.0 / 16.0), b.k(1.0 / 8.0), b.k(1.0 / 4.0), b.k(1.0 / 2.0), b.k(1.0), b.k(2.0),
                                b.k(4.0), b.k(8.0), b.k(16.0)});
    const P factor = b.mux({variant, b.k(2.0 / 3.0), b.k(1.0), b.k(1.5)});
    beatIn = b.op("transport.beat", {});
    gridIn = b.op("select", {wired, b.k(0.0), b.op("mul", {base, factor})});
  }
  if (rateIn.node == graph::kNone) rateIn = b.k(0.0);  // Sync, Note: no rate
  // Steps by trig: the clock's edges when wired; else note-ons (the Note clock, and each voice's own note-on in a per-
  // voice instance: its value and its rate start at the note).
  const bool notes = noteOn.node != graph::kNone && (mode == NoiseClock::Note || !shared);
  const P trigIn = notes ? b.op("select", {wired, clockHigh, noteOn}) : b.op("and", {wired, clockHigh});
  P resetIn = none;
  if (restart == NoiseRestart::OnPlay) resetIn = b.op("transport.playing", {});
  else if (restart == NoiseRestart::OnNote) resetIn = noteOn;  // none out of a voice context: never
  const double lanes = restart == NoiseRestart::OnNote && !shared ? 0.0 : 1.0;  // On note: every note one pattern

  const P v = b.named(bld.atom("noise_step",
                               {rateIn, trigIn, beatIn, gridIn, resetIn, bld.param(kNoiseSmooth), bld.param(kNoiseColour),
                                bld.param(kNoiseDensity), bld.param(kNoiseSteps), bld.param(kNoiseSeed),
                                b.k(gauss ? 1.0 : 0.0), b.k(lanes)},
                               kStep),
                      "v");
  if (shared) bld.patch().setShared(v.node, true);  // shared(…): one instance for all voices
  // The Range: lerp(min, max, (v + 1) / 2) = v · (max − min)/2 + (max + min)/2.
  const P mn = bld.param(kNoiseMin), mx = bld.param(kNoiseMax);
  const P half = b.op("mul", {b.op("sub", {mx, mn}), b.k(0.5)});
  const P mid = b.op("mul", {b.op("add", {mx, mn}), b.k(0.5)});
  bld.output(0, b.op("muladd", {v, half, mid}));
  // The face's trace: v, into the instance's voice tap.
  b.named(bld.atom("voice_tap", {v, bld.slot(0)}, kTap), "tap");
  return Ok{};
}

std::vector<ResourceRequest> resources(std::span<const double>) { return {ResourceRequest{ResourceKind::VoiceTap, 0}}; }

Type makeType() {
  Type t;
  t.id = "noise";
  t.name = "Noise";
  t.version = 1;
  t.inputs = kInputs;
  t.params = kParams;
  t.settings = kSettings;
  t.outputs = kOutputs;
  t.chainInput = 0;  // the module's In: a clock
  t.resources = &resources;
  t.body = &body;
  return t;
}

}  // namespace

const Type& noiseType() noexcept {
  static const Type t = makeType();
  return t;
}

double noiseSyncBeats(uint32_t index) noexcept {
  if (index >= kNoiseSyncCount) index = kNoiseSyncCount - 1;
  constexpr double kFactor[3] = {2.0 / 3.0, 1.0, 1.5};
  return (1.0 / 16.0) * std::ldexp(1.0, static_cast<int>(index / 3)) * kFactor[index % 3];
}

}  // namespace pq::meta
