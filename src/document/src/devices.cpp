// The device library and catalog (T4.1): pq/document/devices.h.
#include "pq/document/devices.h"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

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

fs::path pathOf(std::string_view utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

constexpr StockDevice kStock[] = {
    {"instruments/basic_synth.pqp", "BasicSynth", "Basic Synth"},
    {"devices/utility.pqp", "utility", "Utility"},
    {"devices/saturator.pqp", "saturator", "Saturator"},
    {"devices/eq_eight.pqp", "eq_eight", "EQ Eight"},
};

}  // namespace

// Every parsed file (device files and their imports, each once), kept alive for the library.
struct DeviceLibrary::Files final : graph::ImportLoader {
  std::deque<std::string> texts;
  std::vector<std::unique_ptr<graph::ParseResult>> parsed;
  std::map<std::string, graph::ParseResult*> byPath;
  std::map<std::string, int> active;

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
    graph::ParseOptions po;
    po.imports = this;
    auto r = std::make_unique<graph::ParseResult>(graph::parseDetailed(texts.back(), key, po));
    active.erase(key);
    if (!r->ok()) return Error{ErrorCode::Parse, joinDiagnostics(texts.back(), r->diagnostics), {}};
    graph::ParseResult* raw = r.get();
    byPath[key] = raw;
    parsed.push_back(std::move(r));
    return raw;
  }

  Result<std::vector<graph::SubcircuitRef>> load(std::string_view path, std::string_view importer) override {
    auto r = loadFile(fs::path(importer).parent_path() / pathOf(path));
    if (!r) return std::move(r).error();
    return graph::signatures((*r)->file);
  }
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

DeviceLibrary::DeviceLibrary() : files_(std::make_unique<Files>()) {}
DeviceLibrary::~DeviceLibrary() = default;

Result<std::unique_ptr<DeviceLibrary>> loadDeviceLibrary(std::string_view stdlibDirUtf8) {
  std::unique_ptr<DeviceLibrary> lib(new DeviceLibrary());
  const fs::path root = pathOf(stdlibDirUtf8);
  std::vector<std::pair<const graph::Patch*, const StockDevice*>> found;
  for (const StockDevice& sd : kStock) {
    const fs::path path = root / pathOf(sd.file);
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
    lib->catalog_.add(std::move(*info));
  }
  return lib;
}

}  // namespace pq::doc
