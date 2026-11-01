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
    {"x", "Phase in", InputDefault::VoicePhase},
    {"pos", "Position in", InputDefault::Zero},
}};
constexpr std::array<ParamSpec, 5> kParams = {{
    {"phase", "Phase", 0.0, 0.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, true, false},
    {"position", "Position", 0.0, 0.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, false},
    {"fold", "Fold", 0.0, 0.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, true},
    {"warp", "Warp", 0.0, -1.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, true},
    {"sync", "Sync", 0.0, 0.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, true},
}};
constexpr std::array<SettingSpec, 2> kSettings = {{
    {"mode", "Mode", 0.0, 0.0, 1.0, 2},
    {"warpMode", "Warp mode", 0.0, 0.0, 2.0, 3},
}};
constexpr std::array<OutputSpec, 1> kOutputs = {{{"out", graph::kWidthMono}}};

// Body node ids (stable: the wavetable_pos state key is the call path plus kRead).
enum : uint32_t { kAddPhase, kFrac, kAddPos, kLo, kHi, kClip, kRead };

Error wtError(std::string m) { return Error{ErrorCode::Unsupported, std::move(m), {}}; }

Status body(Builder& b) {
  if (b.setting(kWtMode) != 0.0) return wtError("Shape mode arrives with the live path (TMM.8); the Wavetable plays Phase mode");
  for (const uint32_t i : {kWtFold, kWtWarp, kWtSync})
    if (b.modulated(i))
      return wtError("'" + std::string(kParams[i].name) +
                     "' is baked into the Wavetable's table: modulating it (a wire, a lane, a macro) needs the live "
                     "path, which is not built yet");
  const auto ph = b.atom("frac", {b.atom("add", {b.input(kWtInX), b.param(kWtPhase)}, kAddPhase)}, kFrac);
  const auto pos = b.atom("clip", {b.atom("add", {b.param(kWtPosition), b.input(kWtInPos)}, kAddPos), b.konst(0.0, kLo),
                                   b.konst(1.0, kHi)},
                          kClip);
  b.output(0, b.atom("wavetable_pos", {ph, pos, b.slot(0)}, kRead));
  return Ok{};
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
  s.bake.fold = param(kWtFold);
  s.bake.warp = param(kWtWarp);
  s.bake.sync = param(kWtSync);
  const double wm = kWtWarpMode < src.settings.size() ? src.settings[kWtWarpMode] : kSettings[kWtWarpMode].def;
  const auto mode = static_cast<uint32_t>(std::clamp(std::isfinite(wm) ? std::round(wm) : 0.0, 0.0, 2.0));
  // The warp mode matters only with a warp: an unwarped table is the same in every mode (shared).
  s.bake.warpMode = s.bake.warp != 0.0f ? static_cast<dsp::WarpMode>(mode) : dsp::WarpMode::Bend;
  s.removeDc = refs.removeDc;
  s.normalise = refs.normalise;
  s.sampleRate = src.sampleRate;
  out.finish();
  uint64_t h = 1469598103934665603ull;
  for (const char c : std::string_view("wavetable/1")) h = mix(h, static_cast<unsigned char>(c));
  h = mix(h, src.data ? src.dataHash : dataHash(*data));
  h = mix(h, s.denseFrames);
  h = mix(h, std::bit_cast<uint32_t>(s.bake.fold + 0.0f));
  h = mix(h, std::bit_cast<uint32_t>(s.bake.warp + 0.0f));
  h = mix(h, std::bit_cast<uint32_t>(s.bake.sync + 0.0f));
  h = mix(h, static_cast<uint64_t>(s.bake.warpMode));
  h = mix(h, (s.removeDc ? 1u : 0u) | (s.normalise ? 2u : 0u));
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
  t.chainInstrument = true;
  t.resources = &resources;
  t.body = &body;
  t.defaultData = &wavetableDefaultData;
  t.dataProblem = &dataProblemHook;
  t.frameTable = &frameTable;
  t.liveBakeable = false;
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
