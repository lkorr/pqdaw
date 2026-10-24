// Per-track compile units (T1.x-arch1): pq/graph/project.h describes the rules.
#include "pq/graph/project.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstring>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <utility>

#include "pq/graph/patch_file.h"

namespace pq::graph {

namespace {

Error projectError(ErrorCode code, const std::string& m) { return Error{code, "project: " + m, {}}; }

// The declaration node of a port: an Output/Export (`output`) or an Input/Param of the patch, by name.
std::optional<NodeIndex> findDecl(const Patch& p, std::string_view name, bool output) {
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    const NodeKind k = p.node(n).kind;
    const bool match = output ? (k == NodeKind::Output || k == NodeKind::Export)
                              : (k == NodeKind::Input || k == NodeKind::Param);
    if (match && p.nodeName(n) == name) return n;
  }
  return std::nullopt;
}

std::optional<uint16_t> findPort(const std::vector<SubcircuitPort>& ports, std::string_view name) {
  for (size_t i = 0; i < ports.size(); ++i)
    if (ports[i].name == name) return static_cast<uint16_t>(i);
  return std::nullopt;
}

std::string trackText(const ProjectGraph& g, uint32_t t) {
  const ProjectTrack& tr = g.tracks[t];
  return "track " + std::to_string(t) + " '" + tr.name + "' (id " + std::to_string(tr.id) + ")";
}

Status validateGraph(const ProjectGraph& g) {
  if (g.tracks.empty()) return projectError(ErrorCode::InvalidArgument, "no tracks");
  std::vector<uint32_t> ids;
  for (uint32_t t = 0; t < g.tracks.size(); ++t) {
    const ProjectTrack& tr = g.tracks[t];
    if (tr.id == 0) return projectError(ErrorCode::InvalidArgument, trackText(g, t) + ": id 0 is invalid");
    if (tr.patch == nullptr) return projectError(ErrorCode::InvalidArgument, trackText(g, t) + ": no patch");
    ids.push_back(tr.id);
  }
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
    return projectError(ErrorCode::InvalidArgument, "track ids are not unique");
  auto checkPort = [&](const ProjectPort& p, bool output, const char* what) -> Status {
    if (p.track >= g.tracks.size())
      return projectError(ErrorCode::InvalidArgument, std::string(what) + ": no track " + std::to_string(p.track));
    if (!findDecl(*g.tracks[p.track].patch, p.port, output))
      return projectError(ErrorCode::NotFound, std::string(what) + ": " + trackText(g, p.track) + " has no " +
                                                   (output ? "out/export" : "in/param") + " '" + p.port + "'");
    return Ok{};
  };
  for (const ProjectWire& w : g.wires) {
    if (Status s = checkPort(w.from, true, "wire source"); !s.ok()) return s;
    if (Status s = checkPort(w.to, false, "wire target"); !s.ok()) return s;
  }
  return checkPort(g.output, true, "project output");
}

struct UnionFind {
  std::vector<uint32_t> parent;
  explicit UnionFind(uint32_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0u); }
  uint32_t find(uint32_t x) {
    while (parent[x] != x) x = parent[x] = parent[parent[x]];
    return x;
  }
  void unite(uint32_t a, uint32_t b) {
    a = find(a), b = find(b);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);
  }
};

uint32_t instruments(const ProjectGraph& g, UnionFind& uf, uint32_t root) {
  uint32_t n = 0;
  for (uint32_t t = 0; t < g.tracks.size(); ++t)
    if (g.tracks[t].role == TrackRole::Instrument && uf.find(t) == root) ++n;
  return n;
}

// Tarjan's SCCs over `adj` (nodes 0..n-1), iterative. Returns the component of every node.
std::vector<uint32_t> sccs(uint32_t n, const std::vector<std::vector<uint32_t>>& adj, uint32_t& count) {
  std::vector<uint32_t> index(n, kNone), low(n, 0), comp(n, kNone), stack;
  std::vector<uint8_t> onStack(n, 0);
  uint32_t next = 0;
  count = 0;
  struct Frame { uint32_t v, edge; };
  std::vector<Frame> call;
  for (uint32_t s = 0; s < n; ++s) {
    if (index[s] != kNone) continue;
    call.push_back({s, 0});
    index[s] = low[s] = next++;
    stack.push_back(s);
    onStack[s] = 1;
    while (!call.empty()) {
      Frame& f = call.back();
      if (f.edge < adj[f.v].size()) {
        const uint32_t w = adj[f.v][f.edge++];
        if (index[w] == kNone) {
          index[w] = low[w] = next++;
          stack.push_back(w);
          onStack[w] = 1;
          call.push_back({w, 0});
        } else if (onStack[w]) {
          low[f.v] = std::min(low[f.v], index[w]);
        }
        continue;
      }
      const uint32_t v = f.v;
      call.pop_back();
      if (!call.empty()) low[call.back().v] = std::min(low[call.back().v], low[v]);
      if (low[v] == index[v]) {
        while (true) {
          const uint32_t w = stack.back();
          stack.pop_back();
          onStack[w] = 0;
          comp[w] = count;
          if (w == v) break;
        }
        ++count;
      }
    }
  }
  return comp;
}

// True when the unit graph of these sets has a cycle (SCC) holding more than one instrument.
bool cycleWithInstruments(const ProjectGraph& g, UnionFind& uf) {
  const auto nt = static_cast<uint32_t>(g.tracks.size());
  std::vector<uint32_t> setIndex(nt, kNone), inst;
  uint32_t nu = 0;
  for (uint32_t t = 0; t < nt; ++t) {
    const uint32_t r = uf.find(t);
    if (setIndex[r] == kNone) {
      setIndex[r] = nu++;
      inst.push_back(0);
    }
    if (g.tracks[t].role == TrackRole::Instrument) ++inst[setIndex[r]];
  }
  std::vector<std::vector<uint32_t>> adj(nu);
  for (const ProjectWire& w : g.wires) {
    const uint32_t a = setIndex[uf.find(w.from.track)], b = setIndex[uf.find(w.to.track)];
    if (a != b) adj[a].push_back(b);
  }
  uint32_t count = 0;
  const std::vector<uint32_t> comp = sccs(nu, adj, count);
  std::vector<uint32_t> compInst(count, 0), compSize(count, 0);
  for (uint32_t u = 0; u < nu; ++u) {
    compInst[comp[u]] += inst[u];
    ++compSize[comp[u]];
  }
  for (uint32_t c = 0; c < count; ++c)
    if (compSize[c] > 1 && compInst[c] > 1) return true;
  return false;
}

// Units from the union-find sets, in dependency order; buses for every cross-unit wire and the output.
Result<ProjectPartition> buildPartition(const ProjectGraph& g, UnionFind& uf, std::vector<std::string> notes) {
  const auto nt = static_cast<uint32_t>(g.tracks.size());
  std::vector<uint32_t> setIndex(nt, kNone);
  std::vector<std::vector<uint32_t>> members;
  for (uint32_t t = 0; t < nt; ++t) {
    const uint32_t r = uf.find(t);
    if (setIndex[r] == kNone) {
      setIndex[r] = static_cast<uint32_t>(members.size());
      members.emplace_back();
    }
    members[setIndex[r]].push_back(t);
  }
  const auto nu = static_cast<uint32_t>(members.size());
  // Dependency order: Kahn over the set graph, ties by the lowest member track index (the set's index order).
  std::vector<std::vector<uint32_t>> adj(nu);
  std::vector<uint32_t> indeg(nu, 0);
  for (const ProjectWire& w : g.wires) {
    const uint32_t a = setIndex[uf.find(w.from.track)], b = setIndex[uf.find(w.to.track)];
    if (a == b || std::find(adj[a].begin(), adj[a].end(), b) != adj[a].end()) continue;
    adj[a].push_back(b);
    ++indeg[b];
  }
  std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
  for (uint32_t u = 0; u < nu; ++u)
    if (indeg[u] == 0) ready.push(u);
  std::vector<uint32_t> order, position(nu, kNone);
  while (!ready.empty()) {
    const uint32_t u = ready.top();
    ready.pop();
    position[u] = static_cast<uint32_t>(order.size());
    order.push_back(u);
    for (uint32_t v : adj[u])
      if (--indeg[v] == 0) ready.push(v);
  }
  if (order.size() != nu) return projectError(ErrorCode::Unknown, "internal: the unit graph still has a cycle");

  ProjectPartition part;
  part.notes = std::move(notes);
  part.unitOf.assign(nt, kNone);
  for (uint32_t u : order) {
    ProjectUnit unit;
    unit.tracks = members[u];
    std::sort(unit.tracks.begin(), unit.tracks.end(),
              [&](uint32_t a, uint32_t b) { return g.tracks[a].id < g.tracks[b].id; });
    for (uint32_t t : unit.tracks) {
      part.unitOf[t] = position[u];
      if (g.tracks[t].role == TrackRole::Instrument && unit.voiceTrack == kNone) unit.voiceTrack = t;
      if (!unit.name.empty()) unit.name += '+';
      unit.name += g.tracks[t].name.empty() ? "t" + std::to_string(g.tracks[t].id) : g.tracks[t].name;
    }
    part.units.push_back(std::move(unit));
  }
  // Buses, in first-use order (wires, then the output).
  auto busFor = [&](const ProjectPort& src) {
    for (uint32_t b = 0; b < part.buses.size(); ++b)
      if (part.buses[b].source == src) return b;
    ProjectBus bus;
    bus.source = src;
    bus.producer = part.unitOf[src.track];
    part.buses.push_back(std::move(bus));
    return static_cast<uint32_t>(part.buses.size() - 1);
  };
  for (const ProjectWire& w : g.wires) {
    const uint32_t a = part.unitOf[w.from.track], b = part.unitOf[w.to.track];
    if (a == b) continue;
    ProjectBus& bus = part.buses[busFor(w.from)];
    if (std::find(bus.readers.begin(), bus.readers.end(), b) == bus.readers.end()) bus.readers.push_back(b);
  }
  part.outputBus = busFor(g.output);
  part.buses[part.outputBus].output = true;
  for (ProjectBus& bus : part.buses) std::sort(bus.readers.begin(), bus.readers.end());
  return part;
}

const Patch& crossTrackHelper() {
  // `in x : global` → `out y : global`: flattened away for a global signal, a voice_sum for a per-voice one.
  static const Patch helper = [] {
    Patch h("pqCrossTrack");
    const PortType t = portType(Kind::Infer, Rate::Infer, Poly::Global, kWidthInfer);
    const NodeIndex x = h.addInput("x", t);
    const NodeIndex y = h.addOutput("y", t);
    h.connect(x, y, 0);
    return h;
  }();
  return helper;
}

}  // namespace

std::string busName(uint32_t bus) { return "bus" + std::to_string(bus); }

uint32_t parseBusName(std::string_view name) noexcept {
  if (name.size() < 4 || name.substr(0, 3) != "bus") return kNone;
  uint32_t v = 0;
  const char* b = name.data() + 3;
  const char* e = name.data() + name.size();
  const auto r = std::from_chars(b, e, v);
  if (r.ec != std::errc{} || r.ptr != e || v == kNone) return kNone;
  return v;
}

Result<ProjectPartition> partitionProject(const ProjectGraph& g) {
  if (Status s = validateGraph(g); !s.ok()) return s.error();
  const auto nt = static_cast<uint32_t>(g.tracks.size());
  UnionFind uf(nt);
  std::vector<std::string> notes;
  // 2. Sidechain / modulation wires merge their tracks (SPEC §15), never two instruments.
  for (const ProjectWire& w : g.wires) {
    if (w.kind == WireKind::Audio) continue;
    const uint32_t a = uf.find(w.from.track), b = uf.find(w.to.track);
    if (a == b) continue;
    const std::string what = (w.kind == WireKind::Sidechain ? "sidechain " : "modulation ") +
                             trackText(g, w.from.track) + " -> " + trackText(g, w.to.track);
    if (instruments(g, uf, a) + instruments(g, uf, b) > 1) {
      notes.push_back(what + ": not merged (two instruments cannot share one image's voices); fed forward through a bus");
      continue;
    }
    UnionFind trial = uf;
    trial.unite(a, b);
    if (cycleWithInstruments(g, trial)) {
      notes.push_back(what + ": not merged (the merge would close a cycle through two instruments); fed forward through a bus");
      continue;
    }
    uf = std::move(trial);
  }
  // 3. Cycles between units merge (SPEC §7.4). The condensation of a graph is acyclic, so one pass suffices.
  {
    std::vector<uint32_t> setIndex(nt, kNone), roots;
    for (uint32_t t = 0; t < nt; ++t) {
      const uint32_t r = uf.find(t);
      if (setIndex[r] == kNone) {
        setIndex[r] = static_cast<uint32_t>(roots.size());
        roots.push_back(r);
      }
    }
    const auto nu = static_cast<uint32_t>(roots.size());
    std::vector<std::vector<uint32_t>> adj(nu);
    for (const ProjectWire& w : g.wires) {
      const uint32_t a = setIndex[uf.find(w.from.track)], b = setIndex[uf.find(w.to.track)];
      if (a != b) adj[a].push_back(b);
    }
    uint32_t count = 0;
    const std::vector<uint32_t> comp = sccs(nu, adj, count);
    std::vector<std::vector<uint32_t>> byComp(count);
    for (uint32_t u = 0; u < nu; ++u) byComp[comp[u]].push_back(u);
    for (const auto& c : byComp) {
      if (c.size() < 2) continue;
      uint32_t inst = 0;
      std::string names;
      for (uint32_t u : c) {
        inst += instruments(g, uf, roots[u]);
        for (uint32_t t = 0; t < nt; ++t)
          if (uf.find(t) == roots[u]) names += (names.empty() ? "" : ", ") + trackText(g, t);
      }
      if (inst > 1)
        return projectError(ErrorCode::Unsupported,
                            "a cycle between tracks with more than one instrument cannot be one unit: " + names);
      for (size_t i = 1; i < c.size(); ++i) uf.unite(roots[c[0]], roots[c[i]]);
      notes.push_back("cycle merged into one unit: " + names);
    }
  }
  return buildPartition(g, uf, std::move(notes));
}

Result<ProjectPartition> wholeProject(const ProjectGraph& g) {
  if (Status s = validateGraph(g); !s.ok()) return s.error();
  UnionFind uf(static_cast<uint32_t>(g.tracks.size()));
  for (uint32_t t = 1; t < g.tracks.size(); ++t) uf.unite(0, t);
  return buildPartition(g, uf, {});
}

Result<UnitPatch> composeUnit(const ProjectGraph& g, const ProjectPartition& part, uint32_t unitIndex,
                              std::span<const PortType> busTypes) {
  if (unitIndex >= part.units.size()) return projectError(ErrorCode::InvalidArgument, "no unit " + std::to_string(unitIndex));
  const ProjectUnit& unit = part.units[unitIndex];
  UnitPatch up;
  up.patch = Patch("pqUnit");
  up.voices = unit.voiceTrack != kNone;
  if (up.voices) up.eventSlot = g.tracks[unit.voiceTrack].eventSlot;
  Patch& P = up.patch;

  // Library: the member patches (unique names), the helper, the project's callees.
  for (uint32_t t : unit.tracks) {
    const Patch& tp = *g.tracks[t].patch;
    if (const Patch* other = up.library.find(tp.name()); other != nullptr && other != &tp)
      return projectError(ErrorCode::InvalidArgument, "two tracks of unit '" + unit.name + "' have different patches named '" +
                                                          tp.name() + "'");
    up.library.add(tp);
  }
  up.library.add(crossTrackHelper());
  if (g.library != nullptr) up.library.addAll(*g.library);

  // Call nodes: NodeId = track id (members ascending by id, nothing added before).
  std::vector<NodeIndex> call(g.tracks.size(), kNone);
  std::vector<SubcircuitRef> sig(g.tracks.size());
  for (uint32_t t : unit.tracks) {
    const ProjectTrack& tr = g.tracks[t];
    sig[t] = subcircuitSignature(*tr.patch);
    P.reserveIdsThrough(NodeId{tr.id - 1});
    const NodeIndex n = P.addSubcircuit(sig[t]);
    if (P.node(n).id.value != tr.id) return projectError(ErrorCode::Unknown, "internal: call node id");
    P.setNodeName(n, "t" + std::to_string(tr.id));
    call[t] = n;
  }
  const SubcircuitRef helperSig = subcircuitSignature(crossTrackHelper());

  // Every source value used: a member's port through the helper (one per port), or a root `in bus<k>`.
  std::map<std::pair<uint32_t, std::string>, PortRef> sourceValue;
  std::vector<uint32_t> inBuses;
  for (const ProjectWire& w : g.wires)
    if (part.unitOf[w.to.track] == unitIndex && part.unitOf[w.from.track] != unitIndex) {
      uint32_t b = kNone;
      for (uint32_t k = 0; k < part.buses.size(); ++k)
        if (part.buses[k].source == w.from) b = k;
      if (b == kNone) return projectError(ErrorCode::Unknown, "internal: no bus for a cross-unit wire");
      if (std::find(inBuses.begin(), inBuses.end(), b) == inBuses.end()) inBuses.push_back(b);
    }
  std::sort(inBuses.begin(), inBuses.end());
  for (uint32_t b : inBuses) {
    if (b >= busTypes.size() || busTypes[b].kind == Kind::Infer)
      return projectError(ErrorCode::InvalidArgument, "the type of " + busName(b) + " is not known yet (compile its producer first)");
    const NodeIndex in = P.addInput(busName(b), portType(busTypes[b].kind, Rate::Audio, Poly::Global, busTypes[b].width));
    sourceValue[{part.buses[b].source.track, part.buses[b].source.port}] = PortRef{in, 0};
    up.inBus.push_back(b);
  }
  auto valueOf = [&](const ProjectPort& src) -> PortRef {
    const auto key = std::make_pair(src.track, src.port);
    if (const auto it = sourceValue.find(key); it != sourceValue.end()) return it->second;
    const uint16_t o = *findPort(sig[src.track].outputs, src.port);
    const NodeIndex h = P.addSubcircuit(helperSig);
    P.connect(PortRef{call[src.track], o}, PortRef{h, 0});
    return sourceValue[key] = PortRef{h, 0};
  };

  // Wires into members, summed per target port in wire order.
  std::map<std::pair<uint32_t, std::string>, std::vector<PortRef>> targets;
  std::vector<std::pair<uint32_t, std::string>> targetOrder;
  for (const ProjectWire& w : g.wires) {
    if (part.unitOf[w.to.track] != unitIndex) continue;
    const auto key = std::make_pair(w.to.track, w.to.port);
    auto& list = targets[key];
    if (list.empty()) targetOrder.push_back(key);
    list.push_back(valueOf(w.from));
  }
  const AtomId add = atomId("add");
  for (const auto& key : targetOrder) {
    const auto& list = targets[key];
    PortRef acc = list[0];
    for (size_t i = 1; i < list.size(); ++i) {
      const NodeIndex a = P.addAtom(add);
      P.connect(acc, PortRef{a, 0});
      P.connect(list[i], PortRef{a, 1});
      acc = PortRef{a, 0};
    }
    const uint16_t in = *findPort(sig[key.first].inputs, key.second);
    P.connect(acc, PortRef{call[key.first], in});
  }

  // Buses this unit writes.
  for (uint32_t b = 0; b < part.buses.size(); ++b) {
    if (part.buses[b].producer != unitIndex) continue;
    const NodeIndex out = P.addOutput(busName(b), portType(Kind::Infer, Rate::Infer, Poly::Global, kWidthInfer));
    P.connect(valueOf(part.buses[b].source), PortRef{out, 0});
    up.outBus.push_back(b);
  }

  // Voice monitors of every instrument member (one in a per-track unit; the whole project may have several: their
  // names then carry the track id).
  uint32_t numInstruments = 0;
  for (uint32_t t : unit.tracks) numInstruments += g.tracks[t].role == TrackRole::Instrument ? 1u : 0u;
  for (uint32_t t : unit.tracks) {
    if (g.tracks[t].role != TrackRole::Instrument) continue;
    for (uint16_t o = 0; o < sig[t].outputs.size(); ++o) {
      const SubcircuitPort& port = sig[t].outputs[o];
      if (port.type.poly == Poly::Global) continue;
      std::string name = port.name == "done" ? std::string("done") : "vmon_" + port.name;
      if (numInstruments > 1) name += "_t" + std::to_string(g.tracks[t].id);
      const NodeIndex ex = P.addExport(name);
      P.connect(PortRef{call[t], o}, PortRef{ex, 0});
    }
  }
  return up;
}

PortType busType(const TypedPatch& typed, uint32_t bus) {
  const std::string name = busName(bus);
  const Patch& p = typed.patch;
  for (NodeIndex n = 0; n < p.nodeCount(); ++n)
    if (p.node(n).kind == NodeKind::Output && p.nodeName(n) == name) return typed.inputType(n, 0);
  return PortType{};
}

namespace {

struct Hasher {
  uint64_t h = 0xCBF29CE484222325ull;
  void bytes(const void* p, size_t n) noexcept {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001B3ull;
  }
  void u64(uint64_t v) noexcept { bytes(&v, sizeof v); }
  void str(std::string_view s) noexcept {
    u64(s.size());
    bytes(s.data(), s.size());
  }
  void constant(const Constant& c) noexcept {
    u64(static_cast<uint64_t>(c.kind) | static_cast<uint64_t>(c.unit) << 8);
    u64(std::bit_cast<uint64_t>(c.literal));
  }
  void type(const PortType& t) noexcept {
    u64(static_cast<uint64_t>(t.kind) | static_cast<uint64_t>(t.rate) << 8 | static_cast<uint64_t>(t.poly) << 16 |
        static_cast<uint64_t>(t.width) << 24);
  }
};

}  // namespace

uint64_t unitHash(const TypedPatch& typed, uint64_t salt) noexcept {
  Hasher hs;
  hs.u64(salt);
  const Patch& p = typed.patch;
  hs.u64(p.nodeCount());
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    const Node& nd = p.node(n);
    hs.u64(static_cast<uint64_t>(nd.kind) | static_cast<uint64_t>(nd.atom) << 8 |
           static_cast<uint64_t>(nd.numInputs) << 24 | static_cast<uint64_t>(nd.numOutputs) << 40);
    hs.u64(n < typed.stateKey.size() ? typed.stateKey[n] : 0);
    switch (nd.kind) {
      case NodeKind::Const: hs.constant(p.constant(n)); break;
      case NodeKind::Param: {
        const ParamDecl& d = p.paramDecl(n);
        hs.u64(static_cast<uint64_t>(d.kind) | static_cast<uint64_t>(d.hasRange) << 8);
        hs.constant(d.defaultValue);
        hs.constant(d.rangeMin);
        hs.constant(d.rangeMax);
        hs.str(p.nodeName(n));
        break;
      }
      case NodeKind::Input:
      case NodeKind::Output:
      case NodeKind::Export: hs.str(p.nodeName(n)); break;
      case NodeKind::Unknown: hs.str(p.nodeName(n)); hs.u64(nd.payload); break;
      case NodeKind::Equation: hs.u64(nd.payload); break;
      default: break;
    }
  }
  for (const PortType& t : typed.types) hs.type(t);
  hs.u64(p.wireCount());
  for (const Wire& w : p.wires()) {
    hs.u64(static_cast<uint64_t>(w.from.node) | static_cast<uint64_t>(w.from.port) << 32);
    hs.u64(static_cast<uint64_t>(w.to.node) | static_cast<uint64_t>(w.to.port) << 32);
  }
  for (const Solve& s : p.solves()) hs.u64(s.iterations);
  hs.u64(typed.durationsInSamples ? 1 : 0);
  return hs.h;
}

}  // namespace pq::graph
