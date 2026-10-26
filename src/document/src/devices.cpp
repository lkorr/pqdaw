// The device library and catalog (T4.1): pq/document/devices.h.
#include "pq/document/devices.h"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "pq/document/lower.h"
#include "pq/graph/parse.h"
#include "pq/graph/patch_file.h"

namespace pq::doc {

namespace fs = std::filesystem;

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
  return info;
}

Result<std::string> readText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Error{ErrorCode::Io, "cannot read " + path.string(), {}};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string joinDiagnostics(std::string_view text, const std::vector<graph::Diagnostic>& ds) {
  std::string m;
  for (const graph::Diagnostic& d : ds) {
    if (!m.empty()) m += '\n';
    m += graph::formatDiagnostic(text, d);
  }
  return m;
}

fs::path utf8Path(std::string_view utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

constexpr StockDevice kStock[] = {
    {"instruments/basic_synth.pqp", "BasicSynth", "Basic Synth", DeviceKind::Instrument},
    {"devices/operator.pqp", "operator", "Operator", DeviceKind::Instrument},  // T4.12
    {"devices/utility.pqp", "utility", "Utility"},
    {"devices/saturator.pqp", "saturator", "Saturator"},
    {"devices/eq_eight.pqp", "eq_eight", "EQ Eight"},
    {"devices/lfo_device.pqp", "lfo_device", "LFO"},  // T4.7: a modulation source (its `lfo` jack)
};

}  // namespace

// Every parsed file (device files and their imports, each once), kept alive for the library.
struct DeviceLibrary::Files final : graph::ImportLoader {
  std::deque<std::string> texts;
  std::vector<std::unique_ptr<graph::ParseResult>> parsed;
  std::map<std::string, graph::ParseResult*> byPath;
  std::map<std::string, int> active;
  // T4.4-fix1: per stock device, its file (the key its imports resolve against, its text and parse).
  struct Stock {
    std::string patch, key;
    const std::string* text = nullptr;
    const graph::ParseResult* parsed = nullptr;
  };
  std::vector<Stock> stock;
  std::map<const graph::ParseResult*, std::pair<std::string, const std::string*>> origin;  // parse → (key, text)
  const Stock* stockOf(std::string_view patch) const {
    for (const Stock& s : stock)
      if (s.patch == patch) return &s;
    return nullptr;
  }

  Result<graph::ParseResult*> loadFile(const fs::path& path) {
    std::error_code ec;
    const fs::path full = fs::weakly_canonical(path, ec);
    const std::string key = (ec ? path : full).string();
    if (auto it = byPath.find(key); it != byPath.end()) return it->second;
    if (active.count(key)) return Error{ErrorCode::InvalidArgument, "import cycle at " + key, {}};
    auto text = readText(ec ? path : full);
    if (!text) return text.error();
    active[key] = 1;
    texts.push_back(std::move(*text));
    const std::string* mine = &texts.back();  // a deque: stays valid while imports push their texts
    graph::ParseOptions po;
    po.imports = this;
    auto r = std::make_unique<graph::ParseResult>(graph::parseDetailed(*mine, key, po));
    active.erase(key);
    if (!r->ok()) return Error{ErrorCode::Parse, joinDiagnostics(*mine, r->diagnostics), {}};
    graph::ParseResult* raw = r.get();
    byPath[key] = raw;
    origin[raw] = {key, mine};
    parsed.push_back(std::move(r));
    return raw;
  }

  Result<std::vector<graph::SubcircuitRef>> load(std::string_view path, std::string_view importer) override {
    auto r = loadFile(fs::path(importer).parent_path() / utf8Path(path));
    if (!r) return std::move(r).error();
    return graph::signatures((*r)->file);
  }
};

// T4.4-fix1: resolved user devices, per device id (resolve()).
struct DeviceLibrary::UserCache {
  std::mutex mutex;
  struct Entry {
    std::shared_ptr<const ResolvedDevice> r;
    uint64_t used = 0;
  };
  std::unordered_map<uint32_t, std::vector<Entry>> byDevice;
  uint64_t clock = 0;
  size_t baseFiles = 0;  // files_->parsed of the stock set (later ones: user imports)
};

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

void DeviceCatalog::add(DeviceInfo info) {
  for (DeviceInfo& d : devices_)
    if (d.patch == info.patch) {
      d = std::move(info);
      return;
    }
  devices_.push_back(std::move(info));
}

const DeviceInfo* DeviceCatalog::find(std::string_view patch) const noexcept {
  for (const DeviceInfo& d : devices_)
    if (d.patch == patch) return &d;
  return nullptr;
}

double deviceParamValue(const Device& d, const DeviceParamInfo& p) noexcept {
  const DeviceValue* v = d.param(p.name);
  return v != nullptr ? v->value : p.defaultValue;
}

double deviceSettingValue(const Device& d, const DeviceSettingInfo& s) noexcept {
  const DeviceValue* v = d.setting(s.name);
  return v != nullptr ? v->value : s.defaultValue;
}

std::span<const StockDevice> stockDevices() noexcept { return kStock; }

std::string DeviceLibrary::pathOf(std::string_view patch) const {
  for (const auto& [p, file] : paths_)
    if (p == patch) return file;
  return {};
}

DeviceLibrary::DeviceLibrary() : files_(std::make_unique<Files>()), users_(std::make_unique<UserCache>()) {}
DeviceLibrary::~DeviceLibrary() = default;

Result<std::unique_ptr<DeviceLibrary>> loadDeviceLibrary(std::string_view stdlibDirUtf8) {
  std::unique_ptr<DeviceLibrary> lib(new DeviceLibrary());
  const fs::path root = utf8Path(stdlibDirUtf8);
  std::vector<std::pair<const graph::Patch*, const StockDevice*>> found;
  for (const StockDevice& sd : kStock) {
    const fs::path path = root / utf8Path(sd.file);
    std::error_code ec;
    if (!fs::exists(path, ec)) {  // a reduced stdlib (a test's probe instrument): that device is not available
      lib->missing_.push_back(std::string(sd.patch));
      continue;
    }
    auto file = lib->files_->loadFile(path);
    if (!file) return std::move(file).error();
    const graph::Patch* patch = (*file)->file.find(sd.patch);
    if (!patch)
      return Error{ErrorCode::NotFound, "no patch '" + std::string(sd.patch) + "' in " + std::string(sd.file), {}};
    found.emplace_back(patch, &sd);
    const std::u8string u8 = path.u8string();
    lib->paths_.emplace_back(std::string(sd.patch), std::string(u8.begin(), u8.end()));
    const auto& [key, text] = lib->files_->origin.at(*file);
    lib->files_->stock.push_back(DeviceLibrary::Files::Stock{std::string(sd.patch), key, text, *file});
  }
  // The device files first (in stock order), then every file they import: the first patch under a name wins.
  for (const auto& [patch, sd] : found) lib->library_.add(*patch);
  for (const auto& r : lib->files_->parsed) {
    lib->library_.add(r->file);
    for (graph::SubcircuitRef& s : graph::signatures(r->file)) lib->signatures_.push_back(std::move(s));
  }
  if (found.empty()) return Error{ErrorCode::NotFound, "no stock device under " + root.string(), {}};
  for (const auto& [patch, sd] : found) {
    auto info = describeDevice(*patch, std::string(sd->name));
    if (!info) return std::move(info).error();
    if (info->kind != sd->kind)
      return Error{ErrorCode::InvalidArgument, "device library: " + std::string(sd->patch) + " is listed as " +
                                                   (sd->kind == DeviceKind::Instrument ? "an instrument" : "an audio effect") +
                                                   " but its patch is not one", {}};
    lib->catalog_.add(std::move(*info));
  }
  lib->users_->baseFiles = lib->files_->parsed.size();
  return lib;
}

// ---- User devices (T4.4-fix1) -----------------------------------------------------------------------------------------

namespace {

// What the program depends on in a file's patches (lower.h patchHash per patch, plus the solve blocks' iterations):
// equal for a layout, comment or formatting edit.
uint64_t graphHash(const graph::PatchFile& f) noexcept {
  uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&](uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  };
  for (const graph::Patch& p : f.patches) {
    mix(patchHash(p));
    for (const graph::Solve& s : p.solves()) mix(s.iterations);
  }
  return h;
}

std::string hex16(uint64_t v) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string s(16, '0');
  for (int i = 15; i >= 0; --i, v >>= 4) s[static_cast<size_t>(i)] = kDigits[v & 15];
  return s;
}

std::shared_ptr<const UserDeviceGraph> renamedGraph(const graph::PatchFile& parsed, const std::string& rootName,
                                                   DeviceId device, uint64_t hash) {
  auto g = std::make_shared<UserDeviceGraph>();
  g->file = parsed;
  g->hash = hash;
  const std::string suffix = "__u" + std::to_string(device.value) + "_" + hex16(hash);
  std::vector<std::string> own;
  for (const graph::Patch& p : parsed.patches) own.push_back(p.name());
  for (graph::Patch& p : g->file.patches) {
    p.setName(p.name() + suffix);
    for (graph::NodeIndex n = 0; n < p.nodeCount(); ++n)
      if (p.node(n).kind == graph::NodeKind::Subcircuit &&
          std::find(own.begin(), own.end(), p.subcircuit(n).patch) != own.end())
        p.setSubcircuitPatch(n, p.subcircuit(n).patch + suffix);
  }
  g->root = rootName + suffix;
  for (const graph::Patch& p : g->file.patches) g->library.add(p);
  return g;
}

}  // namespace

std::shared_ptr<const ResolvedDevice> DeviceLibrary::resolve(const Device& d) const {
  if (!d.source) return nullptr;
  UserCache& c = *users_;
  std::lock_guard lock(c.mutex);
  std::vector<UserCache::Entry>& list = c.byDevice[d.id.value];
  const uint64_t now = ++c.clock;
  for (UserCache::Entry& e : list)
    if (e.r->source == d.source || e.r->source->text == d.source->text) {
      e.used = now;
      return e.r;
    }
  // The ids to carry: the editor's parse of this text, else the device's last parse, else the stock file's.
  const UserCache::Entry* latest = nullptr;  // the most recently used entry that parsed
  const UserCache::Entry* lastGood = nullptr;  // … that plays its own graph
  for (const UserCache::Entry& e : list) {
    if (e.r->parsedOk && (!latest || e.used > latest->used)) latest = &e;
    if (e.r->graph && !e.r->stale && (!lastGood || e.used > lastGood->used)) lastGood = &e;
  }
  const Files::Stock* stock = files_->stockOf(d.patch);
  const graph::PatchFile* previous = d.source->ids.get();
  if (!previous && latest) previous = &latest->r->parsed;
  if (!previous && stock) previous = &stock->parsed->file;

  auto r = std::make_shared<ResolvedDevice>();
  r->device = d.id;
  r->source = d.source;
  const DeviceInfo* stockInfo = catalog_.find(d.patch);
  const std::string displayName = stockInfo ? stockInfo->name : d.patch;
  if (!stock) {
    r->error = "the stock device '" + d.patch + "' it was made from is not in the library";
  } else {
    graph::ParseOptions po;
    po.imports = files_.get();
    po.previous = previous;
    po.lenientLayout = true;  // as the circuit editor parses (the same ids for the same text)
    po.deferMissingInputs = true;
    graph::ParseResult pr = graph::parseDetailed(d.source->text, stock->key, po);
    if (!pr.lowered) {
      r->error = pr.diagnostics.empty() ? std::string("the text does not parse")
                                        : graph::formatDiagnostic(d.source->text, pr.diagnostics.front());
    } else {
      r->parsed = std::move(pr.file);
      r->parsedOk = true;
      const graph::Patch* root = r->parsed.find(d.patch);
      if (!pr.diagnostics.empty()) {
        r->error = graph::formatDiagnostic(d.source->text, pr.diagnostics.front());
      } else if (!root) {
        r->error = "the edited text has no patch '" + d.patch + "'";
      } else {
        auto info = describeDevice(*root, displayName);
        if (!info) {
          r->error = info.error().message;
        } else if (info->kind != d.kind) {
          r->error = std::string("the edited patch is not ") +
                     (d.kind == DeviceKind::Instrument ? "an instrument" : "an audio effect") + " any more";
        } else {
          const uint64_t h = graphHash(r->parsed);
          for (const UserCache::Entry& e : list)  // the same graph as an earlier text (a layout / comment edit)
            if (e.r->graph && !e.r->stale && e.r->graph->hash == h) r->graph = e.r->graph;
          if (!r->graph) r->graph = renamedGraph(r->parsed, d.patch, d.id, h);
          r->info = std::move(*info);
        }
      }
    }
  }
  if (!r->error.empty() && lastGood) {  // the last good version keeps playing
    r->graph = lastGood->r->graph;
    r->info = lastGood->r->info;
    r->stale = true;
  }
  for (size_t i = c.baseFiles; i < files_->parsed.size(); ++i) r->extraFiles.push_back(&files_->parsed[i]->file);
  list.push_back(UserCache::Entry{r, now});
  if (list.size() > kResolvedPerDevice) {
    auto oldest = std::min_element(list.begin(), list.end(),
                                   [](const UserCache::Entry& a, const UserCache::Entry& b) { return a.used < b.used; });
    list.erase(oldest);
  }
  return r;
}

std::string DeviceLibrary::stockText(std::string_view patch) const {
  const Files::Stock* s = files_->stockOf(patch);
  return s && s->text ? *s->text : std::string();
}

const graph::PatchFile* DeviceLibrary::stockFile(std::string_view patch) const {
  const Files::Stock* s = files_->stockOf(patch);
  return s ? &s->parsed->file : nullptr;
}

const graph::Patch* DeviceLibrary::patchOf(const Device& d, std::shared_ptr<const ResolvedDevice>& hold) const {
  if (!d.source) return library_.find(d.patch);
  hold = resolve(d);
  return hold ? hold->root() : nullptr;
}

const DeviceInfo* DeviceLibrary::infoOf(const Device& d, std::shared_ptr<const ResolvedDevice>& hold) const {
  if (!d.source) return catalog_.find(d.patch);
  hold = resolve(d);
  return hold && hold->graph ? &hold->info : nullptr;
}

}  // namespace pq::doc
