// The node menu's catalog (TX.node-menu): pq/views/circuit/node_catalog.h.

#include "pq/views/circuit/node_catalog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_set>

#include "pq/document/devices.h"
#include "pq/document/model.h"
#include "pq/document/modules.h"
#include "pq/graph/atoms.h"
#include "pq/graph/edit.h"
#include "pq/graph/parse.h"
#include "pq/graph/patch_file.h"
#include "pq/library/index.h"
#include "pq/library/search.h"
#include "pq/meta/registry.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/library_load.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/view_context.h"

namespace pq::views::circuit {

namespace {

std::string fold(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = library::foldChar(c);
  return out;
}

std::string number(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}

// A normalised param value (doc::DeviceParamInfo / meta ParamSpec units) as the panels read it.
std::string valueText(double v, graph::Unit unit, bool boolean = false, uint32_t steps = 0) {
  if (boolean) return v >= 0.5 ? "on" : "off";
  if (steps > 0) return number(std::round(v));
  switch (unit) {
    case graph::Unit::Hz:
    case graph::Unit::kHz: return std::fabs(v) >= 1000.0 ? number(v / 1000.0) + " kHz" : number(v) + " Hz";
    case graph::Unit::ms:
    case graph::Unit::s: return std::fabs(v) < 1.0 ? number(v * 1000.0) + " ms" : number(v) + " s";
    case graph::Unit::dB: return v <= 0.0 ? std::string("-inf dB") : number(20.0 * std::log10(v)) + " dB";
    case graph::Unit::percent: return number(v * 100.0) + " %";
    case graph::Unit::cents: return number(v) + " c";
    case graph::Unit::st: return number(v / 100.0) + " st";
    case graph::Unit::smp: return number(v) + " smp";
    case graph::Unit::None: break;
  }
  return number(v);
}

// One or two sentences per meta type and stock device (the preview pane). Keyed by id / patch; an unknown one gets
// a generic line from its ports.
std::string_view metaDescription(std::string_view id) noexcept {
  if (id == "wavetable") return "Reads a table of single-cycle frames by phase: position morphs between keyframes, warp bends the cycle. A band-limited oscillator or, fed audio, a waveshaper.";
  if (id == "envelope") return "An attack / decay / sustain / release envelope per voice with bendable segments; as a VCA on its input it ends the voice when released and quiet.";
  if (id == "spectrum") return "A spectrum analyser of what passes through it (the signal is unchanged).";
  if (id == "oscilloscope") return "An oscilloscope locked to the pitch of what passes through it (the signal is unchanged).";
  if (id == "spectrogram") return "A scrolling spectrogram of what passes through it (the signal is unchanged).";
  return {};
}
std::string_view deviceDescription(std::string_view patch) noexcept {
  if (patch == "BasicSynth") return "The default synth: one oscillator per voice through a filter and an amp envelope.";
  if (patch == "operator") return "Four-operator FM synthesis with exact frequency ratios (Live's Operator).";
  if (patch == "utility") return "Gain, width, mid/side, balance, mono and bass mono, phase and channel tools.";
  if (patch == "saturator") return "A waveshaping distortion with drive, curve shapes and a dry/wet mix.";
  if (patch == "eq_eight") return "An eight-band parametric equaliser with a live spectrum and its measured response.";
  if (patch == "auto_filter") return "A resonant filter with an envelope follower, an LFO and a sidechain input.";
  if (patch == "lfo_device") return "A low-frequency oscillator whose `lfo` jack modulates any parameter; the audio passes through.";
  if (patch == doc::kAudioEffectRack) return "Parallel chains of audio effects with macros, chain volumes and zones.";
  if (patch == doc::kInstrumentRack) return "Parallel chains of instruments and effects with key and velocity zones and macros.";
  return {};
}

// Words a search for an operator's symbol or name finds it by.
std::string_view atomAliases(std::string_view name) noexcept {
  static const std::pair<std::string_view, std::string_view> kAliases[] = {
      {"add", "+ plus sum"}, {"sub", "- minus subtract difference"}, {"mul", "* times multiply product"},
      {"div", "/ divide ratio"}, {"mod", "% modulo wrap"}, {"neg", "- negate invert"}, {"lt", "< less"},
      {"le", "<= less equal"}, {"gt", "> greater"}, {"ge", ">= greater equal"}, {"eq", "== equal"},
      {"ne", "!= not equal"}, {"and", "&& logic"}, {"or", "|| logic"}, {"xor", "^^ logic"}, {"not", "! logic invert"},
      {"tanh", "saturate soft clip"}, {"clip", "clamp limit hard clip"}, {"z1", "delay one sample unit"},
      {"phasor", "ramp saw oscillator"}, {"sine_hz", "oscillator sine"}, {"noise", "white random"},
      {"select", "? ternary switch"}, {"lerp", "mix crossfade interpolate"}, {"samplehold", "s&h sample hold"},
  };
  for (const auto& [n, a] : kAliases)
    if (n == name) return a;
  return {};
}

bool visualiser(std::string_view metaId) noexcept { return metaId == "spectrum" || metaId == "oscilloscope" || metaId == "spectrogram"; }

// Atoms that make a free device on their own at the project level: none that needs a voice, the transport's
// sample position, sample data, an engine I/O or is internal; no structure atoms (channels and voice reducers belong
// inside a circuit).
// GU.2: the atoms that are atom blocks (doc::atomBlockAllowed: tables read data a block cannot hold — wavetables,
// shapers: the meta modules are their form —, voice sources need a voice context, I/O and structure atoms the lowering's).
bool projectAtom(const graph::AtomSignature& s) noexcept { return doc::atomBlockAllowed(s); }

void finish(NodeEntry& e) {
  e.foldedName = fold(e.name);
  e.foldedAliases = fold(e.aliases);
  e.foldedDesc = fold(e.description);
  e.mask = library::charMask(e.foldedName) | library::charMask(e.foldedAliases) | library::charMask(e.foldedDesc);
}

void addAtoms(NodeCatalog& c) {
  std::vector<NodeEntry> atoms;
  for (const graph::AtomSignature& s : graph::AtomRegistry::builtin().all()) {
    if (s.has(graph::atom_flags::kInternal)) continue;
    if (c.scope == NodeCatalog::Scope::Project && !projectAtom(s)) continue;
    NodeEntry e;
    e.item = NodeItem::Atom;
    e.category = NodeCategory::Atoms;
    e.name = std::string(s.name);
    e.id = e.name;
    e.key = "atom:" + e.id;
    e.aliases = std::string(atomAliases(s.name)) + " " + std::string(graph::toText(s.category));
    e.description = std::string(s.summary);
    e.chip = "ATOM";
    const uint32_t fixedIn = s.fixedInputs();
    for (uint32_t i = 0; i < s.inputs.size(); ++i) {
      const graph::PortSpec& p = s.inputs[i];
      if (i >= fixedIn) {  // a variadic port: its minimum count, numbered as the language numbers them
        for (uint16_t k = 0; k < s.minRepeat; ++k) e.inputs.push_back(std::string(p.name) + std::to_string(k));
        continue;
      }
      if (p.optional) e.params.push_back({std::string(p.name), number(p.defaultValue)});  // an input with a default
      else e.inputs.push_back(std::string(p.name));
    }
    for (uint32_t i = 0; i < s.outputs.size(); ++i) {
      if (i >= s.fixedOutputs()) {
        for (uint16_t k = 0; k < s.minRepeat; ++k) e.outputs.push_back(std::string(s.outputs[i].name) + std::to_string(k));
        continue;
      }
      e.outputs.push_back(std::string(s.outputs[i].name));
    }
    e.hasIn = !e.inputs.empty();
    e.hasOut = !e.outputs.empty();
    finish(e);
    atoms.push_back(std::move(e));
  }
  // GU.2: the value block — one control as a signal (its curve gives the range) — beside the atoms, at the project level.
  if (c.scope == NodeCatalog::Scope::Project) {
    NodeEntry e;
    e.item = NodeItem::Device;
    e.category = NodeCategory::Atoms;
    e.id = std::string(doc::kValueDevice);
    e.key = "device:" + e.id;
    e.name = "Value";
    e.aliases = "value control knob constant param";
    e.description = "One control as a signal: its knob's value (through its curve) is the output.";
    e.chip = "VALUE";
    e.params.push_back({"Value", "0"});
    e.outputs = {std::string(doc::kValueOutput)};
    e.hasOut = true;
    finish(e);
    atoms.push_back(std::move(e));
  }
  for (NodeEntry& e : atoms) c.entries.push_back(std::move(e));
}

void addMetas(NodeCatalog& c, std::vector<NodeEntry>& visualisers) {
  for (const meta::Type* t : meta::types()) {
    if (!hasFace(*t)) continue;
    NodeEntry e;
    e.item = NodeItem::Meta;
    e.id = std::string(t->id);
    e.key = "meta:" + e.id;
    e.name = std::string(t->name);
    e.category = visualiser(t->id) ? NodeCategory::Visualisers : NodeCategory::MetaModules;
    e.chip = visualiser(t->id) ? "VISUALISER" : "META MODULE";
    e.aliases = "meta_" + e.id + " meta module";
    e.description = std::string(metaDescription(t->id));
    for (const meta::InputSpec& in : t->inputs) e.inputs.push_back(std::string(in.label.empty() ? in.name : in.label));
    for (const meta::OutputSpec& o : t->outputs) e.outputs.push_back(std::string(o.name));
    for (const meta::ParamSpec& p : t->params)
      e.params.push_back({std::string(p.label.empty() ? p.name : p.label), valueText(p.def, p.unit)});
    e.hasIn = t->chainInput >= 0;
    e.hasOut = !t->outputs.empty();
    if (e.description.empty())
      e.description = "A meta module with " + std::to_string(t->inputs.size()) + " inputs and " + std::to_string(t->params.size()) + " params.";
    finish(e);
    (e.category == NodeCategory::Visualisers ? visualisers : c.entries).push_back(std::move(e));
  }
}

// GU.4: the built-in modules (group presets of atom and value blocks with a panel: doc::moduleInfos), beside the meta
// modules (the project only: a module is a group, which lives in a track's circuit).
void addModules(NodeCatalog& c) {
  for (const doc::ModuleInfo& m : doc::moduleInfos()) {
    NodeEntry e;
    e.item = NodeItem::Module;
    e.id = std::string(m.key);
    e.key = std::string(doc::kModuleItemPrefix) + e.id;
    e.name = std::string(m.name);
    e.category = NodeCategory::MetaModules;
    e.chip = "MODULE";
    e.aliases = e.id + " module group preset meta";
    e.description = std::string(m.description);
    e.inputs = {"in"};
    e.outputs = {"out"};
    e.hasIn = e.hasOut = true;
    const doc::BlockSet set = doc::moduleBlocks(m.kind);
    for (const doc::DevicePtr& d : set.devices)
      if (doc::isValueDevice(*d) && d->curve(doc::kValueParam) != nullptr && d->name != doc::kPaReference) {
        const dsp::Curve& cv = *d->curve(doc::kValueParam);
        const doc::DeviceValue* v = d->param(doc::kValueParam);
        e.params.push_back({d->name, number(dsp::evalCurve(cv, v != nullptr ? v->value : 0.0))});
      }
    finish(e);
    c.entries.push_back(std::move(e));
  }
}

void addDevices(const doc::DeviceLibrary* lib, std::vector<NodeEntry>& instruments, std::vector<NodeEntry>& effects) {
  if (lib == nullptr) return;
  for (const doc::DeviceInfo& d : lib->catalog().devices()) {
    NodeEntry e;
    e.item = NodeItem::Device;
    e.id = d.patch;
    e.key = "device:" + d.patch;
    e.name = d.name;
    e.instrument = d.kind == doc::DeviceKind::Instrument;
    e.category = e.instrument ? NodeCategory::Instruments : NodeCategory::AudioEffects;
    e.chip = e.instrument ? "INSTRUMENT" : "AUDIO EFFECT";
    e.description = std::string(deviceDescription(d.patch));
    e.aliases = d.patch;
    const graph::Patch* callee = lib->library().find(d.patch);
    if (callee != nullptr) {
      const graph::SubcircuitRef sig = graph::subcircuitSignature(*callee);
      std::unordered_set<std::string_view> params;
      for (const doc::DeviceParamInfo& p : d.params) params.insert(p.name);
      for (const graph::SubcircuitPort& sp : sig.inputs)
        if (sp.type.rate != graph::Rate::Const && params.count(sp.name) == 0 && !sp.name.ends_with("_add")) e.inputs.push_back(sp.name);
      for (const graph::SubcircuitPort& sp : sig.outputs) e.outputs.push_back(sp.name);
      const MainPorts m = mainPortsOf(sig, callee, e.instrument);
      e.hasIn = !m.in.empty();
      e.hasOut = !m.out.empty();
    }
    for (const doc::DeviceParamInfo& p : d.params)
      e.params.push_back({p.label.empty() ? p.name : p.label, valueText(p.defaultValue, p.unit, p.kind == graph::Kind::Bool, p.steps)});
    if (e.description.empty()) e.description = e.instrument ? "An instrument." : "An audio effect.";
    finish(e);
    (e.instrument ? instruments : effects).push_back(std::move(e));
  }
  for (const bool inst : {false, true}) {
    NodeEntry e;
    e.item = NodeItem::Rack;
    e.id = std::string(inst ? doc::kInstrumentRack : doc::kAudioEffectRack);
    e.key = "rack:" + e.id;
    e.name = inst ? "Instrument Rack" : "Audio Effect Rack";
    e.instrument = inst;
    e.category = inst ? NodeCategory::Instruments : NodeCategory::AudioEffects;
    e.chip = "RACK";
    e.aliases = "rack chains macros";
    e.description = std::string(deviceDescription(e.id));
    e.inputs = inst ? std::vector<std::string>{} : std::vector<std::string>{"in"};
    e.outputs = {"out"};
    e.hasIn = !inst;
    e.hasOut = true;
    for (uint32_t m = 1; m <= 8; ++m) e.params.push_back({"Macro " + std::to_string(m), "0"});
    finish(e);
    (inst ? instruments : effects).push_back(std::move(e));
  }
}

void addLibrary(NodeCatalog& c, const library::Index& index) {
  std::vector<NodeEntry> files;
  for (uint32_t i = 0; i < index.entryCount(); ++i) {
    const library::ItemKind k = index.kind(i);
    if (k != library::ItemKind::Preset && k != library::ItemKind::Device) continue;
    const uint32_t place = index.placeOf(i);
    if (place >= index.places.size() || index.places[place].kind != library::PlaceKind::UserLibrary) continue;
    const library::Category cat = index.category(i);
    NodeEntry e;
    e.item = NodeItem::LibraryFile;
    e.category = NodeCategory::UserLibrary;
    e.id = index.path(i);
    e.key = "file:" + e.id;
    e.name = std::string(index.stem(i));
    e.instrument = cat == library::Category::Instruments;
    const bool rack = cat == library::Category::Racks;
    e.chip = k == library::ItemKind::Device ? "USER DEVICE" : rack ? "RACK PRESET" : "PRESET";
    e.aliases = std::string(library::itemKindName(k)) + (e.instrument ? " instrument" : rack ? " rack" : " effect");
    e.description = std::string(k == library::ItemKind::Device ? "A device saved to the User Library" : "A preset saved to the User Library") +
                    (e.instrument ? " (an instrument)." : cat == library::Category::MetaModules ? " (a meta module)." : ".");
    e.hasIn = !e.instrument;
    e.hasOut = true;
    if (!e.instrument) e.inputs = {"in"};
    e.outputs = {"out"};
    finish(e);
    files.push_back(std::move(e));
  }
  std::stable_sort(files.begin(), files.end(), [](const NodeEntry& a, const NodeEntry& b) { return a.foldedName < b.foldedName; });
  for (NodeEntry& e : files) c.entries.push_back(std::move(e));
}

std::shared_ptr<const NodeCatalog> build(NodeCatalog::Scope scope, const doc::DeviceLibrary* lib, const library::Index* index) {
  auto c = std::make_shared<NodeCatalog>();
  c->scope = scope;
  std::vector<NodeEntry> visualisers, instruments, effects;
  addAtoms(*c);
  addMetas(*c, visualisers);
  if (scope == NodeCatalog::Scope::Project) addModules(*c);
  if (scope == NodeCatalog::Scope::Project) {
    addDevices(lib, instruments, effects);
    auto byName = [](const NodeEntry& a, const NodeEntry& b) { return a.foldedName < b.foldedName; };
    std::stable_sort(instruments.begin(), instruments.end(), byName);
    std::stable_sort(effects.begin(), effects.end(), byName);
    for (NodeEntry& e : instruments) c->entries.push_back(std::move(e));
    for (NodeEntry& e : effects) c->entries.push_back(std::move(e));
  }
  for (NodeEntry& e : visualisers) c->entries.push_back(std::move(e));
  if (scope == NodeCatalog::Scope::Project && index != nullptr) addLibrary(*c, *index);
  return c;
}

std::mutex gRecentMutex;
std::vector<std::string> gRecent;

}  // namespace

std::string_view tabName(NodeTab t) noexcept {
  switch (t) {
    case NodeTab::All: return "All";
    case NodeTab::Atoms: return "Atoms";
    case NodeTab::MetaModules: return "Meta modules";
    case NodeTab::Instruments: return "Instruments";
    case NodeTab::AudioEffects: return "Audio effects";
    case NodeTab::MidiEffects: return "MIDI effects";
    case NodeTab::Visualisers: return "Visualisers";
    case NodeTab::UserLibrary: return "User library";
    case NodeTab::Recent: return "Recent";
    case NodeTab::Count: break;
  }
  return {};
}

std::string_view categoryName(NodeCategory c) noexcept { return tabName(tabOf(c)); }

const NodeEntry* NodeCatalog::find(std::string_view key) const noexcept {
  for (const NodeEntry& e : entries)
    if (e.key == key) return &e;
  return nullptr;
}

namespace {
std::shared_ptr<const NodeCatalog> cachedCatalog(NodeCatalog::Scope scope, const doc::DeviceLibrary* lib,
                                                const std::shared_ptr<const library::Index>& index) {
  // One cached catalog per scope: rebuilt when the library or the index snapshot it was built from changes.
  struct Cached {
    const doc::DeviceLibrary* lib = nullptr;
    std::weak_ptr<const library::Index> index;
    const library::Index* indexRaw = nullptr;
    std::shared_ptr<const NodeCatalog> catalog;
  };
  static Cached cache[2];
  Cached& c = cache[scope == NodeCatalog::Scope::Project ? 0 : 1];
  const bool sameIndex = scope == NodeCatalog::Scope::Circuit || (c.indexRaw == index.get() && (index == nullptr || !c.index.expired()));
  if (c.catalog && c.lib == lib && sameIndex) return c.catalog;
  c.catalog = build(scope, scope == NodeCatalog::Scope::Project ? lib : nullptr, index.get());
  c.lib = lib;
  c.index = index;
  c.indexRaw = index.get();
  return c.catalog;
}

}  // namespace

std::shared_ptr<const NodeCatalog> nodeCatalog(NodeCatalog::Scope scope, const doc::DeviceLibrary* lib) {
  return cachedCatalog(scope, lib, nullptr);
}

std::shared_ptr<const NodeCatalog> nodeCatalog(NodeCatalog::Scope scope, ViewContext& ctx) {
  std::shared_ptr<const library::Index> index;
  if (scope == NodeCatalog::Scope::Project) index = ctx.library().index();
  return cachedCatalog(scope, ctx.deviceLibrary(), index);
}

bool compatible(const NodeEntry& e, NodeFilter f) noexcept {
  switch (f) {
    case NodeFilter::None: return true;
    case NodeFilter::WithInput: return e.hasIn;
    case NodeFilter::WithOutput: return e.hasOut;
    case NodeFilter::WithBoth: return e.hasIn && e.hasOut;
  }
  return true;
}

std::vector<NodeHit> searchNodes(const NodeCatalog& c, std::string_view query, NodeTab tab, NodeFilter filter,
                                 std::span<const std::string> recent) {
  std::vector<NodeHit> out;
  // Terms, folded.
  std::vector<std::string> terms;
  {
    std::string cur;
    for (const char ch : query) {
      if (ch == ' ') {
        if (!cur.empty()) terms.push_back(std::move(cur));
        cur.clear();
      } else {
        cur.push_back(library::foldChar(ch));
      }
    }
    if (!cur.empty()) terms.push_back(std::move(cur));
  }
  std::vector<uint64_t> termMasks;
  for (const std::string& t : terms) termMasks.push_back(library::charMask(t));
  std::vector<int32_t> scratch;
  auto score = [&](const NodeEntry& e) -> int32_t {
    int32_t total = 0;
    for (size_t k = 0; k < terms.size(); ++k) {
      if ((termMasks[k] & ~e.mask) != 0) return library::kNoMatch;
      int32_t best = library::fuzzyScore(terms[k], e.foldedName, scratch);
      if (const int32_t a = library::fuzzyScore(terms[k], e.foldedAliases, scratch); a != library::kNoMatch)
        best = std::max(best, a - 24);
      // Descriptions: the term as written (a substring of three letters or more; a subsequence would match almost any
      // sentence), well below a name's match; at a word start a little better.
      if (terms[k].size() >= 3)
        if (const size_t at = e.foldedDesc.find(terms[k]); at != std::string::npos) {
          const bool word = at == 0 || e.foldedDesc[at - 1] == ' ' || e.foldedDesc[at - 1] == '(';
          best = std::max(best, library::kScoreMatch * static_cast<int32_t>(terms[k].size()) + (word ? 8 : 0) - 64);
        }
      if (best == library::kNoMatch) return library::kNoMatch;
      total += best;
    }
    return total;
  };
  auto consider = [&](uint32_t i) {
    const NodeEntry& e = c.entries[i];
    const int32_t s = terms.empty() ? 0 : score(e);
    if (s == library::kNoMatch) return;
    out.push_back({i, s, compatible(e, filter)});
  };
  if (tab == NodeTab::Recent) {
    for (const std::string& key : recent)
      for (uint32_t i = 0; i < c.entries.size(); ++i)
        if (c.entries[i].key == key) consider(i);
    std::stable_sort(out.begin(), out.end(), [&](const NodeHit& a, const NodeHit& b) {
      if (a.compatible != b.compatible) return a.compatible;
      return !terms.empty() && a.score > b.score;
    });
    return out;
  }
  for (uint32_t i = 0; i < c.entries.size(); ++i)
    if (tab == NodeTab::All || tabOf(c.entries[i].category) == tab) consider(i);
  // Grouped by category; with a query the group holding the best match comes first (then the next best group …).
  int32_t best[2][static_cast<size_t>(NodeCategory::Count)];
  for (auto& row : best)
    for (int32_t& b : row) b = library::kNoMatch;
  for (const NodeHit& h : out) {
    int32_t& b = best[h.compatible ? 1 : 0][static_cast<size_t>(c.entries[h.entry].category)];
    b = std::max(b, h.score);
  }
  std::stable_sort(out.begin(), out.end(), [&](const NodeHit& a, const NodeHit& b) {
    if (a.compatible != b.compatible) return a.compatible;
    const NodeCategory ca = c.entries[a.entry].category, cb = c.entries[b.entry].category;
    if (ca != cb) {
      if (!terms.empty()) {
        const int32_t ba = best[a.compatible ? 1 : 0][static_cast<size_t>(ca)], bb = best[b.compatible ? 1 : 0][static_cast<size_t>(cb)];
        if (ba != bb) return ba > bb;
      }
      return ca < cb;
    }
    if (terms.empty()) return false;  // catalog order
    if (a.score != b.score) return a.score > b.score;
    return c.entries[a.entry].name.size() < c.entries[b.entry].name.size();
  });
  return out;
}

void noteRecentNode(std::string_view key) {
  std::lock_guard<std::mutex> lock(gRecentMutex);
  std::erase(gRecent, key);
  gRecent.insert(gRecent.begin(), std::string(key));
  if (gRecent.size() > kRecentNodes) gRecent.resize(kRecentNodes);
}

std::vector<std::string> recentNodes() {
  std::lock_guard<std::mutex> lock(gRecentMutex);
  return gRecent;
}

void clearRecentNodes() {
  std::lock_guard<std::mutex> lock(gRecentMutex);
  gRecent.clear();
}

// ---- typed specs ------------------------------------------------------------------------------------------------

SpecParts splitSpec(std::string_view spec) {
  auto trim = [](std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
  };
  spec = trim(spec);
  static constexpr std::string_view kOps[] = {"<=", ">=", "==", "!=", "&&", "||", "^^", "+", "-", "*", "/", "%", "<", ">"};
  for (const std::string_view op : kOps)
    if (spec.starts_with(op) && spec.size() > op.size()) {
      const std::string_view rest = trim(spec.substr(op.size()));
      if (rest.empty()) break;
      // `-3dB` is a literal, `- 3` an operator with an operand.
      if (op == "-" && spec[1] != ' ') break;
      return {std::string(op), std::string(rest)};
    }
  return {std::string(spec), {}};
}

namespace {

std::vector<graph::SubcircuitRef> specCallees(std::span<const graph::SubcircuitRef> more) {
  std::vector<graph::SubcircuitRef> out(more.begin(), more.end());
  if (const auto p = meta::prelude())
    for (const graph::SubcircuitRef& r : p->signatures) out.push_back(r);
  return out;
}

// The node of `spec` added to `p` (with its literal operand wired); errors say why the text is not one node.
Result<graph::NodeIndex> addSpecNode(graph::Patch& p, std::string_view spec, std::span<const graph::SubcircuitRef> callees) {
  const SpecParts parts = splitSpec(spec);
  if (parts.node.empty()) return Error{ErrorCode::InvalidArgument, "type a node: an atom, a literal, an operator or a declaration", {}};
  auto n = graph::edit::addNode(p, parts.node, callees);
  if (!n) return std::move(n).error();
  if (!parts.operand.empty()) {
    const graph::Node& nd = p.node(*n);
    if (nd.kind != graph::NodeKind::Atom || nd.numInputs < 2)
      return Error{ErrorCode::InvalidArgument, "an operand follows a binary operator (`* 2`)", {}};
    auto c = graph::edit::addNode(p, parts.operand, {});
    if (!c || p.node(*c).kind != graph::NodeKind::Const)
      return Error{ErrorCode::InvalidArgument, "`" + parts.operand + "` is not a literal", {}};
    if (auto w = graph::edit::connect(p, graph::PortRef{*c, 0}, graph::PortRef{*n, 1}); !w) return std::move(w).error();
  }
  return *n;
}

}  // namespace

bool nodeSpecValid(std::string_view spec, NodeCatalog::Scope scope, std::span<const graph::SubcircuitRef> callees) {
  graph::Patch p("Spec");
  const std::vector<graph::SubcircuitRef> all = specCallees(callees);
  auto n = addSpecNode(p, spec, all);
  if (!n) return false;
  if (scope == NodeCatalog::Scope::Circuit) return true;
  const graph::NodeKind k = p.node(*n).kind;
  return k == graph::NodeKind::Atom || k == graph::NodeKind::Const || k == graph::NodeKind::Param ||
         (k == graph::NodeKind::Subcircuit && meta::typeOfCall(p.subcircuit(*n).patch) != nullptr);
}

Result<SpecDevice> specDevice(std::string_view spec) {
  graph::Patch probe("Spec");
  const std::vector<graph::SubcircuitRef> callees = specCallees({});
  auto pn = addSpecNode(probe, spec, callees);
  if (!pn) return std::move(pn).error();
  const graph::Node& pnode = probe.node(*pn);
  std::string name;
  switch (pnode.kind) {
    case graph::NodeKind::Atom: name = std::string(graph::atomSignature(pnode.atom).name); break;
    case graph::NodeKind::Const: name = "Const"; break;
    case graph::NodeKind::Param: name = probe.nodeName(*pn); break;
    case graph::NodeKind::Subcircuit:
      return Error{ErrorCode::InvalidArgument, "a meta module is placed as itself (its face), a patch inside a device", {}};
    default: return Error{ErrorCode::InvalidArgument, "a declaration (in, out, export) is a port of a patch, not a node", {}};
  }
  // The patch's name: the node's, capitalised (atom names are lower case: never an atom's name), letters only.
  std::string patchName;
  for (const char ch : name)
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')
      patchName.push_back(patchName.empty() && ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 32) : ch);
    else if (ch == '.')
      patchName.push_back('_');
  if (patchName.empty() || (patchName[0] >= '0' && patchName[0] <= '9')) patchName = "Node" + patchName;
  graph::Patch p(patchName);
  // Which inputs become the device's: the required ones not wired by the spec (an optional one keeps its default).
  std::vector<uint32_t> ins;
  {
    const graph::Adjacency adj = probe.adjacency();
    for (uint32_t i = 0; i < pnode.numInputs; ++i)
      if (!probe.inputOptional(*pn, i) && adj.driver[probe.flatInput(*pn, i)] == graph::kNone) ins.push_back(i);
  }
  const bool firstReal = !ins.empty() && probe.inputType(*pn, ins.front()).kind != graph::Kind::Bool;
  // A node without a real first input still makes an audio effect: an audio input first that drives nothing.
  if (!firstReal) {
    if (auto d = graph::edit::addNode(p, "in " + graph::edit::uniqueName(p, "x") + " : audio", {}); !d) return std::move(d).error();
  }
  std::vector<graph::NodeIndex> inNodes;
  for (const uint32_t i : ins) {
    const std::string nm = graph::edit::uniqueName(p, std::string(probe.inputName(*pn, i)));
    const bool b = probe.inputType(*pn, i).kind == graph::Kind::Bool;
    auto d = graph::edit::addNode(p, "in " + nm + " : " + (b ? "bool" : "audio"), {});
    if (!d) return std::move(d).error();
    inNodes.push_back(*d);
  }
  auto n = addSpecNode(p, spec, callees);
  if (!n) return std::move(n).error();
  for (size_t k = 0; k < ins.size(); ++k)
    if (auto w = graph::edit::connect(p, graph::PortRef{inNodes[k], 0}, graph::PortRef{*n, static_cast<uint16_t>(ins[k])}); !w) return std::move(w).error();
  const uint32_t outs = p.node(*n).numOutputs;
  for (uint32_t o = 0; o < outs; ++o) {
    std::string base = p.node(*n).kind == graph::NodeKind::Atom ? std::string(p.outputName(*n, o)) : std::string("out");
    if (base.empty()) base = "out";
    const std::string nm = graph::edit::uniqueName(p, base);
    const bool b = p.outputType(*n, o).kind == graph::Kind::Bool;
    auto d = graph::edit::addNode(p, "out " + nm + " : " + (b ? "bool" : "audio"), {});
    if (!d) return std::move(d).error();
    if (auto w = graph::edit::connect(p, graph::PortRef{*n, static_cast<uint16_t>(o)}, graph::PortRef{*d, 0}); !w) return std::move(w).error();
  }
  if (outs == 0) return Error{ErrorCode::InvalidArgument, "`" + std::string(spec) + "` has no output: it cannot be a device", {}};
  SpecDevice out;
  out.patch = patchName;
  out.text = graph::print(p);
  {
    std::string_view t = spec;
    while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
    while (!t.empty() && t.back() == ' ') t.remove_suffix(1);
    out.name = std::string(t);
  }
  return out;
}

}  // namespace pq::views::circuit
