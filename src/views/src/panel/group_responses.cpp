// EQ.4: groups' analytic responses for the displays (pq/views/panel/group_responses.h).

#include "pq/views/panel/group_responses.h"

#include <algorithm>

#include "pq/document/devices.h"
#include "pq/views/view_context.h"

namespace pq::views {

GroupResponses::GroupResponses(ViewContext& ctx) : ctx_(ctx) {}
GroupResponses::~GroupResponses() = default;

const doc::Snapshot& GroupResponses::doc() const { return ctx_.doc(); }
double GroupResponses::sampleRate() const noexcept { return ctx_.sampleRate(); }

void GroupResponses::sync() {
  if (revision_ == ctx_.structuralRevision()) return;
  revision_ = ctx_.structuralRevision();
  lowered_.reset();
  lowerError_.clear();
  lowerTried_ = false;
  entries_.clear();
  series_.clear();
}

GroupResponses::Entry* GroupResponses::find(doc::DeviceId g) noexcept {
  for (Entry& e : entries_)
    if (e.group == g) return &e;
  return nullptr;
}

void GroupResponses::want(std::span<const doc::DeviceId> groups) {
  sync();
  std::vector<doc::DeviceId> missing;
  for (const doc::DeviceId g : groups)
    if (g.valid() && find(g) == nullptr && std::find(missing.begin(), missing.end(), g) == missing.end()) missing.push_back(g);
  if (missing.empty()) return;
  const doc::Snapshot& snap = ctx_.doc();
  const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
  if (!lowerTried_ && snap && lib != nullptr) {
    lowerTried_ = true;
    doc::LowerOptions lo;
    lo.library = &lib->library();
    lo.devices = lib;
    lo.clipSlotOf = [](doc::TrackId) { return 0u; };
    auto low = doc::lower(*snap, lo);
    ++lowerings_;
    if (low) lowered_ = std::make_unique<doc::LoweredProject>(std::move(*low));
    else lowerError_ = low.error().message;
  }
  const size_t first = entries_.size();
  for (const doc::DeviceId g : missing) {
    Entry e;
    e.group = g;
    e.response = std::make_unique<doc::GroupResponse>();
    entries_.push_back(std::move(e));
  }
  if (!lowered_ || lib == nullptr) {
    for (size_t k = first; k < entries_.size(); ++k)
      entries_[k].error = lowerError_.empty() ? std::string("no document or device library") : lowerError_;
    return;
  }
  std::vector<doc::GroupResponse*> out;
  out.reserve(missing.size());
  for (size_t k = first; k < entries_.size(); ++k) out.push_back(entries_[k].response.get());
  const std::vector<Status> st = doc::GroupResponse::prepareMany(*snap, *lowered_, *lib, missing, out);
  for (size_t k = 0; k < st.size(); ++k) {
    Entry& e = entries_[first + k];
    if (!st[k]) e.error = st[k].error().message;
    else e.updated = snap;  // (prepare read the controls of this snapshot)
  }
}

doc::GroupResponse* GroupResponses::get(doc::DeviceId group) {
  sync();
  Entry* e = find(group);
  if (e == nullptr) {
    want(std::span<const doc::DeviceId>(&group, 1));
    e = find(group);
  }
  if (e == nullptr || !e->error.empty()) return nullptr;
  const doc::Snapshot& snap = ctx_.doc();
  if (snap && e->updated != snap) {
    e->response->update(*snap);
    e->updated = snap;
  }
  return e->response.get();
}

const GroupResponses::Curve* GroupResponses::curve(doc::DeviceId group, std::span<const double> hz, uint64_t key) {
  doc::GroupResponse* r = get(group);
  if (r == nullptr) return nullptr;
  Entry& e = *find(group);
  if (e.curve.version != 0 && e.curveStamp == r->stamp() && e.curveKey == key && e.curvePoints == hz.size()) return &e.curve;
  Curve& c = e.curve;
  c.in = std::max<uint16_t>(1, r->inChannels());
  c.out = std::max<uint16_t>(1, r->outChannels());
  c.h.resize(hz.size() * c.in * c.out);
  c.kind = r->evaluate(hz, sampleRate(), c.h);
  c.version = ++versions_;
  e.curveStamp = r->stamp();
  e.curveKey = key;
  e.curvePoints = hz.size();
  return &c;
}

const GroupResponses::Curve* GroupResponses::series(doc::DeviceId name, std::span<const doc::DeviceId> members,
                                                    std::span<const uint8_t> passes, std::span<const double> hz, uint64_t key) {
  std::vector<const Curve*>& cs = scratchCurves_;
  std::vector<uint64_t>& vs = scratchVersions_;
  cs.clear();
  vs.clear();
  for (size_t i = 0; i < members.size(); ++i) {
    const Curve* c = curve(members[i], hz, key);
    if (c == nullptr && !(i < passes.size() && passes[i] != 0)) return nullptr;
    if (c != nullptr) cs.push_back(c);
    vs.push_back(c != nullptr ? c->version : 0);
  }
  for (size_t i = 0; i + 1 < cs.size(); ++i)
    if (cs[i]->out != cs[i + 1]->in) return nullptr;
  Series* s = nullptr;
  for (Series& x : series_)
    if (x.name == name) s = &x;
  if (s == nullptr) {
    series_.push_back({});
    s = &series_.back();
    s->name = name;
  }
  Curve& out = s->curve;
  if (out.version != 0 && s->key == key && s->versions == vs && out.h.size() == hz.size() * out.in * out.out) return &out;
  s->versions = vs;
  s->key = key;
  out.in = cs.empty() ? 1 : cs.front()->in;
  out.out = cs.empty() ? 1 : cs.back()->out;
  out.kind = graph::ResponseKind::Exact;
  for (const Curve* c : cs)
    if (c->kind == graph::ResponseKind::Unavailable) out.kind = graph::ResponseKind::Unavailable;
    else if (c->kind == graph::ResponseKind::Approximate && out.kind == graph::ResponseKind::Exact) out.kind = graph::ResponseKind::Approximate;
  const size_t n = hz.size();
  out.h.assign(n * out.in * out.out, std::complex<double>(0.0));
  using Cx = std::complex<double>;
  if (cs.empty()) {
    for (size_t k = 0; k < n; ++k) out.h[k] = 1.0;  // (nothing in the series: In = Out)
  } else if (std::all_of(cs.begin(), cs.end(), [&](const Curve* c) { return c->in == out.in && c->out == out.in; }) && out.in <= 4) {
    // The usual chain: every member W × W (W = 1: mono, 2: stereo). In place, member after member, points inner.
    const size_t w = out.in, ww = w * w;
    std::copy(cs.front()->h.begin(), cs.front()->h.begin() + static_cast<std::ptrdiff_t>(n * ww), out.h.begin());
    for (size_t m = 1; m < cs.size(); ++m) {
      const Cx* hm = cs[m]->h.data();
      Cx* acc = out.h.data();
      if (w == 1) {
        for (size_t k = 0; k < n; ++k) acc[k] *= hm[k];
        continue;
      }
      for (size_t k = 0; k < n; ++k, acc += ww, hm += ww)
        for (size_t i = 0; i < w; ++i) {
          Cx row[4];
          for (size_t j = 0; j < w; ++j) {
            Cx sum = acc[i * w] * hm[j];
            for (size_t x = 1; x < w; ++x) sum += acc[i * w + x] * hm[x * w + j];
            row[j] = sum;
          }
          for (size_t j = 0; j < w; ++j) acc[i * w + j] = row[j];
        }
    }
  } else {
    // Any channel counts that chain: per point acc (in × width) · H (width × out'), member after member.
    std::vector<Cx>& acc = scratchH_;
    std::vector<Cx>& next = scratchNext_;
    for (size_t k = 0; k < n; ++k) {
      const Curve& c0 = *cs.front();
      acc.assign(c0.h.begin() + static_cast<std::ptrdiff_t>(k * c0.in * c0.out), c0.h.begin() + static_cast<std::ptrdiff_t>((k + 1) * c0.in * c0.out));
      size_t width = c0.out;
      for (size_t m = 1; m < cs.size(); ++m) {
        const Curve& c = *cs[m];
        next.resize(static_cast<size_t>(out.in) * c.out);
        for (size_t i = 0; i < out.in; ++i)
          for (size_t j = 0; j < c.out; ++j) {
            Cx sum = 0.0;
            for (size_t x = 0; x < width; ++x) sum += acc[i * width + x] * c.at(k, static_cast<uint16_t>(x), static_cast<uint16_t>(j));
            next[i * c.out + j] = sum;
          }
        width = c.out;
        acc.swap(next);
      }
      std::copy(acc.begin(), acc.end(), out.h.begin() + static_cast<std::ptrdiff_t>(k * out.in * out.out));
    }
  }
  out.version = ++versions_;
  return &out;
}

std::string GroupResponses::why(doc::DeviceId group) const {
  for (const Entry& e : entries_)
    if (e.group == group) return e.error;
  return "not asked for";
}

}  // namespace pq::views
