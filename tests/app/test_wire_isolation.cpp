// WI.2 (docs/plans/RB.md D1; ARCHITECTURE "Cut wires"): one bad wire never fails a unit. A compile whose type inference
// blames wires (ProjectCompiler::lastTypeFailures) cuts them (Refused / Type) and compiles again; the rest of the project
// plays exactly as the project without those wires. The two kinds of type error a document wire can make today (the
// lowering adapts widths at every input it wires — a wider source into a meta module's input takes the mid, across
// tracks channel 0 —, so no width error comes from one: Q?-WI.2-a):
//   rate   the Utility's audio into delay_fade's `fade` (an input bound to param rate: read once per block);
//   kind   a main-path link from an instrument (real) into an `and` block's main input (bool): cut, the path breaks there.
// An edit of a cut wire's track tries it again; a clean project never runs a second round (LinkStats::typeRetries).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "../support/blocks.h"
#include "pq/app/project_link.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/engine/offline_render.h"
#include "pq/views/view_context.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr double kRate = 48000.0;
using Channels = std::vector<std::vector<float>>;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

Channels render(ProjectLink& link, double seconds) {
  engine::BufferSink sink;
  auto st = link.renderOffline(0, static_cast<uint64_t>(std::llround(seconds * kRate)), sink);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  return sink.channels;
}

float peak(const Channels& c) {
  float p = 0.0f;
  for (const auto& ch : c)
    for (const float v : ch) p = std::max(p, std::fabs(v));
  return p;
}

// Track A: notes → instrument → Utility → mixer, a free delay_fade, the Utility's output cabled into its `fade` (rate).
// Track B: notes → instrument → and → mixer (kind: the link into `and`).
struct Song {
  DocumentStore store;
  TrackId a, b;
  DeviceId util, fade, gate;
  WireId rate, kind;
  Song() {
    a = track();
    b = track();
    util = test_blocks::appendAfter(store, a, "module:utility");
    fade = add(a, "atom:delay_fade", ops::kOffPath);
    gate = add(b, "atom:and", kAppend);
    const MainPath path = mainPath(*store.current(), *findTrackPtr(*store.current(), b));
    REQUIRE(path.indexOf(gate) >= 0);
    kind = path.links.at(static_cast<size_t>(path.indexOf(gate)));
    rate = must(ops::connectWire(*store.current(), store.ids(), util, "", fade, "fade")).wires.at(0);
  }
  TrackId track() {
    const TrackId t = must(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi)).tracks.at(0);
    std::vector<Note> n{playback_fixture::note(store.ids(), 0, 4 * pq::kPpq, 0)};
    REQUIRE(store.dispatch(AddClip{t, playback_fixture::clip(store.ids(), 0, 4 * pq::kPpq, std::move(n), "n")}).ok());
    return t;
  }
  DeviceId add(TrackId t, const char* patch, uint32_t at) {
    return must(ops::addDevice(*store.current(), store.ids(), t, patch, DeviceKind::AudioEffect, at)).devices.at(0);
  }
  ops::Edit must(ops::EditResult e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  // The project without wires `w`.
  Snapshot without(std::vector<WireId> w) const {
    DocumentStore s(store.current());
    REQUIRE(s.dispatch(ops::deleteWires(*store.current(), w)->command).ok());
    return s.current();
  }
};

}  // namespace

TEST_CASE("WI.2: two bad wires are cut (Refused / Type); the rest plays bit for bit as the project without them") {
  Song s;
  auto link = makeLink(s.store.current());  // the first compile (a build): fails, cuts both, compiles
  CHECK(link->stats().typeCutWires == 2);
  CHECK(link->stats().typeRetries >= 1);
  CHECK(link->stats().compileErrors == 0);
  const std::string rateMsg = link->wireError(s.rate), kindMsg = link->wireError(s.kind);
  MESSAGE("rate: " << rateMsg << "\nkind: " << kindMsg);
  CHECK(rateMsg.find("rate audio exceeds") != std::string::npos);
  CHECK(kindMsg.find("real into bool") != std::string::npos);
  for (const Wire& w : wiresOf(*s.store.current()))  // nothing else is cut
    if (w.id != s.rate && w.id != s.kind) CHECK(link->wireError(w.id).empty());
  const Channels played = render(*link, 0.5);
  auto ref = makeLink(s.without({s.rate, s.kind}));
  CHECK(ref->stats().typeCutWires == 0);
  CHECK(ref->stats().typeRetries == 0);  // a clean project: one round
  const Channels expected = render(*ref, 0.5);
  CHECK(peak(expected) > 0.01f);  // the Utility chain sounds
  CHECK(played == expected);      // bit for bit

  // The lowering the link compiles lists both Refused / Type.
  LowerOptions lo;
  lo.library = &playback_fixture::unitsLibrary().library();
  lo.devices = &playback_fixture::unitsLibrary();
  lo.cutWires = [&](WireId w) { return !link->wireError(w).empty(); };
  auto low = lower(*s.store.current(), lo);
  REQUIRE(low.ok());
  for (const WireId w : {s.rate, s.kind}) {
    const auto it = std::find_if(low->wires.begin(), low->wires.end(), [&](const WireState& x) { return x.id == w; });
    REQUIRE(it != low->wires.end());
    CHECK(it->status == WireStatus::Refused);
    CHECK(it->reason == WireRefusal::Type);
  }
  // An attached view context gets the cut set (what the cable views draw red, with the message).
  views::ViewContext ctx(*link);
  ctx.store().reset(s.store.current());
  link->attach(ctx);
  link->flush();
  REQUIRE(ctx.wireError(s.rate) != nullptr);
  CHECK(*ctx.wireError(s.rate) == rateMsg);
  CHECK(ctx.wireError(s.kind) != nullptr);
  for (const Wire& w : wiresOf(*s.store.current()))
    if (w.id != s.rate && w.id != s.kind) CHECK(ctx.wireError(w.id) == nullptr);
  link->detach();
}

TEST_CASE("WI.2: an edit of a cut wire's track tries it again, another track's edit leaves it cut; a deleted wire leaves the set") {
  Song s;
  auto link = makeLink(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->stats().typeCutWires == 2);
  const uint64_t retries = link->stats().typeRetries;
  // A structural edit of track B (a block added off its path): B's link is tried again (one round) and cut again; A's
  // wire stays cut without a round of its own.
  (void)s.add(s.b, "atom:add", ops::kOffPath);
  link->flush();
  CHECK(link->stats().typeRetries == retries + 1);
  CHECK(link->stats().typeCutWires == 2);
  CHECK(!link->wireError(s.rate).empty());
  CHECK(!link->wireError(s.kind).empty());
  // An edit of A (the rate wire's source track): tried again, still bad, cut again.
  (void)s.add(s.a, "atom:add", ops::kOffPath);
  link->flush();
  CHECK(link->stats().typeRetries == retries + 2);
  CHECK(link->stats().typeCutWires == 2);
  CHECK(link->stats().compileErrors == 0);
  // The bad wire deleted: out of the set, nothing retried.
  REQUIRE(s.store.dispatch(ops::deleteWires(*s.store.current(), std::vector<WireId>{s.rate})->command).ok());
  link->flush();
  CHECK(link->wireError(s.rate).empty());
  CHECK(link->stats().typeCutWires == 1);
  CHECK(link->stats().typeRetries == retries + 2);
  CHECK(link->stats().compileErrors == 0);
}
