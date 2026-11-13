// EQ.2: the analytic frequency response of a typed graph (pq/graph/response.h).

#include "pq/graph/response.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

#include "fold.h"
#include "response_rules.h"
#include "pq/dsp/kernel.h"
#include "pq/graph/kernel_map.h"

namespace pq::graph {

namespace {

using Cx = std::complex<double>;

// Whether registry input `i` of `atom` is a duration the language writes in seconds (the kernel takes samples).
bool isDuration(AtomId atom, uint32_t i) noexcept {
  const KernelMapping& km = kernelMapping(atom);
  if (km.args.empty()) return false;
  const ArgUse u = km.args[std::min<size_t>(i, km.args.size() - 1)].use;
  return u == ArgUse::InputSeconds || u == ArgUse::InitMaxDelaySeconds;
}

}  // namespace

void LinearResponse::prepare(const TypedPatch& typed, ResponseEnd source, ResponseEnd sink, std::span<const uint32_t> settable) {
  *this = LinearResponse{};
  const Patch& p = typed.patch;
  durationsInSamples_ = typed.durationsInSamples;
  const uint32_t nodes = p.nodeCount(), ports = p.portCount();
  if (typed.types.size() != ports) return;
  Graph g(typed);
  const std::vector<NodeIndex>& owner = g.owner;
  auto driverPort = [&](NodeIndex n, uint32_t i) { return g.driverPort(n, i); };
  bool srcOk = false, sinkOk = false;
  const uint32_t srcPort = g.resolve(source, srcOk, srcWidth_);
  const uint32_t sinkPort = g.resolve(sink, sinkOk, sinkWidth_);
  if (!srcOk || !sinkOk || srcPort == kNone) return;  // Unavailable
  if (srcPort == sinkPort) {
    identity_ = true;
    prepared_ = ResponseKind::Exact;
    return;
  }
  // TX.mod-bands: the set ports' nodes (g.setOut[n]: 1 + the set output; 0: none). The source's own node is never cut.
  g.cut(settable, owner[srcPort]);

  // ---- signal: forward from the source (its own inputs cut); relevance: back from the sink through signal ports ----
  std::vector<uint8_t> sig, rel, relNode;
  g.signalFrom(srcPort, sig);
  if (sinkPort == kNone || sig[sinkPort] == 0) {
    zero_ = true;
    prepared_ = ResponseKind::Exact;
    return;
  }
  g.relevantTo(sinkPort, srcPort, sig, rel, relNode);
  for (NodeIndex n = 0; n < nodes; ++n) {
    if (relNode[n] == 0) continue;
    const Node& node = p.node(n);
    if (node.kind != NodeKind::Atom || atomSignature(node.atom).response == nullptr) return;  // Unavailable
  }
  // TX.group-oversample: each node runs at g.rateOf(n) (log2 of its oversample region's factor; 0: the base rate). The
  // converters are evaluated at the base rate (their rules are the half-band cascades at the base omega).

  // ---- unknowns: one per (relevant port, channel) ----
  std::vector<uint32_t> varBase(ports, kNone);
  uint32_t vars = 0;
  for (uint32_t f = 0; f < ports; ++f)
    if (rel[f] != 0) {
      varBase[f] = vars;
      vars += g.width(f);
    }
  for (uint16_t s = 0; s < srcWidth_; ++s) srcVar_.push_back(varBase[srcPort] + s);
  for (uint16_t j = 0; j < sinkWidth_; ++j) sinkVar_.push_back(varBase[sinkPort] + std::min<uint32_t>(j, g.width(sinkPort) - 1u));

  // ---- control values: what relevant nodes read besides the signal, in dependency order ----
  beginControls(g);
  for (NodeIndex n = 0; n < nodes; ++n) {
    if (relNode[n] == 0) continue;
    for (uint32_t i = 0; i < p.node(n).numInputs; ++i) {
      const uint32_t d = driverPort(n, i);
      if (d != kNone && sig[d] == 0) needControl(g, owner[d]);
    }
  }
  endControls();
  // An `smp` literal wired straight into a duration is samples already (optimise()'s rule, QUESTIONS Q35).
  auto smpLiteral = [&](uint32_t port) {
    if (port == kNone) return false;
    const NodeIndex o = owner[port];
    return p.node(o).kind == NodeKind::Const && p.constant(o).unit == Unit::smp;
  };

  // ---- cells and terms ----
  std::vector<std::pair<uint32_t, Term>> edges;  // (unknown, term)
  uint32_t argCount = 0;
  size_t cacheCount = 0;  // doubles
  gain_.assign(kRates, Cx{});  // gain_[r]: z at the current point at rate r (an oversample region; the z1 terms)
  for (NodeIndex n = 0; n < nodes; ++n) {
    if (relNode[n] == 0) continue;
    const Node& node = p.node(n);
    const AtomSignature& s = atomSignature(node.atom);
    const bool isChannel = s.name == "channel", isPack = s.name == "pack";
    const uint32_t inBegin = static_cast<uint32_t>(ins_.size());
    for (uint32_t i = 0; i < node.numInputs; ++i) {
      InSrc src;
      src.port = driverPort(n, i);
      src.def = g.inputDefault(n, i);
      src.width = src.port != kNone ? g.width(src.port) : 1;
      src.signal = src.port != kNone && sig[src.port] != 0 ? 1 : 0;
      src.toSamples = isDuration(node.atom, i) && !durationsInSamples_ && !smpLiteral(src.port) ? 1 : 0;
      ins_.push_back(src);
    }
    uint16_t width = 1;
    for (uint32_t o = 0; o < node.numOutputs; ++o) width = std::max(width, g.width(p.flatOutput(n, o)));
    // `channel`'s index: a const (infer()); read from its literal.
    uint32_t channelIndex = 0;
    if (isChannel) {
      const uint32_t d = node.numInputs > 1 ? driverPort(n, 1) : kNone;
      if (d != kNone && p.node(owner[d]).kind != NodeKind::Const) return;  // Unavailable: not a literal index
      channelIndex = d != kNone ? static_cast<uint32_t>(std::max(0.0, p.constant(owner[d]).value())) : 0u;
    }
    for (uint16_t c = 0; c < width; ++c) {
      Cell cell;
      cell.rule = s.response;
      cell.rate = g.rateOf(n);
      usedRates_ |= static_cast<uint8_t>(1u << cell.rate);
      cell.inBegin = inBegin;
      cell.inCount = node.numInputs;
      cell.numOutputs = node.numOutputs;
      cell.channel = c;
      cell.gainBase = static_cast<uint32_t>(gain_.size());
      gain_.resize(gain_.size() + static_cast<size_t>(node.numOutputs) * node.numInputs);
      cell.argBase = argCount;
      argCount += node.numInputs;
      cell.cacheBase = static_cast<uint32_t>(cacheCount);  // EQ.1b: the rule's scratch for one evaluate
      cacheCount += (s.responseCacheBytes + sizeof(double) - 1) / sizeof(double);
      // CPU: z1 is the commonest atom whose gain depends on the frequency; its term reads the one shared slot that
      // holds z (set once per point) instead of calling the rule per point.
      cell.zOnly = s.response == &rules::z1;
      cells_.push_back(cell);
      for (uint32_t o = 0; o < node.numOutputs; ++o) {
        const uint32_t f = p.flatOutput(n, o);
        if (rel[f] == 0 || c >= g.width(f)) continue;
        const uint32_t v = varBase[f] + c;
        uint32_t packOffset = 0;
        for (uint32_t i = 0; i < node.numInputs; ++i) {
          const InSrc& in = ins_[inBegin + i];
          uint32_t ic;
          if (isPack) {
            const uint32_t lo = packOffset;
            packOffset += in.width;
            if (c < lo || c >= packOffset) continue;
            ic = c - lo;
          } else if (isChannel) {
            if (i != 0) continue;
            ic = in.width == 1 ? 0u : std::min<uint32_t>(channelIndex, in.width - 1u);
          } else {
            ic = in.width == 1 ? 0u : std::min<uint32_t>(c, in.width - 1u);
          }
          if (in.signal == 0) continue;
          edges.push_back({v, Term{varBase[in.port] + ic, cell.zOnly ? uint32_t{cell.rate} : cell.gainBase + o * node.numInputs + i}});
        }
      }
    }
  }
  // CSR by unknown.
  termBegin_.assign(vars + 1, 0);
  for (const auto& e : edges) ++termBegin_[e.first + 1];
  for (uint32_t v = 0; v < vars; ++v) termBegin_[v + 1] += termBegin_[v];
  terms_.resize(edges.size());
  {
    std::vector<uint32_t> fill(termBegin_.begin(), termBegin_.end() - 1);
    for (const auto& e : edges) terms_[fill[e.first]++] = e.second;
  }

  // ---- strongly connected components (iterative Tarjan; emitted dependencies first) ----
  constexpr uint32_t kUnset = kNone;
  std::vector<uint32_t> index(vars, kUnset), low(vars, 0), tstack;
  std::vector<uint8_t> onStack(vars, 0);
  std::vector<std::pair<uint32_t, uint32_t>> call;  // (unknown, next term)
  uint32_t counter = 0;
  sccOf_.assign(vars, 0);
  localOf_.assign(vars, 0);
  sccBegin_.push_back(0);
  uint32_t maxLoop = 0;
  for (uint32_t root = 0; root < vars; ++root) {
    if (index[root] != kUnset) continue;
    call.push_back({root, termBegin_[root]});
    index[root] = low[root] = counter++;
    tstack.push_back(root);
    onStack[root] = 1;
    while (!call.empty()) {
      auto& [v, t] = call.back();
      if (t < termBegin_[v + 1]) {
        const uint32_t u = terms_[t++].from;
        if (index[u] == kUnset) {
          index[u] = low[u] = counter++;
          tstack.push_back(u);
          onStack[u] = 1;
          call.push_back({u, termBegin_[u]});
        } else if (onStack[u] != 0) {
          low[v] = std::min(low[v], index[u]);
        }
        continue;
      }
      const uint32_t done = v;
      call.pop_back();
      if (!call.empty()) low[call.back().first] = std::min(low[call.back().first], low[done]);
      if (low[done] != index[done]) continue;
      const uint32_t comp = static_cast<uint32_t>(sccBegin_.size()) - 1u;
      uint32_t w;
      do {
        w = tstack.back();
        tstack.pop_back();
        onStack[w] = 0;
        sccOf_[w] = comp;
        localOf_[w] = static_cast<uint32_t>(sccVars_.size()) - sccBegin_.back();
        sccVars_.push_back(w);
      } while (w != done);
      sccBegin_.push_back(static_cast<uint32_t>(sccVars_.size()));
      const uint32_t size = sccBegin_.back() - sccBegin_[comp];
      bool loop = size > 1;
      if (!loop)
        for (uint32_t k = termBegin_[done]; k < termBegin_[done + 1]; ++k) loop |= terms_[k].from == done;
      sccLoop_.push_back(loop ? 1 : 0);
      if (loop) maxLoop = std::max(maxLoop, size);
    }
  }

  valRe_.assign(static_cast<size_t>(vars) * srcWidth_ * kBatch, 0.0);
  valIm_.assign(valRe_.size(), 0.0);
  gainRe_.assign(gain_.size() * kBatch, 0.0);
  gainIm_.assign(gainRe_.size(), 0.0);
  mat_.assign(static_cast<size_t>(maxLoop) * (maxLoop + srcWidth_), Cx{});
  dynamicCells_.assign(cells_.size(), 0);
  argValue_.assign(argCount, 0.0f);
  cache_.assign(cacheCount, 0.0);
  argKnown_.assign(argCount, 0);
  argSignal_.assign(argCount, 0);
  prepared_ = ResponseKind::Exact;
}

ResponseKind LinearResponse::firstCall(uint32_t c, const Point& pt, double sampleRate, bool& dynamic) noexcept {
  const Cell& cell = cells_[c];
  const double cellRate = sampleRate * static_cast<double>(1u << cell.rate);  // (a region's durations count its samples)
  for (uint32_t i = 0; i < cell.inCount; ++i) {
    const InSrc& s = ins_[cell.inBegin + i];
    argSignal_[cell.argBase + i] = s.signal;
    float v = 0.0f;
    const bool known = s.signal == 0 && readControl(s, cell.channel, v);
    argKnown_[cell.argBase + i] = known ? 1 : 0;
    argValue_[cell.argBase + i] = known && s.toSamples != 0 ? static_cast<float>(v * cellRate) : v;
  }
  std::fill_n(gain_.data() + cell.gainBase, static_cast<size_t>(cell.numOutputs) * cell.inCount, Cx{});
  return recall(c, pt, sampleRate, dynamic, true);
}

ResponseKind LinearResponse::recall(uint32_t c, const Point& pt, double sampleRate, bool& dynamic, bool fresh) noexcept {
  const Cell& cell = cells_[c];
  Cx* g = gain_.data() + cell.gainBase;
  ResponseArgs a;
  a.omega = pt.omega[cell.rate];
  a.z = pt.z[cell.rate];
  a.ejw1 = pt.ejw1[cell.rate];
  a.sampleRate = sampleRate * static_cast<double>(1u << cell.rate);
  a.numInputs = cell.inCount;
  a.numOutputs = cell.numOutputs;
  a.value = argValue_.data() + cell.argBase;
  a.known = argKnown_.data() + cell.argBase;
  a.signal = argSignal_.data() + cell.argBase;
  a.gain = g;
  a.cache = cache_.data() + cell.cacheBase;
  a.fresh = fresh;
  const ResponseKind k = cell.rule(a);
  dynamic = a.dependsOnOmega;
  return k;
}

void LinearResponse::solveBatch(uint32_t first, uint32_t count, std::span<Cx> out) noexcept {
  constexpr uint32_t P = kBatch;
  const uint32_t W = srcWidth_;
  const uint32_t comps = static_cast<uint32_t>(sccBegin_.size()) - 1u;
  const uint32_t srcBase = srcVar_.front();
  double* vr = valRe_.data();
  double* vi = valIm_.data();
  const double* gr = gainRe_.data();
  const double* gi = gainIm_.data();
  for (uint32_t comp = 0; comp < comps; ++comp) {
    const uint32_t b = sccBegin_[comp], e = sccBegin_[comp + 1];
    if (sccLoop_[comp] == 0) {
      // CPU: points are the lanes (struct of arrays, kBatch doubles per value), so each term is one vectorised
      // complex multiply-add over the batch.
      const uint32_t v = sccVars_[b];
      for (uint32_t s = 0; s < W; ++s) {
        const double inj = v == srcBase + s ? 1.0 : 0.0;
        alignas(64) double yr[P], yi[P];  // local accumulators: no aliasing with the inputs, kept in registers
        for (uint32_t p = 0; p < P; ++p) {
          yr[p] = inj;
          yi[p] = 0.0;
        }
        for (uint32_t t = termBegin_[v]; t < termBegin_[v + 1]; ++t) {
          const double* ar = gr + static_cast<size_t>(terms_[t].gain) * P;
          const double* ai = gi + static_cast<size_t>(terms_[t].gain) * P;
          const double* xr = vr + (static_cast<size_t>(terms_[t].from) * W + s) * P;
          const double* xi = vi + (static_cast<size_t>(terms_[t].from) * W + s) * P;
          for (uint32_t p = 0; p < P; ++p) {
            yr[p] += ar[p] * xr[p] - ai[p] * xi[p];
            yi[p] += ar[p] * xi[p] + ai[p] * xr[p];
          }
        }
        std::copy_n(yr, P, vr + (static_cast<size_t>(v) * W + s) * P);
        std::copy_n(yi, P, vi + (static_cast<size_t>(v) * W + s) * P);
      }
      continue;
    }
    // A loop: (I − A) y = r over its k unknowns, r from what is already known; per point.
    const uint32_t k = e - b, cols = k + W;
    for (uint32_t p = 0; p < count; ++p) {
      Cx* m = mat_.data();
      std::fill_n(m, static_cast<size_t>(k) * cols, Cx{});
      for (uint32_t a = 0; a < k; ++a) {
        Cx* row = m + static_cast<size_t>(a) * cols;
        row[a] = 1.0;
        const uint32_t v = sccVars_[b + a];
        for (uint32_t t = termBegin_[v]; t < termBegin_[v + 1]; ++t) {
          const uint32_t u = terms_[t].from;
          const size_t gs = static_cast<size_t>(terms_[t].gain) * P + p;
          const Cx g{gr[gs], gi[gs]};
          if (sccOf_[u] == comp) {
            row[localOf_[u]] -= g;
          } else {
            for (uint32_t s = 0; s < W; ++s) {
              const size_t xs = (static_cast<size_t>(u) * W + s) * P + p;
              row[k + s] += g * Cx{vr[xs], vi[xs]};
            }
          }
        }
      }
      bool singular = false;
      for (uint32_t col = 0; col < k && !singular; ++col) {
        uint32_t piv = col;
        double best = std::abs(m[static_cast<size_t>(col) * cols + col]);
        for (uint32_t r = col + 1; r < k; ++r) {
          const double a = std::abs(m[static_cast<size_t>(r) * cols + col]);
          if (a > best) {
            best = a;
            piv = r;
          }
        }
        if (!(best > 1e-300)) {
          singular = true;
          break;
        }
        if (piv != col)
          std::swap_ranges(m + static_cast<size_t>(piv) * cols, m + static_cast<size_t>(piv + 1) * cols,
                           m + static_cast<size_t>(col) * cols);
        const Cx* prow = m + static_cast<size_t>(col) * cols;
        const Cx inv = 1.0 / prow[col];
        for (uint32_t r = col + 1; r < k; ++r) {
          Cx* row = m + static_cast<size_t>(r) * cols;
          const Cx f = row[col] * inv;
          if (f == Cx{}) continue;
          for (uint32_t j = col; j < cols; ++j) row[j] -= f * prow[j];
        }
      }
      for (uint32_t a = k; a-- > 0;) {
        const uint32_t v = sccVars_[b + a];
        const Cx* row = m + static_cast<size_t>(a) * cols;
        for (uint32_t s = 0; s < W; ++s) {
          Cx y{std::numeric_limits<double>::infinity(), 0.0};
          if (!singular) {
            Cx acc = row[k + s];
            for (uint32_t j = a + 1; j < k; ++j) {
              const size_t xs = (static_cast<size_t>(sccVars_[b + j]) * W + s) * P + p;
              acc -= row[j] * Cx{vr[xs], vi[xs]};
            }
            y = acc / row[a];
          }
          const size_t ys = (static_cast<size_t>(v) * W + s) * P + p;
          vr[ys] = y.real();
          vi[ys] = y.imag();
        }
      }
    }
  }
  const uint32_t S = sinkWidth_;
  for (uint32_t p = 0; p < count; ++p)
    for (uint32_t s = 0; s < W; ++s)
      for (uint32_t j = 0; j < S; ++j) {
        const size_t xs = (static_cast<size_t>(sinkVar_[j]) * W + s) * P + p;
        out[(static_cast<size_t>(first + p) * W + s) * S + j] = Cx{vr[xs], vi[xs]};
      }
}

void LinearResponse::storeGains(uint32_t c, uint32_t p) noexcept {
  const Cell& cell = cells_[c];
  const uint32_t n = cell.numOutputs * cell.inCount;
  for (uint32_t i = 0; i < n; ++i) {
    const Cx g = gain_[cell.gainBase + i];
    gainRe_[static_cast<size_t>(cell.gainBase + i) * kBatch + p] = g.real();
    gainIm_[static_cast<size_t>(cell.gainBase + i) * kBatch + p] = g.imag();
  }
}

ResponseKind LinearResponse::evaluate(std::span<const double> hz, double sampleRate, std::span<Cx> out) noexcept {
  const size_t per = static_cast<size_t>(srcWidth_) * sinkWidth_;
  const size_t need = hz.size() * per;
  if (out.size() < need) return ResponseKind::Unavailable;
  auto fill = [&](Cx v) { std::fill_n(out.begin(), need, v); };
  const Cx nan{std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
  if (prepared_ == ResponseKind::Unavailable) {
    fill(nan);
    return ResponseKind::Unavailable;
  }
  if (zero_ || identity_) {
    fill(Cx{});
    if (identity_)
      for (size_t k = 0; k < hz.size(); ++k)
        for (uint32_t s = 0; s < std::min(srcWidth_, sinkWidth_); ++s)
          out[k * per + static_cast<size_t>(s) * sinkWidth_ + s] = 1.0;
    return ResponseKind::Exact;
  }
  if (hz.empty()) return prepared_;
  runControl(sampleRate);
  const double toOmega = 2.0 * std::numbers::pi / sampleRate;
  ResponseKind status = ResponseKind::Exact;
  dynamicCount_ = 0;
  // Each rate's omega, z and e^jw - 1 at a point (rate r: an oversample region at x2^r sees omega / 2^r).
  auto pointAt = [&](double w, Point& pt) {
    for (uint32_t r = 0; r < kRates; ++r) {
      if ((usedRates_ >> r & 1u) == 0) continue;
      const double wr = w / static_cast<double>(1u << r);
      pt.omega[r] = wr;
      pt.z[r] = std::polar(1.0, -wr);
      pt.ejw1[r] = dsp::svfEjwMinus1(wr);
    }
  };
  Point p0;
  pointAt(hz[0] * toOmega, p0);
  for (uint32_t c = 0; c < cells_.size(); ++c) {
    bool dyn = false;
    status = worse(status, firstCall(c, p0, sampleRate, dyn));
    if (cells_[c].zOnly) continue;
    if (dyn) dynamicCells_[dynamicCount_++] = c;
    else
      for (uint32_t p = 0; p < kBatch; ++p) storeGains(c, p);  // static: the same gains at every point
  }
  if (status == ResponseKind::Unavailable) {
    fill(nan);
    return status;
  }
  const uint32_t n = static_cast<uint32_t>(hz.size());
  for (uint32_t first = 0; first < n; first += kBatch) {
    const uint32_t count = std::min(kBatch, n - first);
    for (uint32_t p = 0; p < kBatch; ++p) {
      const uint32_t k = first + std::min(p, count - 1u);  // a partial batch repeats its last point
      Point pt;
      if (k == 0) pt = p0;
      else pointAt(hz[k] * toOmega, pt);  // (e^jw - 1 once per point and rate: the filters' rules share it)
      for (uint32_t r = 0; r < kRates; ++r) {  // slots 0 ... kRates - 1: z at each rate (the z1 terms)
        gainRe_[static_cast<size_t>(r) * kBatch + p] = pt.z[r].real();
        gainIm_[static_cast<size_t>(r) * kBatch + p] = pt.z[r].imag();
      }
      bool dyn = false;
      for (uint32_t d = 0; d < dynamicCount_; ++d) {
        status = worse(status, recall(dynamicCells_[d], pt, sampleRate, dyn));
        storeGains(dynamicCells_[d], p);
      }
    }
    solveBatch(first, count, out);
  }
  if (status == ResponseKind::Unavailable) fill(nan);
  return status;
}

ResponseKind linearResponse(const TypedPatch& typed, ResponseEnd source, ResponseEnd sink, std::span<const double> hz,
                            double sampleRate, std::vector<Cx>& out, std::span<const std::pair<NodeIndex, float>> params) {
  LinearResponse r;
  r.prepare(typed, source, sink);
  for (const auto& [node, v] : params)
    for (uint32_t i = 0; i < r.paramNodes().size(); ++i)
      if (r.paramNodes()[i] == node) r.setParam(i, v);
  out.assign(hz.size() * r.sourceChannels() * r.sinkChannels(), Cx{});
  return r.evaluate(hz, sampleRate, out);
}

}  // namespace pq::graph
