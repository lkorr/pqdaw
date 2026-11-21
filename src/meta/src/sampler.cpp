// The Sampler engine (SM.2): pq/meta/sampler.h.
#include "pq/meta/sampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace pq::meta {

namespace {

constexpr std::array<InputSpec, kSmpInputs> kInputs = {{
    {"hz", "Frequency in", InputDefault::Unwired, graph::Kind::Real, Reads::Frequency},
    {"gate", "Gate", InputDefault::VoiceGate, graph::Kind::Bool, Reads::Gate},
    {"key", "Key in", InputDefault::Zero, graph::Kind::Real, Reads::Signal},
    {"position", "Position in", InputDefault::Unwired, graph::Kind::Real, Reads::Signal},
}};
constexpr std::array<ParamSpec, kSmpParams> kParams = {{
    {"start", "Start", 0.0, 0.0, 1.0},
    {"end", "End", 1.0, 0.0, 1.0},
    {"loopStart", "Loop start", 0.0, 0.0, 1.0},
    {"loopLength", "Loop length", 1.0, 0.0, 1.0},
    {"loopFade", "Loop fade", 0.0, 0.0, 1.0, graph::Unit::s},
    {"transpose", "Transpose", 0.0, -48.0, 48.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false, false, 97},
    {"detune", "Detune", 0.0, -50.0, 50.0},
    {"gain", "Gain", 0.0, -36.0, 12.0},
    {"root", "Root", kSamplerDefaultRoot, 8.0, 20000.0, graph::Unit::Hz, dsp::CurvePreset::Octave},
    {"sampleBpm", "Sample BPM", 120.0, 20.0, 999.0},
    {"fadeIn", "Fade in", 0.0, 0.0, 2.0, graph::Unit::s},
    {"fadeOut", "Fade out", 0.0, 0.0, 2.0, graph::Unit::s},
    {"grain", "Grain size", 0.03, 0.001, 0.5, graph::Unit::s},
    {"flux", "Flux", 0.0, 0.0, 100.0},
    {"envelope", "Transient envelope", 100.0, 0.0, 100.0},
}};
constexpr std::array<SettingSpec, kSmpSettings> kSettings = {{
    {"mode", "Mode", 0.0, 0.0, 2.0, 3},        // Classic, One-Shot, Slice
    {"warp", "Warp", 0.0, 0.0, 6.0, 7},        // Off, Re-Pitch, Beats, Tones, Texture, Complex, Complex Pro
    {"loop", "Loop", 1.0, 0.0, 2.0, 3},        // Off, Forward, Ping-pong
    {"trigger", "Trigger", 0.0, 0.0, 1.0, 2},  // Trigger, Gate
    {"beatsLoop", "Transient loop", 1.0, 0.0, 2.0, 3},  // SM.4 Beats: Off, Forward, Back-Forward
}};
constexpr std::array<OutputSpec, 2> kOutputs = {{{"out", graph::kWidthStereo}, {"gateOut", graph::kWidthMono}}};

// Body node ids: the stateful nodes fixed (their state keys survive every re-expansion, a setting change included);
// the rest in build order from kSeq.
enum : uint32_t { kRead, kRise, kSlice, kScrub, kTap, kWarp, kSeq = 16 };

class Body {
public:
  using P = Builder::PortRef;
  explicit Body(Builder& b) : b_(b) {}
  P op(std::string_view atom, std::initializer_list<P> ins, uint16_t repeat = 0) {
    return b_.atom(atom, ins, next_++, repeat);
  }
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
  const double r = std::round(v);
  return r <= 0.0 ? 0u : r >= n - 1.0 ? n - 1 : static_cast<uint32_t>(r);
}

Status body(Builder& bld) {
  using P = Builder::PortRef;
  Body b(bld);
  const auto mode = static_cast<SamplerMode>(choice(bld.setting(kSmpMode), 3));
  const auto warp = static_cast<SamplerWarp>(choice(bld.setting(kSmpWarp), 7));
  const bool repitch = warp == SamplerWarp::RePitch;
  // SM.4: Beats / Tones / Texture play through sampler_warp (Complex / Complex Pro: Tones until SM.5); Slice keeps its
  // own path (its slices play at their recorded pitch, unwarped).
  const bool stretch = mode != SamplerMode::Slice && warp >= SamplerWarp::Beats;
  const uint32_t loop = choice(bld.setting(kSmpLoop), 3);
  const bool gateMode = choice(bld.setting(kSmpTrigger), 2) == 1;
  const P slot = b.named(bld.slot(0), "sample");  // (read by several atoms: named, as the text form needs)
  const P unwired = b.named(b.k(kUnwiredInput / 2), "unwired");
  const P gate = bld.input(kSmpGate);
  // Rate (1: the file's own speed): hz / root (unwired hz: the root), × the transpose / detune ratio; Slice keeps the
  // ratio only (its slices play at their recorded pitch); Re-Pitch × the song's tempo over the sample's.
  const P root = bld.param(kSmpRoot);
  const P ratio = b.op("cents_to_hz", {b.op("add", {b.op("mul", {bld.param(kSmpTranspose), b.k(100.0)}),
                                                    bld.param(kSmpDetune)}),
                                       b.k(1.0)});
  P rate = ratio;
  const P hzIn = bld.input(kSmpHz);
  // (Every node is made only where it is read: an anonymous dead node does not survive the text form.)
  if (mode != SamplerMode::Slice) {
    const P hz = b.op("select", {b.op("gt", {hzIn, unwired}), hzIn, root});
    rate = b.op("mul", {b.op("div", {hz, root}), ratio});
  } else if (bld.context().voice) {
    // Slice: a wired frequency's motion away from the note's pitch (a glide, a bend) moves the rate.
    const P noteHz = b.named(b.op("voice.noteHz", {}), "note_hz");
    const P bend = b.op("select", {b.op("and", {b.op("gt", {hzIn, unwired}), b.op("gt", {noteHz, b.k(0.0)})}),
                                   b.op("div", {hzIn, b.op("max", {noteHz, b.k(1e-6)})}), b.k(1.0)});
    rate = b.op("mul", {ratio, bend});
  }
  if (repitch && !stretch) rate = b.op("mul", {rate, b.op("div", {b.op("transport.bpm", {}), bld.param(kSmpSampleBpm)})});
  const P trig = bld.atom("rise", {gate}, kRise);
  // The region: Classic start → end with its loop; One-Shot start → end; Slice the key's slice.
  P start = bld.param(kSmpStart), end = bld.param(kSmpEnd);
  P loopStart, loopEnd, fade, pingpong, fadeIn, fadeOut;
  const bool looping = mode == SamplerMode::Classic && loop != 0;
  P rd;
  if (stretch) {
    // SM.4: the voice's content follows the song's tempo from its trigger; the pitch is the rate (hz / root ×
    // transpose). Loop: Classic's (ping-pong loops forward: warp_voice.h loops the content's beats).
    const double wm = warp == SamplerWarp::Beats ? 0.0 : warp == SamplerWarp::Texture ? 2.0 : 1.0;
    if (looping) {
      loopStart = bld.param(kSmpLoopStart);
      loopEnd = b.op("min", {b.op("add", {loopStart, bld.param(kSmpLoopLength)}), b.k(1.0)});
    } else {
      loopStart = b.k(0.0), loopEnd = b.k(0.0);
    }
    rd = b.named(bld.atom("sampler_warp",
                          {slot, rate, trig, start, end, loopStart, loopEnd, b.op("transport.bpm", {}),
                           bld.param(kSmpSampleBpm), b.k(wm), bld.param(kSmpGrain), bld.param(kSmpFlux),
                           b.k(static_cast<double>(choice(bld.setting(kSmpBeatsLoop), 3))), bld.param(kSmpEnvelope),
                           b.op("sr", {})},
                          kWarp),
                 "rd");
  } else {
    if (looping) {
      loopStart = bld.param(kSmpLoopStart);
      loopEnd = b.op("min", {b.op("add", {loopStart, bld.param(kSmpLoopLength)}), b.k(1.0)});
      fade = bld.param(kSmpLoopFade);
      pingpong = b.k(loop == 2 ? 1.0 : 0.0);
    } else {
      loopStart = b.k(0.0), loopEnd = b.k(0.0), fade = b.k(0.0), pingpong = b.k(0.0);
    }
    if (mode == SamplerMode::Classic) {
      fadeIn = b.k(0.0), fadeOut = b.k(0.0);
    } else {
      fadeIn = bld.param(kSmpFadeIn);
      fadeOut = bld.param(kSmpFadeOut);
      if (mode == SamplerMode::Slice) {
        const P s = b.named(bld.atom("sample_slice", {slot, bld.input(kSmpKey), b.k(1.0)}, kSlice), "slice");
        start = s;
        end = P{s.node, 1};
      }
    }
    rd = b.named(bld.atom("sample_read_cubic",
                          {slot, rate, trig, start, loopStart, loopEnd, fade, pingpong, end, fadeIn, fadeOut, b.k(1.0)},
                          kRead),
                 "rd");
  }
  P l = rd, r = P{rd.node, 2};
  const P playing = P{rd.node, 1};
  // A wired position reads the sample there directly (an unwired one folds this away).
  const P posIn = bld.input(kSmpPosition);
  const P scrubbing = b.named(b.op("gt", {posIn, unwired}), "scrubbing");
  const P sc = b.named(bld.atom("sample_scrub", {slot, posIn}, kScrub), "scrub");
  l = b.op("select", {scrubbing, sc, l});
  r = b.op("select", {scrubbing, P{sc.node, 1}, r});
  const P g = b.named(b.op("db_to_gain", {bld.param(kSmpGain)}), "gain_lin");
  bld.output(0, b.op("pack", {b.op("mul", {l, g}), b.op("mul", {r, g})}, 2));
  // Gate out (the envelope after it) and the voice's end.
  P gateOut = gate;
  P done;
  if (mode == SamplerMode::Classic) {
    done = b.op("not", {b.op("and", {gate, playing})});
  } else {
    gateOut = gateMode ? b.op("and", {gate, playing}) : playing;
    gateOut = b.named(gateOut, "gate_out");
    done = b.op("not", {gateOut});
  }
  // SM.8: outside a voice (a Sampler no notes reach, e.g. dropped on an audio track) a wired Position is the gate: the
  // Envelope behind gateOut opens while something scrubs the sample. (Folded away when Position is unwired.)
  if (!bld.context().voice) gateOut = b.op("or", {gateOut, scrubbing});
  bld.output(1, b.op("gate_to_real", {gateOut}));  // (a type's outputs are Real: 1 / 0)
  // SM.10: the face's playheads — per voice where it reads now (0 … 1 of the sample: the playhead, or a wired Position),
  // into the instance's voice tap (request 1; one relaxed load per block while no face reads it).
  const P head = b.op("select", {scrubbing, posIn, P{rd.node, 3}});
  b.named(bld.atom("voice_tap", {head, bld.slot(1)}, kTap), "tap");
  bld.done(done);
  return Ok{};
}

std::vector<ResourceRequest> resources(std::span<const double>) {
  return {ResourceRequest{ResourceKind::Sample, 0}, ResourceRequest{ResourceKind::VoiceTap, 1}};
}

DataPtr defaultData() { return samplerData(0); }

std::string dataProblem(const DataValue& v) {
  if (v.kind != DataValue::Kind::Record) return "sampler data must be a record";
  const DataValue* s = v.field("sample");
  if (s == nullptr || s->kind != DataValue::Kind::Number || !(s->number >= 0.0) || s->number > 4294967295.0 ||
      s->number != std::floor(s->number))
    return "sampler data: `sample` must be a sample id (a whole number ≥ 0)";
  for (const char* f : {"slices", "markers"})
    if (const DataValue* x = v.field(f); x != nullptr && x->kind != DataValue::Kind::Floats)
      return std::string("sampler data: `") + f + "` must be floats";
  for (const char* f : {"sliceBy", "sensitivity"})
    if (const DataValue* x = v.field(f); x != nullptr && x->kind != DataValue::Kind::Number)
      return std::string("sampler data: `") + f + "` must be a number";
  if (const DataValue* x = v.field("rootText"); x != nullptr && x->kind != DataValue::Kind::String)
    return "sampler data: `rootText` must be text";
  return {};
}

std::vector<uint32_t> sampleRefs(const DataValue& data) {
  const uint32_t s = samplerSample(&data);
  return s != 0 ? std::vector<uint32_t>{s} : std::vector<uint32_t>{};
}

SampleBinding sampleBinding(const DataValue* data, uint32_t index) {
  if (index != 0) return {};
  SampleBinding b{samplerSample(data), samplerSlices(data), {}};
  if (data != nullptr)
    if (const DataValue* m = data->field("markers"); m != nullptr && m->kind == DataValue::Kind::Floats) b.markers = m->floats;
  return b;
}

Type makeType() {
  Type t;
  t.id = "sampler";
  t.name = "Sampler";
  t.version = 1;
  t.inputs = kInputs;
  t.params = kParams;
  t.settings = kSettings;
  t.outputs = kOutputs;
  t.chainInput = kSmpHz;  // a chain feeds its frequency (at the note source: the note's Hz)
  t.resources = &resources;
  t.body = &body;
  t.defaultData = &defaultData;
  t.dataProblem = &dataProblem;
  t.voiceEnd = true;
  t.sampleRefs = &sampleRefs;
  t.sampleBinding = &sampleBinding;
  return t;
}

}  // namespace

const Type& samplerType() noexcept {
  static const Type t = makeType();
  return t;
}

uint32_t samplerSample(const DataValue* data) noexcept {
  if (data == nullptr) return 0;
  const DataValue* s = data->field("sample");
  if (s == nullptr || s->kind != DataValue::Kind::Number || !(s->number >= 0.0) || s->number > 4294967295.0) return 0;
  return static_cast<uint32_t>(s->number);
}

std::vector<float> samplerSlices(const DataValue* data) {
  if (data == nullptr) return {};
  const DataValue* s = data->field("slices");
  return s != nullptr && s->kind == DataValue::Kind::Floats ? s->floats : std::vector<float>{};
}

DataPtr samplerData(uint32_t sample, std::vector<float> slices) {
  return std::make_shared<const DataValue>(DataValue::record({{"sample", DataValue::num(static_cast<double>(sample))},
                                                              {"slices", DataValue::floatArray(std::move(slices))}}));
}

std::vector<float> samplerMarkers(const DataValue* data) {
  const DataValue* m = data != nullptr ? data->field("markers") : nullptr;
  return m != nullptr && m->kind == DataValue::Kind::Floats ? m->floats : std::vector<float>{};
}

std::string samplerRootText(const DataValue* data) {
  const DataValue* t = data != nullptr ? data->field("rootText") : nullptr;
  return t != nullptr && t->kind == DataValue::Kind::String ? t->text : std::string();
}

DataPtr samplerWith(const DataValue* data, std::string_view name, DataValue v) {
  DataValue out = data != nullptr && data->kind == DataValue::Kind::Record ? *data : *samplerData(0);
  for (auto& [k, x] : out.fields)
    if (k == name) {
      x = std::move(v);
      return std::make_shared<const DataValue>(std::move(out));
    }
  out.fields.emplace_back(std::string(name), std::move(v));
  std::sort(out.fields.begin(), out.fields.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return std::make_shared<const DataValue>(std::move(out));
}

}  // namespace pq::meta
