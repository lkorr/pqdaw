// The block library (T4.1; GU.11): pq/document/devices.h.
#include "pq/document/devices.h"
#include "pq/graph/param_curve.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "pq/document/lower.h"
#include "pq/graph/parse.h"
#include "pq/graph/patch_file.h"

namespace pq::doc {

namespace {

Error invalidDevice(const std::string& patch, const std::string& why) {
  return Error{ErrorCode::InvalidArgument, "device patch '" + patch + "': " + why, {}};
}

const graph::Attr* findAttr(const graph::ParamDecl& d, std::string_view name) {
  for (const graph::Attr& a : d.attrs)
    if (a.name == name) return &a;
  return nullptr;
}

DeviceParamInfo paramInfo(const graph::Patch& p, graph::NodeIndex n) {
  const graph::ParamDecl& d = p.paramDecl(n);
  DeviceParamInfo info;
  info.name = p.nodeName(n);
  info.label = info.name;
  info.kind = d.kind == graph::Kind::Bool ? graph::Kind::Bool : graph::Kind::Real;
  info.defaultValue = d.defaultValue.value();
  info.unit = d.defaultValue.unit;
  if (info.kind == graph::Kind::Bool) {
    info.hasRange = true;
    info.min = 0.0;
    info.max = 1.0;
    info.steps = 2;
  } else if (d.hasRange) {
    info.hasRange = true;
    info.min = d.rangeMin.value();
    info.max = d.rangeMax.value();
    if (info.unit == graph::Unit::None) info.unit = d.rangeMin.unit;
  }
  if (const graph::Attr* a = findAttr(d, "label"); a && !a->args.empty() && a->args[0].value.type == graph::AttrValue::Type::String)
    info.label = a->args[0].value.text;
  if (const graph::Attr* a = findAttr(d, "unit"); a && !a->args.empty()) {
    graph::Unit u = graph::Unit::None;
    if (graph::unitFromSuffix(a->args[0].value.text, u)) info.unit = u;
  }
  if (const graph::Attr* a = findAttr(d, "curve"); a && !a->args.empty())
    info.exponential = a->args[0].value.text == "exp";
  if (const graph::Attr* a = findAttr(d, "steps"); a && !a->args.empty() &&
                                                   a->args[0].value.type == graph::AttrValue::Type::Number) {
    const double v = a->args[0].value.number.value();
    if (v >= 2.0 && v <= 4096.0) info.steps = static_cast<uint32_t>(v);
  }
  info.curve = graph::declaredCurve(d);  // GU.1
  return info;
}

}  // namespace

const DeviceParamInfo* DeviceInfo::param(std::string_view n) const noexcept {
  for (const DeviceParamInfo& p : params)
    if (p.name == n) return &p;
  return nullptr;
}

const DeviceSettingInfo* DeviceInfo::setting(std::string_view n) const noexcept {
  for (const DeviceSettingInfo& s : settings)
    if (s.name == n) return &s;
  return nullptr;
}

Result<DeviceInfo> describeDevice(const graph::Patch& p, std::string displayName) {
  DeviceInfo info;
  info.patch = p.name();
  info.name = std::move(displayName);
  const graph::Adjacency adj = p.adjacency();
  bool haveAudioIn = false, haveOut = false, voiceOut = false;
  for (graph::NodeIndex n = 0; n < p.nodeCount(); ++n) {
    const graph::Node& nd = p.node(n);
    switch (nd.kind) {
      case graph::NodeKind::Param: info.params.push_back(paramInfo(p, n)); break;
      case graph::NodeKind::Input: {
        const graph::PortType t = p.outputType(n, 0);
        if (t.rate == graph::Rate::Const) {
          DeviceSettingInfo s;
          s.name = p.nodeName(n);
          const graph::WireIndex w = adj.driver[p.flatInput(n, 0)];
          if (w != graph::kNone) {
            const graph::NodeIndex src = p.wire(w).from.node;
            if (p.node(src).kind == graph::NodeKind::Const) {
              s.defaultValue = p.constant(src).value();
              s.hasDefault = true;
            }
          }
          info.settings.push_back(std::move(s));
        } else if (!haveAudioIn) {
          if (t.rate != graph::Rate::Audio && t.rate != graph::Rate::Infer)
            return invalidDevice(info.patch, "its first input '" + p.nodeName(n) + "' is not an audio input");
          if (t.poly == graph::Poly::Voice) return invalidDevice(info.patch, "its audio input is per voice");
          haveAudioIn = true;
          info.inWidth = t.width;
        }
        break;
      }
      case graph::NodeKind::Output:
        if (!haveOut) {
          const graph::PortType t = p.inputType(n, 0);
          haveOut = true;
          voiceOut = t.poly == graph::Poly::Voice;
          info.outWidth = t.width;
        }
        break;
      default: break;
    }
  }
  if (!haveOut) return invalidDevice(info.patch, "it has no output");
  if (haveAudioIn) {
    if (voiceOut) return invalidDevice(info.patch, "an audio effect's output must be global");
    info.kind = DeviceKind::AudioEffect;
  } else {
    if (!voiceOut) return invalidDevice(info.patch, "an instrument's first output must be per voice");
    info.kind = DeviceKind::Instrument;
  }
  return info;
}

double deviceParamPosition(const Device& d, const DeviceParamInfo& p) noexcept {  // GU.1
  const DeviceValue* v = d.param(p.name);
  return v != nullptr ? v->value : dsp::curvePosition(p.curve, p.defaultValue, 0.0);
}

const dsp::Curve& deviceParamCurve(const Device& d, const DeviceParamInfo& p) noexcept {
  const dsp::Curve* c = d.curve(p.name);
  return c != nullptr ? *c : p.curve;
}

double deviceParamValue(const Device& d, const DeviceParamInfo& p) noexcept {
  return dsp::evalCurve(deviceParamCurve(d, p), deviceParamPosition(d, p));
}

double deviceSettingValue(const Device& d, const DeviceSettingInfo& s) noexcept {
  const DeviceValue* v = d.setting(s.name);
  return v != nullptr ? v->value : s.defaultValue;
}

std::string deviceParamNameOf(const Project& p, const DeviceLibrary* lib, const ParamRef& r) {
  if (r.kind != ParamKind::DeviceParam) return {};
  const Device* d = findDevice(p, DeviceId{r.owner}).device;
  if (d == nullptr) return {};
  if (lib != nullptr) {
    if (const DeviceInfo* info = lib->infoOf(*d))
      for (const DeviceParamInfo& pi : info->params)
        if (deviceParamHash(pi.name) == r.target) return pi.name;
  }
  for (const DeviceValue& v : d->params)
    if (deviceParamHash(v.name) == r.target) return v.name;
  return {};
}

DeviceLibrary::DeviceLibrary() : prelude_(meta::prelude()) {}
DeviceLibrary::~DeviceLibrary() = default;

std::optional<DeviceInfo> metaDeviceInfo(const meta::Type& t, DeviceKind kind) {
  // TMM.10-fix1: every type is a device of either kind (what reaches it decides its form, not its type).
  DeviceInfo info;
  info.patch = std::string(kMetaDevicePrefix) + std::string(t.id);
  info.name = std::string(t.name);
  info.kind = kind;
  info.outWidth = t.outputs.empty() ? 0 : t.outputs.front().width;
  for (const meta::ParamSpec& s : t.params) {
    DeviceParamInfo p;
    p.name = std::string(s.name);
    p.label = s.label.empty() ? p.name : std::string(s.label);
    p.defaultValue = s.def;
    p.hasRange = true;
    p.min = s.min;
    p.max = s.max;
    p.unit = s.unit;
    p.exponential = s.curve == dsp::CurvePreset::Octave;
    p.curve = meta::paramCurve(s);  // GU.1
    info.params.push_back(std::move(p));
  }
  for (const meta::SettingSpec& s : t.settings) info.settings.push_back(DeviceSettingInfo{std::string(s.name), s.def, true});
  return info;
}

Result<std::unique_ptr<DeviceLibrary>> loadDeviceLibrary(std::string_view stdlibDirUtf8) {
  (void)stdlibDirUtf8;
  std::unique_ptr<DeviceLibrary> lib(new DeviceLibrary());
  // TMM.3: the meta-module prelude (default expansions `meta_<type>`), callable like any library patch.
  for (const meta::Prelude::Entry& e : lib->prelude_->entries) lib->library_.add(e.nested);
  for (const graph::SubcircuitRef& s : lib->prelude_->signatures) lib->signatures_.push_back(s);
  // TX.groups / IG.1-2: the fixed group patches (every new MIDI track holds an instrument group), so a lowering needs no
  // library of its own for them.
  lib->library_.add(groupPatch());
  lib->library_.add(groupVoicePatch());
  lib->library_.add(groupNotesPatch());
  // GU.2: the value block and every atom block (built once per process; a lowering then needs no library of its own).
  lib->library_.add(valuePatch());
  for (const graph::AtomSignature& s : graph::AtomRegistry::builtin().all())
    if (atomBlockAllowed(s)) {
      if (const graph::Patch* ap = atomBlockPatch(s.name)) lib->library_.add(*ap);
      if (const graph::Patch* vp = voicelessAtomPatch(s.name)) lib->library_.add(*vp);
    }
  for (const meta::Prelude::Entry& e : lib->prelude_->entries)
    for (const DeviceKind k : {DeviceKind::Instrument, DeviceKind::AudioEffect}) {
      std::optional<DeviceInfo> info = metaDeviceInfo(*e.type, k);
      lib->metaInfos_.push_back(info ? std::move(*info) : DeviceInfo{});
    }
  return lib;
}

// TX.groups: the one patch every group plays (model.h groupPatchText), parsed once, and its description.
namespace {
struct GroupPatch {
  graph::PatchFile file;
  const graph::Patch* patch = nullptr;
  DeviceInfo info;
};
const GroupPatch& groupPatchData() {
  static const GroupPatch g = [] {
    GroupPatch x;
    Result<graph::PatchFile> f = graph::parse(groupPatchText(), "group");
    if (!f.ok()) return x;  // a broken text: groupPatch() then fails loudly in the tests (never in practice)
    x.file = std::move(f.value());
    x.patch = x.file.find(kGroupDevice);
    if (x.patch != nullptr)
      if (Result<DeviceInfo> i = describeDevice(*x.patch, "Group"); i.ok()) x.info = std::move(i.value());
    return x;
  }();
  return g;
}
}  // namespace

const graph::Patch& groupPatch() {
  const GroupPatch& g = groupPatchData();
  if (g.patch == nullptr) std::abort();  // the fixed text always parses (tests/document test_groups)
  return *g.patch;
}

const DeviceInfo& groupDeviceInfo() {
  (void)groupPatch();
  return groupPatchData().info;
}

namespace {
// TX.groups-fix1: the voice and note-source variants (model.h groupVoicePatchText / groupNotesPatchText), parsed once.
graph::PatchFile parseGroupVariant(std::string_view text) {
  Result<graph::PatchFile> f = graph::parse(text, "group");
  return f.ok() ? std::move(f.value()) : graph::PatchFile{};
}
const graph::Patch& groupVariant(const graph::PatchFile& f, std::string_view name) {
  const graph::Patch* p = f.find(name);
  if (p == nullptr) std::abort();  // the fixed texts always parse (tests/app/test_groups.cpp)
  return *p;
}
}  // namespace

const graph::Patch& groupVoicePatch() {
  static const graph::PatchFile f = parseGroupVariant(groupVoicePatchText());
  static const graph::Patch& p = groupVariant(f, "GroupVoice");
  return p;
}

const graph::Patch& groupNotesPatch() {
  static const graph::PatchFile f = parseGroupVariant(groupNotesPatchText());
  static const graph::Patch& p = groupVariant(f, "GroupNotes");
  return p;
}

// ---- GU.2: atom blocks and value blocks -------------------------------------------------------------------------

bool atomBlockAllowed(const graph::AtomSignature& s) noexcept {
  using namespace graph::atom_flags;
  if (s.has(kInternal) || s.has(kSideEffect)) return false;
  // GU.9: the voice terminals (voice.noteHz, voice.velocity …) are blocks: inside an instrument group they read the
  // note of the voice they run for; anywhere else they give 0 (voicelessAtomPatch). `pack` joins channels (a chain
  // mixer's stereo gain), `channel` picks one (GU.10).
  if (s.category == graph::AtomCategory::Voice) return s.inputs.empty() && s.fixedOutputs() == 1;
  if (s.has(kNeedsVoice)) return false;
  if (s.name == "pack") return true;
  if (s.name == "channel") return true;  // GU.10: one channel of a wide signal (Utility's L and R; its index a setting)
  switch (s.category) {
    case graph::AtomCategory::Io:
    case graph::AtomCategory::Sample:
    case graph::AtomCategory::Structure: return false;
    default: break;
  }
  if (s.fixedOutputs() == 0 && !(s.has(kVariadicOutputs) && s.minRepeat > 0)) return false;  // nothing to give on
  return s.name.find("wavetable") == std::string_view::npos && s.name != "shaper" && s.name != "tuning_hz" &&
         s.name != "curve";
}

namespace {

struct AtomBlock {
  graph::Patch patch{""};
  DeviceInfo info;
  std::optional<graph::Patch> voiceless;  // GU.9: a voice terminal's stand-in outside a voice (its outputs = 0)
};

std::unique_ptr<AtomBlock> buildAtomBlock(std::string_view atomName) {
  const std::optional<graph::AtomId> id = graph::AtomRegistry::builtin().find(atomName);
  if (!id) return nullptr;
  const graph::AtomSignature& s = graph::atomSignature(*id);
  if (!atomBlockAllowed(s)) return nullptr;
  std::string name = "Atom_";
  for (const char ch : s.name) name.push_back(ch == '.' ? '_' : ch);
  auto b = std::make_unique<AtomBlock>();
  b->patch = graph::Patch(name);
  graph::Patch& p = b->patch;
  const uint32_t fixedIn = s.fixedInputs();
  const bool variadicIn = s.has(graph::atom_flags::kVariadicInputs) && s.inputs.size() > fixedIn;
  const uint16_t repeat = variadicIn || s.has(graph::atom_flags::kVariadicOutputs) ? s.minRepeat : 0;
  const graph::NodeIndex atom = p.addAtom(*id, repeat);
  std::vector<std::string> inNames;
  auto addIn = [&](const graph::PortSpec& spec, std::string nm, uint16_t pin) {
    graph::PortType t;
    // Real or Bool; an atom of any kind (select, z1, mux …) takes and gives reals as a block (a port of a patch has one
    // declared kind).
    t.kind = spec.type.kind == graph::Kind::Bool ? graph::Kind::Bool : graph::Kind::Real;
    const bool setting = spec.type.rate == graph::Rate::Const;
    if (setting) t.rate = graph::Rate::Const;
    const graph::NodeIndex in = p.addInput(nm, t);
    // WI.2: a bool input nothing drives is false (an unconnected input reads a real 0 otherwise, which a bool refuses: a
    // logic block off every path, or one a cut wire left unfed, failed its whole track's compile).
    if (spec.optional || setting || t.kind == graph::Kind::Bool) {
      const graph::NodeIndex c = p.addConst(t.kind == graph::Kind::Bool ? graph::Constant::boolean(spec.defaultValue != 0.0f)
                                                                       : graph::Constant::real(spec.defaultValue));
      p.connect(graph::PortRef{c, 0}, graph::PortRef{in, 0});
      if (setting) b->info.settings.push_back(DeviceSettingInfo{nm, static_cast<double>(spec.defaultValue), true});
    }
    p.connect(graph::PortRef{in, 0}, graph::PortRef{atom, pin});
    inNames.push_back(std::move(nm));
  };
  for (uint32_t i = 0; i < fixedIn; ++i) addIn(s.inputs[i], std::string(s.inputs[i].name), static_cast<uint16_t>(i));
  if (variadicIn)
    for (uint16_t k = 0; k < s.minRepeat; ++k)
      addIn(s.inputs[fixedIn], std::string(s.inputs[fixedIn].name) + std::to_string(k), static_cast<uint16_t>(fixedIn + k));
  const uint32_t outs = p.node(atom).numOutputs;
  if (outs == 0) return nullptr;
  for (uint32_t o = 0; o < outs; ++o) {
    std::string nm = std::string(p.outputName(atom, o));
    if (nm.empty()) nm = "out";
    if (o >= s.fixedOutputs()) nm += std::to_string(o - s.fixedOutputs());  // a variadic output: numbered
    if (std::find(inNames.begin(), inNames.end(), nm) != inNames.end()) nm += "_out";
    graph::PortType t;
    t.kind = p.outputType(atom, o).kind == graph::Kind::Bool ? graph::Kind::Bool : graph::Kind::Real;
    const graph::NodeIndex out = p.addOutput(nm, t);
    p.connect(graph::PortRef{atom, static_cast<uint16_t>(o)}, graph::PortRef{out, 0});
  }
  std::vector<graph::NodeId> ids;
  for (graph::NodeIndex n = 0; n < p.nodeCount(); ++n) ids.push_back(graph::NodeId{n + 1});
  if (!p.setNodeIds(ids) || !p.validate()) return nullptr;
  if (s.category == graph::AtomCategory::Voice) {  // GU.9: the same ports, every output 0
    graph::Patch v(name + "__voiceless");
    for (graph::NodeIndex n = 0; n < p.nodeCount(); ++n) {
      if (p.node(n).kind != graph::NodeKind::Output) continue;
      const graph::NodeIndex out = v.addOutput(std::string(p.nodeName(n)), graph::portType(graph::Kind::Real));
      v.connect(graph::PortRef{v.addConst(graph::Constant::real(0.0)), 0}, graph::PortRef{out, 0});
    }
    std::vector<graph::NodeId> vids;
    for (graph::NodeIndex n = 0; n < v.nodeCount(); ++n) vids.push_back(graph::NodeId{n + 1});
    if (v.setNodeIds(vids) && v.validate()) b->voiceless = std::move(v);
  }
  b->info.patch = std::string(kAtomDevicePrefix) + std::string(s.name);
  b->info.name = std::string(s.name);
  b->info.kind = DeviceKind::AudioEffect;
  return b;
}

struct AtomBlocks {
  std::mutex m;
  std::unordered_map<std::string, std::unique_ptr<AtomBlock>> made;  // by atom name (null: not a block)
};
AtomBlocks& atomBlocks() {
  static AtomBlocks a;
  return a;
}
const AtomBlock* atomBlock(std::string_view atomName) {
  AtomBlocks& a = atomBlocks();
  std::lock_guard lock(a.m);
  auto it = a.made.find(std::string(atomName));
  if (it == a.made.end()) it = a.made.emplace(std::string(atomName), buildAtomBlock(atomName)).first;
  return it->second.get();  // (unique_ptr: the address stays put when the map rehashes)
}

struct ValueBlock {
  graph::PatchFile file;
  const graph::Patch* patch = nullptr;
  DeviceInfo info;
};
const ValueBlock& valueBlock() {
  static const ValueBlock v = [] {
    ValueBlock x;
    Result<graph::PatchFile> f = graph::parse(valuePatchText(), "value");
    if (!f.ok()) return x;
    x.file = std::move(f.value());
    x.patch = x.file.find(kValueDevice);
    if (x.patch == nullptr) return x;
    x.info.patch = std::string(kValueDevice);
    x.info.name = "Value";
    x.info.kind = DeviceKind::AudioEffect;
    for (graph::NodeIndex n = 0; n < x.patch->nodeCount(); ++n)
      if (x.patch->node(n).kind == graph::NodeKind::Param) x.info.params.push_back(paramInfo(*x.patch, n));
    return x;
  }();
  return v;
}

}  // namespace

const graph::Patch* atomBlockPatch(std::string_view atomName) {
  const AtomBlock* b = atomBlock(atomName);
  return b != nullptr ? &b->patch : nullptr;
}
const graph::Patch* voicelessAtomPatch(std::string_view atomName) {
  const AtomBlock* b = atomBlock(atomName);
  return b != nullptr && b->voiceless ? &*b->voiceless : nullptr;
}
const DeviceInfo* atomBlockInfo(std::string_view atomName) {
  const AtomBlock* b = atomBlock(atomName);
  return b != nullptr ? &b->info : nullptr;
}
const graph::Patch& valuePatch() {
  const ValueBlock& v = valueBlock();
  if (v.patch == nullptr) std::abort();  // the fixed text always parses (tests/document test_blocks)
  return *v.patch;
}
const DeviceInfo& valueDeviceInfo() {
  (void)valuePatch();
  return valueBlock().info;
}
const graph::Patch* builtinBlockPatch(const Device& d) {
  if (isGroupDevice(d)) return &groupPatch();
  if (isAtomDevice(d)) return atomBlockPatch(atomDeviceName(d));
  if (isValueDevice(d)) return &valuePatch();
  return nullptr;
}

const graph::Patch* DeviceLibrary::patchOf(const Device& d) const {
  if (isGroupDevice(d)) return &groupPatch();  // TX.groups
  if (isAtomDevice(d)) return atomBlockPatch(atomDeviceName(d));  // GU.2
  if (isValueDevice(d)) return &valuePatch();                     // GU.2
  if (isMetaDevice(d)) {  // TMM.3
    const meta::Prelude::Entry* e = prelude_->find(metaDeviceType(d));
    if (e == nullptr) return nullptr;
    // TMM.10-fix1: every form has the same params; the note-source form for the device at the instrument position.
    if (d.kind == DeviceKind::Instrument && e->notes) return &*e->notes;
    return e->chain();
  }
  return nullptr;  // GU.11: not a block
}

const DeviceInfo* DeviceLibrary::infoOf(const Device& d) const {
  if (isGroupDevice(d)) return &groupDeviceInfo();  // TX.groups
  if (isAtomDevice(d)) return atomBlockInfo(atomDeviceName(d));  // GU.2
  if (isValueDevice(d)) return &valueDeviceInfo();               // GU.2
  if (isMetaDevice(d)) {  // TMM.3
    for (size_t i = 0; i < prelude_->entries.size(); ++i)
      if (prelude_->entries[i].type->id == metaDeviceType(d)) {
        const DeviceInfo& info = metaInfos_[2 * i + (d.kind == DeviceKind::Instrument ? 0 : 1)];
        return info.patch.empty() ? nullptr : &info;
      }
    return nullptr;
  }
  return nullptr;  // GU.11: not a block
}

}  // namespace pq::doc
