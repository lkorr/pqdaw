// TMM.5: the Wavetable meta module — its data codec and validation, the expansion (own phasor when x is unwired in a
// voice context; Shape mode and modulated baked controls refused), the table job (identity keys, one dense frame for
// one keyframe, the warp mode only with a warp), the off-thread TableBuilder (newest wins, identical specs shared), and
// a standalone .pqp through pqc, bit-identical to the hand-written atoms.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "pq/engine/offline_render.h"
#include "pq/meta/registry.h"
#include "pq/meta/table_builder.h"
#include "pq/meta/wavetable.h"
#include "pq/pqc/pqc.h"

using namespace pq;
using namespace pq::meta;

namespace {

std::vector<float> cycleOf(dsp::BasicCycle c) {
  const AlignedBuffer<float> b = dsp::basicCycle(c);
  return {b.span().begin(), b.span().end()};
}

WavetableData sawToSquare() {
  WavetableData d;
  d.keyframes.push_back({0.0f, cycleOf(dsp::BasicCycle::Saw)});
  d.keyframes.push_back({1.0f, cycleOf(dsp::BasicCycle::Square)});
  d.gaps = {dsp::GapMode::Crossfade};
  return d;
}

TableJob jobFor(const DataPtr& data, std::vector<double> params, std::vector<double> settings = {}, double sr = 48000) {
  TableSource s;
  s.data = data;
  s.dataHash = data ? dataHash(*data) : 0;
  s.params = params;
  s.settings = settings;
  s.sampleRate = sr;
  TableJob j;
  REQUIRE(wavetableType().frameTable(s, 0, j).ok());
  return j;
}

}  // namespace

TEST_CASE("wavetable: registered; controls, inputs, settings as planned") {
  const Type& t = wavetableType();
  CHECK(findType("wavetable") == &t);
  CHECK(callName(t) == "meta_wavetable");
  CHECK(t.inputIndex("x") == kWtInX);
  CHECK(t.inputIndex("pos") == kWtInPos);
  CHECK(t.paramIndex("phase") == kWtPhase);
  CHECK(t.paramIndex("position") == kWtPosition);
  CHECK(t.paramIndex("fold") == kWtFold);
  CHECK(t.paramIndex("warp") == kWtWarp);
  CHECK(t.paramIndex("sync") == kWtSync);
  CHECK(t.params[kWtFold].bakeable);
  CHECK(t.params[kWtWarp].bakeable);
  CHECK(t.params[kWtSync].bakeable);
  CHECK_FALSE(t.params[kWtPosition].bakeable);
  CHECK_FALSE(t.liveBakeable);
  CHECK(t.settingIndex("mode") == kWtMode);
  CHECK(t.settingIndex("warpMode") == kWtWarpMode);
  CHECK(t.chainInstrument);
  CHECK(t.chainEffectInput < 0);
  CHECK(prelude()->find("wavetable") != nullptr);
}

TEST_CASE("wavetable: data round trip, validation, the default") {
  WavetableData d = sawToSquare();
  d.frames = 32;
  d.removeDc = true;
  const DataPtr v = wavetableData(d);
  CHECK(wavetableDataProblem(*v).empty());
  CHECK(dataProblem(*v).empty());
  auto back = parseWavetableData(*v);
  REQUIRE(back.ok());
  REQUIRE(back->keyframes.size() == 2);
  CHECK(back->keyframes[1].position == 1.0f);
  CHECK(back->keyframes[0].cycle == d.keyframes[0].cycle);
  CHECK(back->gaps == d.gaps);
  CHECK(back->frames == 32);
  CHECK(back->removeDc);
  CHECK_FALSE(back->normalise);
  CHECK(dataEqual(wavetableData(*back), v));
  // The default: one sine keyframe, valid.
  const DataPtr def = wavetableDefaultData();
  CHECK(wavetableDataProblem(*def).empty());
  CHECK(def == wavetableDefaultData());  // one shared value (its hash is computed once by the link)
  CHECK(parseWavetableData(*def)->keyframes.size() == 1);
  // Problems.
  WavetableData bad = sawToSquare();
  bad.keyframes[1].cycle.resize(100);
  CHECK_FALSE(wavetableDataProblem(*wavetableData(bad)).empty());
  bad = sawToSquare();
  bad.keyframes[0].position = 0.8f;
  bad.keyframes[1].position = 0.2f;
  CHECK_FALSE(wavetableDataProblem(*wavetableData(bad)).empty());
  bad = sawToSquare();
  bad.frames = 0;
  CHECK_FALSE(wavetableDataProblem(*wavetableData(bad)).empty());
  CHECK_FALSE(wavetableDataProblem(DataValue::num(1)).empty());
  CHECK_FALSE(wavetableDataProblem(DataValue::record({{"keyframes", DataValue::list({})}})).empty());
  CHECK_FALSE(wavetableDataProblem(DataValue::record({{"keyframes", *def->field("keyframes")},
                                                      {"gaps", DataValue::list({DataValue::str("spectral")})}}))
                   .empty());  // one keyframe has no gap
  CHECK(wavetableType().dataProblem(*def).empty());
}

TEST_CASE("wavetable: expansion — own phasor when x is unwired in a voice context; refusals") {
  const Type& t = wavetableType();
  ExpandContext c;
  const std::vector<uint32_t> slots{7};
  c.slots = slots;
  auto e = expand(t, c, "wt");
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(e->resources.size() == 1);
  CHECK(e->resources[0].kind == ResourceKind::FrameTable);
  auto has = [&](const graph::Patch& p, std::string_view atom) {
    for (graph::NodeIndex n = 0; n < p.nodeCount(); ++n)
      if (p.node(n).kind == graph::NodeKind::Atom && p.node(n).atom == graph::atomId(atom)) return true;
    return false;
  };
  CHECK(has(e->patch, "phasor"));
  CHECK(has(e->patch, "wavetable_pos"));
  CHECK(has(e->patch, "voice.hz"));
  // Outside a voice context x defaults to 0: no phasor.
  c.voice = false;
  auto g = expand(t, c, "wt");
  REQUIRE(g.ok());
  CHECK_FALSE(has(g->patch, "phasor"));
  CHECK(g->key != e->key);
  // Shape mode (TMM.8) and modulated baked controls are refused with a reason.
  c.voice = true;
  const std::vector<double> shape{1.0, 0.0};
  c.settings = shape;
  auto s = expand(t, c, "wt");
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().message.find("Shape mode") != std::string::npos);
  c.settings = {};
  for (const uint32_t i : {kWtFold, kWtWarp, kWtSync}) {
    std::vector<uint8_t> mod(t.params.size(), 0);
    mod[i] = 1;
    c.modulated = mod;
    auto m = expand(t, c, "wt");
    REQUIRE_FALSE(m.ok());
    CHECK(m.error().message.find(std::string(t.params[i].name)) != std::string::npos);
  }
  // Modulated phase / position are fine (runtime params).
  std::vector<uint8_t> mod(t.params.size(), 0);
  mod[kWtPhase] = mod[kWtPosition] = 1;
  c.modulated = mod;
  CHECK(expand(t, c, "wt").ok());
  // The chain instrument form plays its own phasor per voice.
  c.modulated = {};
  c.form = Form::ChainInstrument;
  auto chain = expand(t, c, "wt_chain");
  REQUIRE(chain.ok());
  CHECK(has(chain->patch, "phasor"));
}

TEST_CASE("wavetable: table jobs — identity keys, dense frames, bake values, warp mode only with a warp") {
  const DataPtr one = wavetableDefaultData();
  const DataPtr two = wavetableData(sawToSquare());
  TableJob a = jobFor(one, {});
  CHECK(a.spec.denseFrames == 1);  // one keyframe: one frame
  CHECK_FALSE(a.spec.bake.active());
  CHECK(a.spec.keyframes.size() == 1);
  CHECK(a.spec.keyframes[0].cycle.data() == one->field("keyframes")->items[0].field("cycle")->floats.data());  // no copy
  TableJob b = jobFor(two, {});
  CHECK(b.spec.denseFrames == kWtDefaultFrames);
  REQUIRE(b.spec.gaps.size() == 1);
  CHECK(b.spec.gaps[0] == dsp::GapMode::Crossfade);
  CHECK(a.key != b.key);
  CHECK(jobFor(two, {}).key == b.key);                       // equal state, equal key
  CHECK(jobFor(wavetableData(sawToSquare()), {}).key == b.key);  // equal data in another value: equal key
  // Baked values (phase and position are not part of the table).
  TableJob f = jobFor(two, {0.3, 0.7, 0.25, 0.0, 0.0});
  CHECK(f.spec.bake.fold == 0.25f);
  CHECK(f.key != b.key);
  CHECK(jobFor(two, {0.9, 0.1, 0.0, 0.0, 0.0}).key == b.key);
  CHECK(jobFor(two, {0.0, 0.0, 5.0, 0.0, 0.0}).spec.bake.fold == 1.0f);  // clamped
  // The warp mode matters only with a warp.
  CHECK(jobFor(two, {}, {0.0, 2.0}).key == b.key);
  const TableJob w0 = jobFor(two, {0.0, 0.0, 0.0, 0.5, 0.0}, {0.0, 0.0});
  const TableJob w2 = jobFor(two, {0.0, 0.0, 0.0, 0.5, 0.0}, {0.0, 2.0});
  CHECK(w0.key != w2.key);
  CHECK(w2.spec.bake.warpMode == dsp::WarpMode::PulseWidth);
  // The sample rate.
  CHECK(jobFor(two, {}, {}, 44100.0).key != b.key);
  // A moved job keeps its spans valid.
  TableJob moved = std::move(b);
  moved.finish();
  CHECK(moved.spec.keyframes.size() == 2);
  CHECK(moved.spec.keyframes[1].cycle.size() == dsp::kKeyframeCycle);
}

TEST_CASE("wavetable: TableBuilder — builds off-thread, newest wins per target, identical specs shared") {
  std::mutex m;
  std::vector<std::pair<uint32_t, uint64_t>> got;
  std::vector<std::shared_ptr<const dsp::FrameTable>> tables;
  TableBuilder tb([&](uint32_t target, uint64_t key, std::shared_ptr<const dsp::FrameTable> t) {
    std::lock_guard lock(m);
    got.emplace_back(target, key);
    tables.push_back(std::move(t));
  });
  const DataPtr two = wavetableData(sawToSquare());
  TableJob j = jobFor(two, {});
  const uint64_t key = j.key;
  tb.submit(1, std::move(j));
  REQUIRE(tb.waitIdle());
  REQUIRE(got.size() == 1);
  CHECK(got[0] == std::make_pair(1u, key));
  CHECK(tables[0]->frames() == kWtDefaultFrames);
  CHECK(tb.stats().built == 1);
  CHECK(tb.cached(key) == tables[0]);
  // The same spec for another target: shared, no build.
  tb.submit(2, jobFor(two, {}));
  REQUIRE(tb.waitIdle());
  REQUIRE(got.size() == 2);
  CHECK(tables[1] == tables[0]);
  CHECK(tb.stats().built == 1);
  CHECK(tb.stats().shared == 1);
  // A drag: many values for one target. The last one is always delivered last; queued ones are replaced.
  uint64_t last = 0;
  for (int i = 1; i <= 12; ++i) {
    TableJob d = jobFor(two, {0.0, 0.0, 0.05 * i, 0.0, 0.0});
    last = d.key;
    tb.submit(3, std::move(d));
  }
  REQUIRE(tb.waitIdle());
  CHECK(got.back() == std::make_pair(3u, last));
  const TableBuilder::Stats st = tb.stats();
  CHECK(st.submitted == 14);
  CHECK(st.built + st.shared + st.replaced == 14);
  MESSAGE("drag of 12 fold values: built " << st.built - 1 << ", replaced " << st.replaced << "; last bake build "
                                           << st.lastBuildMs << " ms");
  // A table nobody holds any more is built again (the cache keeps no table alive).
  tables.clear();
  tb.submit(1, jobFor(two, {}));
  REQUIRE(tb.waitIdle());
  CHECK(tb.stats().built == st.built + 1);
}

TEST_CASE("wavetable: TableBuilder previews — a slow (baked) job is heard first as an 8-frame preview, the full table once the drag pauses") {
  using Clock = std::chrono::steady_clock;
  std::mutex m;
  struct Got {
    uint64_t key;
    uint32_t frames;
    Clock::time_point at;
  };
  std::vector<Got> got;
  std::vector<std::shared_ptr<const dsp::FrameTable>> keep;
  TableBuilder tb([&](uint32_t, uint64_t key, std::shared_ptr<const dsp::FrameTable> t) {
    std::lock_guard lock(m);
    got.push_back({key, t->frames(), Clock::now()});
    keep.push_back(std::move(t));
  });
  const DataPtr two = wavetableData(sawToSquare());
  // Policy: unbaked 64 frames is fast enough (≈ 10 ms) to build in full; a bake is previewed; one keyframe never.
  CHECK_FALSE(TableBuilder::wantsPreview(jobFor(two, {}).spec));
  CHECK(TableBuilder::wantsPreview(jobFor(two, {0.0, 0.0, 0.3, 0.0, 0.0}).spec));
  // A fold drag at 60 Hz for ≈ 0.5 s: each step's preview arrives quickly; the full table follows the last step.
  std::vector<std::pair<uint64_t, Clock::time_point>> sent;
  const auto t0 = Clock::now();
  for (int i = 1; i <= 30; ++i) {
    TableJob j = jobFor(two, {0.0, 0.0, 0.02 * i, 0.0, 0.0});
    sent.emplace_back(j.key, Clock::now());
    tb.submit(7, std::move(j));
    std::this_thread::sleep_until(t0 + std::chrono::microseconds(16667 * i));
  }
  const uint64_t last = sent.back().first;
  // The full build waits for the pause (kPreviewSettle) and then arrives on its own (no waitIdle).
  for (int i = 0; i < 400; ++i) {
    {
      std::lock_guard lock(m);
      if (!got.empty() && got.back().key == last && got.back().frames == kWtDefaultFrames) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::lock_guard lock(m);
  REQUIRE(!got.empty());
  CHECK(got.back().key == last);
  CHECK(got.back().frames == kWtDefaultFrames);  // the full table last
  CHECK(tb.cached(last) == keep.back());         // only full tables are cached
  // Latency per step: submit → first delivery of that key (a preview, or the full table).
  std::vector<double> lat;
  size_t previews = 0;
  for (const auto& [key, at] : sent)
    for (const Got& g : got)
      if (g.key == key) {
        lat.push_back(std::chrono::duration<double, std::milli>(g.at - at).count());
        break;
      }
  for (const Got& g : got) previews += g.frames == TableBuilder::kPreviewFrames ? 1u : 0u;
  std::sort(lat.begin(), lat.end());
  REQUIRE(!lat.empty());
  const TableBuilder::Stats st = tb.stats();
  MESSAGE("fold drag, 30 steps at 60 Hz: " << lat.size() << " steps heard (" << previews << " previews, " << st.built
                                           << " full builds), latency median " << lat[lat.size() / 2] << " ms, max "
                                           << lat.back() << " ms; preview build " << st.lastPreviewMs << " ms (max "
                                           << st.maxPreviewMs << "), full " << st.lastBuildMs << " ms");
  CHECK(st.built == 1);             // one full table: when the drag stopped
  CHECK(previews >= 5);             // (load-tolerant here; the rate is measured above and in the app test)
  CHECK(st.built + st.shared + st.replaced == st.submitted);
}

namespace {

namespace fs = std::filesystem;

fs::path writeTemp(const std::string& name, const std::string& text) {
  const fs::path p = fs::temp_directory_path() / ("pq_test_wt_" + name);
  std::ofstream(p, std::ios::binary) << text;
  return p;
}

std::vector<float> render(const fs::path& file) {
  pqc::RenderOptions o;
  o.notes = "60@0:0.4";
  o.seconds = 0.5;
  engine::BufferSink sink;
  auto r = pqc::renderFile(file, o, sink);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  REQUIRE(sink.channels.size() == 1);
  return sink.channels[0];
}

}  // namespace

TEST_CASE("wavetable: a standalone .pqp renders in pqc, bit-identical to the hand-written atoms") {
  const fs::path wt = writeTemp("wt.pqp", "patch W {\n  out out : audio voice mono\n  wt1 = meta_wavetable()\n  out = wt1\n}\n");
  const fs::path ref = writeTemp("ref.pqp",
                                 "patch R {\n  out out : audio voice mono\n  out = wavetable_pos(phasor(voice.hz), 0)\n}\n");
  const std::vector<float> a = render(wt), b = render(ref);
  REQUIRE(a.size() == b.size());
  float peak = 0.0f;
  size_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    peak = std::max(peak, std::fabs(a[i]));
    diff += a[i] != b[i] ? 1 : 0;
  }
  CHECK(peak > 0.9f);  // the engine's default table: a sine
  CHECK(diff == 0);
}
