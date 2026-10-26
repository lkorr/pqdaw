#pragma once

// Tests only (T4.5): compile patches with live probes (pq/graph/inspect.h) on named nodes or on every node output of the
// root patch, run them with taps bound (pq/engine/inspect.h), and compare with the same patch compiled without probes.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "interp_harness.h"
#include "pq/core/probe_level.h"
#include "pq/core/tap_ring.h"
#include "pq/engine/inspect.h"
#include "pq/graph/inspect.h"

namespace probe_test {

using namespace pq::graph;
namespace eng = pq::engine;
namespace fs = std::filesystem;

struct Pair {
  std::string file, name;
  interp_test::Compiled plain, probed;
  std::vector<std::string> probeNames;  // per probe id
};

// Infer → optimise → schedule `patch` with `targets` as probes (ids = indices). Null on any error.
inline bool compileOne(const Patch& patch, const PatchLibrary& lib, std::span<const ProbeTarget> targets, double sr,
                       interp_test::Compiled& out, std::string& why, bool voiceSum = false) {
  InferOptions io;
  io.library = &lib;
  io.probes = targets;
  InferResult inf = inferDetailed(patch, io);
  if (!inf.ok()) {
    std::vector<RootInput> roots;
    const Adjacency adj = patch.adjacency();
    for (NodeIndex n = 0; n < patch.nodeCount(); ++n) {
      if (patch.node(n).kind != NodeKind::Input) continue;
      RootInput ri;
      ri.connected = adj.driver[patch.flatInput(n, 0)] == kNone;
      roots.push_back(ri);
    }
    io.inputs = roots;
    inf = inferDetailed(patch, io);
  }
  if (!inf.ok()) {
    why = inf.diagnostics.front().message;
    return false;
  }
  std::vector<uint32_t> valueMap;
  OptimiseOptions oo;
  oo.sampleRate = sr;
  oo.valueMap = &valueMap;
  const std::vector<uint32_t> sources = inf.probeSources;
  auto opt = optimise(std::move(inf.typed), oo);
  if (!opt.ok()) {
    why = opt.error().message;
    return false;
  }
  std::vector<ProbePoint> points = probePoints(opt.value().patch, sources, valueMap);
  for (ProbePoint& p : points) p.voiceSum = voiceSum;
  ScheduleOptions so;
  so.sampleRate = sr;
  so.probes = points;
  ScheduleResult sr2 = scheduleDetailed(opt.value(), so);
  if (!sr2.ok()) {
    why = sr2.diagnostics.front().message;
    return false;
  }
  out.name = patch.name();
  out.typed = std::move(opt.value());
  out.sched = std::move(sr2.schedule);
  return true;
}

// Every output of every root node (up to `maxProbes`), named "<node>.<output>".
inline std::vector<ProbeTarget> everyOutput(const Patch& p, std::vector<std::string>& names, uint32_t maxProbes = 48) {
  std::vector<ProbeTarget> t;
  for (NodeIndex n = 0; n < p.nodeCount() && t.size() < maxProbes; ++n)
    for (uint16_t o = 0; o < p.node(n).numOutputs && t.size() < maxProbes; ++o) {
      ProbeTarget pt;
      pt.node = p.node(n).id;
      pt.output = o;
      t.push_back(pt);
      names.push_back(p.describe(n) + "." + std::to_string(o));
    }
  return t;
}

// Every patch of `path` (imports loaded), compiled without and with a probe on every root node output.
inline std::vector<Pair> compileFilePairs(const fs::path& path, double sr, std::vector<std::string>& skipped) {
  std::vector<Pair> out;
  text_util::DirLoader loader(path.parent_path());
  std::vector<std::unique_ptr<ParseResult>> files;
  std::vector<std::string> texts;
  std::vector<fs::path> pending{path};
  std::vector<std::string> seen;
  while (!pending.empty()) {
    const fs::path p = fs::weakly_canonical(pending.front());
    pending.erase(pending.begin());
    if (std::find(seen.begin(), seen.end(), p.string()) != seen.end()) continue;
    seen.push_back(p.string());
    texts.push_back(text_util::readFile(p));
    ParseOptions po;
    po.imports = &loader;
    files.push_back(std::make_unique<ParseResult>(parseDetailed(texts.back(), p.string(), po)));
    if (!files.back()->ok()) {
      if (files.size() == 1) return out;
      continue;
    }
    for (const std::string& imp : files.back()->file.imports) pending.push_back(p.parent_path() / imp);
  }
  PatchLibrary lib;
  for (const auto& f : files)
    if (f->ok()) lib.add(f->file);
  for (const Patch& patch : files.front()->file.patches) {
    Pair pr;
    pr.file = path.filename().string();
    pr.name = patch.name();
    std::string why;
    const std::vector<ProbeTarget> targets = everyOutput(patch, pr.probeNames);
    if (!compileOne(patch, lib, {}, sr, pr.plain, why) || !compileOne(patch, lib, targets, sr, pr.probed, why)) {
      skipped.push_back(pr.file + ":" + pr.name + ": " + why);
      continue;
    }
    pr.plain.file = pr.probed.file = pr.file;
    out.push_back(std::move(pr));
  }
  return out;
}

// One patch from text with probes on the named root nodes (output 0, or "name.k" for output k).
inline interp_test::Compiled compileText(const std::string& text, const std::vector<std::string>& probes, bool voiceSum = false,
                                         double sr = 48000.0) {
  ParseResult pr = parseDetailed(text, "test.pqp");
  if (!pr.ok()) {
    std::fprintf(stderr, "%s\n", formatDiagnostic(text, pr.diagnostics.front()).c_str());
    std::abort();
  }
  PatchLibrary lib;
  lib.add(pr.file);
  const Patch& p = pr.file.patches[0];
  std::vector<ProbeTarget> targets;
  for (const std::string& spec : probes) {
    std::string name = spec;
    uint16_t output = 0;
    if (const size_t dot = spec.find('.'); dot != std::string::npos) {
      name = spec.substr(0, dot);
      output = static_cast<uint16_t>(std::stoi(spec.substr(dot + 1)));
    }
    ProbeTarget t;
    for (NodeIndex n = 0; n < p.nodeCount(); ++n)
      if (p.nodeName(n) == name) t.node = p.node(n).id;
    if (!t.node.valid()) {
      std::fprintf(stderr, "no node '%s'\n", name.c_str());
      std::abort();
    }
    t.output = output;
    targets.push_back(t);
  }
  interp_test::Compiled c;
  std::string why;
  if (!compileOne(p, lib, targets, sr, c, why, voiceSum)) {
    std::fprintf(stderr, "%s\n", why.c_str());
    std::abort();
  }
  return c;
}

// Taps for every probe id of a schedule: a ring (with a reader registered) and a level each.
struct Taps {
  std::vector<std::unique_ptr<pq::TapRing>> rings;
  std::vector<std::unique_ptr<pq::ProbeLevel>> levels;
  std::vector<eng::ProbeTap> taps;
  explicit Taps(const Schedule& s, uint32_t capacity = 1u << 15) {
    for (const ProbeRef& p : s.probes) {
      rings.push_back(std::make_unique<pq::TapRing>(1, capacity, 1024));
      rings.back()->addReader();
      levels.push_back(std::make_unique<pq::ProbeLevel>());
      taps.push_back({p.id, rings.back().get(), levels.back().get()});
    }
  }
  // Everything probe `id` received (≤ capacity − 1024 frames).
  std::vector<float> all(uint32_t id) const {
    for (size_t i = 0; i < taps.size(); ++i)
      if (taps[i].id == id) {
        const pq::TapRing& r = *rings[i];
        const uint64_t from = r.oldestReadable(), n = r.written() - from;
        std::vector<float> v(n);
        if (n && r.read(0, from, static_cast<uint32_t>(n), v.data()) != n) v.clear();
        return v;
      }
    return {};
  }
  const pq::ProbeLevel* level(uint32_t id) const {
    for (size_t i = 0; i < taps.size(); ++i)
      if (taps[i].id == id) return levels[i].get();
    return nullptr;
  }
};

// An image builder that binds `taps` (and `meter`) to each image it builds through `inner` (empty: the interpreter).
inline interp_test::ImageBuilder binding(const interp_test::ImageBuilder& inner, const Taps* taps, eng::CostMeter* meter = nullptr) {
  return [inner, taps, meter](const Schedule& s, const eng::InterpretOptions& io) -> pq::Result<std::unique_ptr<eng::ProgramImage>> {
    auto img = inner ? inner(s, io) : eng::buildInterpreted(s, io);
    if (!img.ok()) return img;
    eng::InspectBinding b;
    if (taps) b.probes = taps->taps;
    b.meter = meter;
    if (!(*img)->bindInspect(b)) return pq::Error{pq::ErrorCode::Unsupported, "bindInspect refused", {}};
    return img;
  };
}

// Streams equal bit for bit (doubles made from floats; NaN equal to NaN).
inline bool sameStreams(const interp_test::Streams& a, const interp_test::Streams& b, std::string& where) {
  if (a.size() != b.size()) {
    where = "different stream sets";
    return false;
  }
  for (const auto& [k, x] : a) {
    const auto it = b.find(k);
    if (it == b.end() || it->second.size() != x.size()) {
      where = "stream missing or of another length";
      return false;
    }
    for (size_t t = 0; t < x.size(); ++t) {
      const double p = x[t], q = it->second[t];
      if (std::isnan(p) && std::isnan(q)) continue;
      if (std::memcmp(&p, &q, sizeof p) != 0) {
        where = "stream (" + std::to_string(k.kind) + ", " + std::to_string(k.a) + ") differs at t = " + std::to_string(t);
        return false;
      }
    }
  }
  return true;
}

inline bool sameFloats(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

}  // namespace probe_test
