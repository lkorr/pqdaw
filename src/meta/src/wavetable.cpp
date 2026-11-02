// The Wavetable meta module (TMM.5): pq/meta/wavetable.h.
#include "pq/meta/wavetable.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <string_view>

#include "pq/meta/table_builder.h"

namespace pq::meta {

namespace {

constexpr std::array<InputSpec, 2> kInputs = {{
    // TMM.10-fix1: x unwired is phase 0; at the note source the chain gives it the note's phase (Reads::Phase).
    {"x", "Phase in", InputDefault::Zero, graph::Kind::Real, Reads::Phase},
    {"pos", "Position in", InputDefault::Zero},
}};
constexpr std::array<ParamSpec, 5> kParams = {{
    {"phase", "Phase", 0.0, 0.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, true, false},
    {"position", "Position", 0.0, 0.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, false},
    {"fold", "Fold", 0.0, 0.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, true},
    {"warp", "Warp", 0.0, -1.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, true},
    {"sync", "Sync", 0.0, 0.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, true},
}};
constexpr std::array<SettingSpec, 3> kSettings = {{
    {"mode", "Mode", 0.0, 0.0, 1.0, 2},
    {"warpMode", "Warp mode", 0.0, 0.0, 2.0, 3},
    {"os", "Oversampling", 0.0, 0.0, 4.0, 5},  // TMM.8: 0 auto, 1 … 4 = ×2 / ×4 / ×8 / ×16
}};
constexpr std::array<OutputSpec, 1> kOutputs = {{{"out", graph::kWidthMono}}};

// Body node ids (stable: the reading atom's state key is the call path plus its id).
enum : uint32_t {
  kAddPhase, kFrac, kAddPos, kLo, kHi, kClip, kRead, kLive, kZero, kModeK, kCorrK, kShape, kAddX,
  // TX.mod-all: Shape mode's live path (u = frac(clip((x + 1)/2, 0, 1)) inside the region, then wavetable_live).
  kShapeOneK, kShapeHalfK, kShapeAdd1, kShapeHalf, kShapeClip, kShapeFrac
};


// Which controls the live path applies (wavetable.h "Live path"): sync comes before warp, so a live warp takes sync
// along (a baked sync would be applied after the warp).
struct LiveSet {
  bool warp = false, fold = false, sync = false;
  bool any() const noexcept { return warp || fold || sync; }
};
LiveSet liveSet(std::span<const uint8_t> modulated) {
  auto mod = [&](uint32_t i) { return i < modulated.size() && modulated[i] != 0; };
  LiveSet s;
  s.warp = mod(kWtWarp);
  s.fold = mod(kWtFold);
  s.sync = mod(kWtSync) || s.warp;
  return s;
}

}  // namespace

uint8_t wavetableLiveControls(std::span<const double> /*settings*/, std::span<const uint8_t> modulated) noexcept {
  // TX.mod-all: Shape mode applies them live too (its curve is read through the same live path once one is modulated).
  const LiveSet l = liveSet(modulated);
  return static_cast<uint8_t>((l.fold ? kWtLiveFold : 0) | (l.warp ? kWtLiveWarp : 0) | (l.sync ? kWtLiveSync : 0));
}

uint32_t wavetableOversampling(std::span<const double> settings, std::span<const uint8_t> modulated) noexcept {
  const double v = kWtOversampling < settings.size() && std::isfinite(settings[kWtOversampling]) ? settings[kWtOversampling] : 0.0;
  const auto choice = static_cast<uint32_t>(std::clamp(std::round(v), 0.0, 4.0));
  if (choice > 0) return 1u << choice;
  const bool shape = kWtMode < settings.size() && settings[kWtMode] >= 0.5;
  const LiveSet live = liveSet(modulated);
  if (shape && !live.any()) return 2;
  // TMM.8-fix (measured, QUESTIONS TMM.8): ×16 whenever the read phase moves (a live warp or sync: their kinks and
  // steps at 4.4 kHz alias −89 … −94 dBFS at ×8 with an envelope at full depth, −105 at ×16); ×8 for fold alone.
  // TX.mod-all: Shape mode with a live control reads through the same live path, by the same rule.
  return live.sync ? 16u : 8u;
}

namespace {

Status body(Builder& b) {
  const bool shape = b.setting(kWtMode) >= 0.5;
  const std::span<const uint8_t> modulated = b.context().modulated;
  std::vector<double> settings(kSettings.size());
  for (size_t i = 0; i < settings.size(); ++i) settings[i] = b.setting(i);
  const uint32_t os = wavetableOversampling(settings, modulated);
  const auto lo = b.konst(0.0, kLo), hi = b.konst(1.0, kHi);
  const auto pos = b.atom("clip", {b.atom("add", {b.param(kWtPosition), b.input(kWtInPos)}, kAddPos), lo, hi}, kClip);
  // TX.mod-all ("a wire into a param makes it live at audio rate": one rule, pq/document/lower.h "Modulation wires"):
  // every modulated fold / warp / sync is applied per sample, in Phase and in Shape mode; the unmodulated ones stay
  // baked into the table.
  const LiveSet live = liveSet(modulated);
  // The live read of phase `ph`: the modulated controls wired, 0 for the others.
  auto liveRead = [&](Builder::PortRef ph) {
    const auto zero = b.konst(0.0, kZero);
    const double wm = live.warp ? b.setting(kWtWarpMode) : 0.0;
    return b.atom("wavetable_live",
                  {ph, pos, live.warp ? b.param(kWtWarp) : zero, live.fold ? b.param(kWtFold) : zero,
                   live.sync ? b.param(kWtSync) : zero, b.slot(0), b.konst(wm, kModeK),
                   b.konst(live.warp || live.sync ? 1.0 : 0.0, kCorrK)},
                  kLive);
  };
  if (shape) {
    // Shape mode: x (plus the phase slider as a bias) through the table as a curve, ADAA, in its own region.
    const auto x = b.atom("add", {b.input(kWtInX), b.param(kWtPhase)}, kAddX);
    const uint16_t region = b.region(os, {});
    if (!live.any()) {
      const auto y = b.atom("wavetable_shape", {x, pos, b.slot(0)}, kShape);
      b.inRegion(y, region);
      b.output(0, y);
      return Ok{};
    }
    // With a live control the curve is read through the live path: its read phase u = clip((x + 1)/2, 0, 1) is formed
    // inside the region (x is up-converted as a signal, so a fast input never looks like a phase wrap), and the live
    // read's band follows |Δu| as Phase mode's follows |Δφ| (the curve's k-th harmonic sits at k·|du/dt|); frac maps
    // the clamp's top u = 1 to T(0) = T(1), as wavetable_shape's curve does. Not ADAA: the clamp's corner (|x| > 1) is
    // not corrected (first-order ADAA only attenuated it).
    const auto a1 = b.atom("add", {x, b.konst(1.0, kShapeOneK)}, kShapeAdd1);
    const auto half = b.atom("mul", {a1, b.konst(0.5, kShapeHalfK)}, kShapeHalf);
    const auto cu = b.atom("clip", {half, lo, hi}, kShapeClip);
    const auto u = b.atom("frac", {cu}, kShapeFrac);
    const auto y = liveRead(u);
    for (const auto n : {a1, half, cu, u, y}) b.inRegion(n, region);
    b.output(0, y);
    return Ok{};
  }
  const auto ph = b.atom("frac", {b.atom("add", {b.input(kWtInX), b.param(kWtPhase)}, kAddPhase)}, kFrac);
  if (!live.any()) {
    b.output(0, b.atom("wavetable_pos", {ph, pos, b.slot(0)}, kRead));
    return Ok{};
  }
  // The live path: an oversample region around one wavetable_live read; the phase enters by interpolation.
  b.bind(ph, "ph");
  const auto y = liveRead(ph);
  b.inRegion(y, b.region(os, {"ph"}));
  b.output(0, y);
  return Ok{};
}

// TMM.8-fix: the table depends on which controls the live path applies and on Shape mode (Type::tableVariant).
// TX.mod-all: Shape mode with a live control reads the live path's table (the same variant as Phase mode's).
uint32_t tableVariant(std::span<const double> settings, std::span<const uint8_t> modulated) {
  const LiveSet l = liveSet(modulated);
  if (kWtMode < settings.size() && settings[kWtMode] >= 0.5 && !l.any()) return 8u;
  return (l.warp ? 1u : 0u) | (l.fold ? 2u : 0u) | (l.sync ? 4u : 0u);
}

std::vector<ResourceRequest> resources(std::span<const double>) { return {ResourceRequest{ResourceKind::FrameTable, 0}}; }

uint64_t mix(uint64_t h, uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i, v >>= 8) h = (h ^ (v & 0xFF)) * 1099511628211ull;
  return h;
}

constexpr std::string_view kSpectral = "spectral", kCrossfade = "crossfade";

// The keyframes / gaps / flags of a valid data value, without copying the cycles.
struct DataRefs {
  std::vector<const DataValue*> keys;  // records {pos, cycle}
  const DataValue* gaps = nullptr;
  uint32_t frames = kWtDefaultFrames;
  bool removeDc = false, normalise = false;
};

std::string readRefs(const DataValue& v, DataRefs& out) {
  if (v.kind != DataValue::Kind::Record) return "wavetable data must be a record";
  for (const auto& [name, _] : v.fields)
    if (name != "keyframes" && name != "gaps" && name != "frames" && name != "removeDc" && name != "normalise")
      return "wavetable data has an unknown field '" + name + "'";
  const DataValue* keys = v.field("keyframes");
  if (keys == nullptr || keys->kind != DataValue::Kind::List) return "wavetable data needs a 'keyframes' list";
  if (keys->items.empty() || keys->items.size() > dsp::kMaxKeyframes) return "a wavetable has 1 to 256 keyframes";
  out.keys.clear();
  double prev = 0.0;
  for (size_t i = 0; i < keys->items.size(); ++i) {
    const DataValue& k = keys->items[i];
    const std::string at = "keyframe " + std::to_string(i);
    if (k.kind != DataValue::Kind::Record || k.fields.size() != 2) return at + " must be {pos, cycle}";
    const DataValue* pos = k.field("pos");
    const DataValue* cyc = k.field("cycle");
    if (pos == nullptr || pos->kind != DataValue::Kind::Number || cyc == nullptr || cyc->kind != DataValue::Kind::Floats)
      return at + " must be {pos: number, cycle: floats}";
    if (!(pos->number >= 0.0 && pos->number <= 1.0)) return at + ": pos must be in [0, 1]";
    if (pos->number < prev) return at + ": keyframe positions must not decrease";
    prev = pos->number;
    if (cyc->floats.size() != dsp::kKeyframeCycle) return at + ": a cycle has 2048 samples";
    out.keys.push_back(&k);
  }
  out.gaps = v.field("gaps");
  if (out.gaps != nullptr) {
    if (out.gaps->kind != DataValue::Kind::List || out.gaps->items.size() != keys->items.size() - 1)
      return "'gaps' must list one mode per gap between keyframes";
    for (const DataValue& g : out.gaps->items)
      if (g.kind != DataValue::Kind::String || (g.text != kSpectral && g.text != kCrossfade))
        return "a gap is \"spectral\" or \"crossfade\"";
  }
  auto flag = [&](const char* name, bool& to) -> std::string {
    const DataValue* f = v.field(name);
    if (f == nullptr) return {};
    if (f->kind != DataValue::Kind::Number || (f->number != 0.0 && f->number != 1.0))
      return std::string("'") + name + "' is 0 or 1";
    to = f->number != 0.0;
    return {};
  };
  if (std::string e = flag("removeDc", out.removeDc); !e.empty()) return e;
  if (std::string e = flag("normalise", out.normalise); !e.empty()) return e;
  if (const DataValue* f = v.field("frames")) {
    if (f->kind != DataValue::Kind::Number || !(f->number >= 1.0 && f->number <= dsp::kMaxKeyframes) ||
        f->number != std::floor(f->number))
      return "'frames' is a whole number from 1 to 256";
    out.frames = static_cast<uint32_t>(f->number);
  }
  return {};
}

std::string dataProblemHook(const DataValue& v) { return wavetableDataProblem(v); }

Status frameTable(const TableSource& src, uint32_t index, TableJob& out) {
  if (index != 0) return Error{ErrorCode::InvalidArgument, "the Wavetable reads one table", {}};
  DataPtr data = src.data ? src.data : wavetableDefaultData();
  DataRefs refs;
  if (std::string e = readRefs(*data, refs); !e.empty()) return Error{ErrorCode::InvalidArgument, std::move(e), {}};
  out.data = data;
  out.keys.clear();
  out.gaps.clear();
  for (const DataValue* k : refs.keys)
    out.keys.push_back(dsp::Keyframe{static_cast<float>(k->field("pos")->number), k->field("cycle")->floats});
  if (refs.gaps != nullptr)
    for (const DataValue& g : refs.gaps->items)
      out.gaps.push_back(g.text == kCrossfade ? dsp::GapMode::Crossfade : dsp::GapMode::Spectral);
  auto param = [&](uint32_t i) {
    const ParamSpec& p = kParams[i];
    const double v = i < src.params.size() && std::isfinite(src.params[i]) ? src.params[i] : p.def;
    return static_cast<float>(std::clamp(v, p.min, p.max));
  };
  dsp::FrameTableSpec& s = out.spec;
  s = dsp::FrameTableSpec{};
  // One keyframe: every dense frame would be the same, so build one (the readers' single-frame path).
  s.denseFrames = out.keys.size() == 1 ? 1u : refs.frames;
  // TMM.8: Phase mode bakes only the controls its live path does not apply; Shape mode bakes all three into the curve
  // and builds the antiderivative arrays its ADAA reads. TX.mod-all: Shape mode with a live control reads through the
  // live path, so its table is the live path's (the live band, the unmodulated controls baked, no antiderivative).
  const bool shape = kWtMode < src.settings.size() && src.settings[kWtMode] >= 0.5;
  const LiveSet live = liveSet(src.modulated);
  const bool adaa = shape && !live.any();
  s.bake.fold = live.fold ? 0.0f : param(kWtFold);
  s.bake.warp = live.warp ? 0.0f : param(kWtWarp);
  s.bake.sync = live.sync ? 0.0f : param(kWtSync);
  s.antiderivative = adaa;
  s.liveBand = !adaa;  // TMM.8-fix: baked with the live band limit, so baked <-> live switching is seamless
  const double wm = kWtWarpMode < src.settings.size() ? src.settings[kWtWarpMode] : kSettings[kWtWarpMode].def;
  const auto mode = static_cast<uint32_t>(std::clamp(std::isfinite(wm) ? std::round(wm) : 0.0, 0.0, 2.0));
  // The warp mode matters only with a warp: an unwarped table is the same in every mode (shared).
  s.bake.warpMode = s.bake.warp != 0.0f ? static_cast<dsp::WarpMode>(mode) : dsp::WarpMode::Bend;
  s.removeDc = refs.removeDc;
  s.normalise = refs.normalise;
  s.sampleRate = src.sampleRate;
  out.focus = param(kWtPosition);  // TMM.16: where the voices read (a focused preview rebuilds the frames there first)
  out.finish();
  uint64_t h = 1469598103934665603ull;
  for (const char c : std::string_view("wavetable/1")) h = mix(h, static_cast<unsigned char>(c));
  h = mix(h, src.data ? src.dataHash : dataHash(*data));
  h = mix(h, s.denseFrames);
  h = mix(h, std::bit_cast<uint32_t>(s.bake.fold + 0.0f));
  h = mix(h, std::bit_cast<uint32_t>(s.bake.warp + 0.0f));
  h = mix(h, std::bit_cast<uint32_t>(s.bake.sync + 0.0f));
  h = mix(h, static_cast<uint64_t>(s.bake.warpMode));
  h = mix(h, (s.removeDc ? 1u : 0u) | (s.normalise ? 2u : 0u) | (s.antiderivative ? 4u : 0u));
  h = mix(h, std::bit_cast<uint64_t>(s.sampleRate));
  out.key = h;
  return Ok{};
}

Type makeType() {
  Type t;
  t.id = "wavetable";
  t.name = "Wavetable";
  t.version = 1;
  t.inputs = kInputs;
  t.params = kParams;
  t.settings = kSettings;
  t.outputs = kOutputs;
  // TMM.10-fix1: the chain feeds x — the note's phase at the note source, a Phase accumulator's phase after one, audio
  // after an audio stage (TMM.8: Shape mode shapes it).
  t.chainInput = static_cast<int32_t>(kWtInX);
  t.resources = &resources;
  t.body = &body;
  t.defaultData = &wavetableDefaultData;
  t.dataProblem = &dataProblemHook;
  t.frameTable = &frameTable;
  t.liveBakeable = true;  // TMM.8: the live path
  // TX.mod-all: no bakedOnly hook: every mode applies a modulated fold / warp / sync live (Type::bakedOnly stays for types
  // whose live path depends on a setting).
  t.tableVariant = &tableVariant;
  return t;
}

}  // namespace

const Type& wavetableType() noexcept {
  static const Type t = makeType();
  return t;
}

DataPtr wavetableData(const WavetableData& d) {
  std::vector<DataValue> keys;
  keys.reserve(d.keyframes.size());
  for (const WavetableKeyframe& k : d.keyframes)
    keys.push_back(DataValue::record({{"pos", DataValue::num(k.position)}, {"cycle", DataValue::floatArray(k.cycle)}}));
  std::vector<std::pair<std::string, DataValue>> f;
  f.emplace_back("keyframes", DataValue::list(std::move(keys)));
  if (d.keyframes.size() > 1) {
    std::vector<DataValue> gaps;
    for (size_t i = 0; i + 1 < d.keyframes.size(); ++i)
      gaps.push_back(DataValue::str(std::string(i < d.gaps.size() && d.gaps[i] == dsp::GapMode::Crossfade ? kCrossfade
                                                                                                          : kSpectral)));
    f.emplace_back("gaps", DataValue::list(std::move(gaps)));
  }
  if (d.frames != kWtDefaultFrames) f.emplace_back("frames", DataValue::num(d.frames));
  if (d.removeDc) f.emplace_back("removeDc", DataValue::num(1));
  if (d.normalise) f.emplace_back("normalise", DataValue::num(1));
  return std::make_shared<const DataValue>(DataValue::record(std::move(f)));
}

Result<WavetableData> parseWavetableData(const DataValue& v) {
  DataRefs refs;
  if (std::string e = readRefs(v, refs); !e.empty()) return Error{ErrorCode::InvalidArgument, std::move(e), {}};
  WavetableData d;
  for (const DataValue* k : refs.keys)
    d.keyframes.push_back({static_cast<float>(k->field("pos")->number), k->field("cycle")->floats});
  d.gaps.assign(d.keyframes.size() - 1, dsp::GapMode::Spectral);
  if (refs.gaps != nullptr)
    for (size_t i = 0; i < refs.gaps->items.size(); ++i)
      if (refs.gaps->items[i].text == kCrossfade) d.gaps[i] = dsp::GapMode::Crossfade;
  d.frames = refs.frames;
  d.removeDc = refs.removeDc;
  d.normalise = refs.normalise;
  return d;
}

std::string wavetableDataProblem(const DataValue& v) {
  DataRefs refs;
  return readRefs(v, refs);
}

DataPtr wavetableDefaultData() {
  static const DataPtr d = [] {
    WavetableData w;
    const AlignedBuffer<float> sine = dsp::basicCycle(dsp::BasicCycle::Sine);
    w.keyframes.push_back({0.0f, std::vector<float>(sine.span().begin(), sine.span().end())});
    return wavetableData(w);
  }();
  return d;
}

}  // namespace pq::meta
