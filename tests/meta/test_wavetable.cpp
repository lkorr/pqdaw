// TMM.5: the Wavetable meta module — its data codec and validation, the expansion (own phasor when x is unwired in a
// voice context; TMM.8: the live path for modulated fold / warp / sync, Shape mode, the os setting), the table job
// (identity keys, one dense frame for one keyframe, the warp mode only with a warp; TMM.8: live params unbaked, Shape
// mode's antiderivative), the off-thread TableBuilder (newest wins, identical specs shared), and a standalone .pqp
// through pqc, bit-identical to the hand-written atoms.
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
  CHECK(t.liveBakeable);  // TMM.8: the live path (Shape mode refuses through bakedOnly)
  CHECK(t.bakedOnly != nullptr);
  CHECK(t.settingIndex("mode") == kWtMode);
  CHECK(t.settingIndex("warpMode") == kWtWarpMode);
  CHECK(t.settingIndex("os") == kWtOversampling);
  CHECK(t.chainInput == static_cast<int32_t>(kWtInX));  // TMM.10-fix1: the chain feeds x in every form
  CHECK(t.inputs[kWtInX].reads == Reads::Phase);           // the note source gives it the note's phase
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

TEST_CASE("wavetable: expansion — x unwired is phase 0 (no implicit note); refusals") {
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
  // TMM.10-fix1 (SPEC §15 2026-11-01 "notes are a signal"): x unwired is phase 0 in a voice context too — no phasor of
  // its own, no implicit note (the note's phase arrives by a wire: phasor(voice.hz), or the chain's note source).
  CHECK_FALSE(has(e->patch, "phasor"));
  CHECK(has(e->patch, "wavetable_pos"));
  CHECK_FALSE(has(e->patch, "voice.hz"));
  c.voice = false;
  auto g = expand(t, c, "wt");
  REQUIRE(g.ok());
  CHECK_FALSE(has(g->patch, "phasor"));
  CHECK(g->key != e->key);
  // TMM.8: Shape mode expands to wavetable_shape in an oversample(2) region (auto); a modulated fold / warp / sync in
  // Phase mode expands onto the live path (wavetable_live in an oversample region, ×8 auto for fold, ×16 with a moving
  // read phase: warp or sync), and is
  // refused in Shape mode with a reason.
  auto regionFactor = [&](const graph::Patch& p) -> uint32_t { return p.regions().empty() ? 0u : p.regions()[0].factor; };
  c.voice = true;
  const std::vector<double> shape{1.0, 0.0};
  c.settings = shape;
  auto s = expand(t, c, "wt");
  REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
  CHECK(has(s->patch, "wavetable_shape"));
  CHECK(regionFactor(s->patch) == 2);
  for (const uint32_t i : {kWtFold, kWtWarp, kWtSync}) {
    std::vector<uint8_t> mod(t.params.size(), 0);
    mod[i] = 1;
    c.modulated = mod;
    c.settings = {};
    auto m = expand(t, c, "wt");
    REQUIRE_MESSAGE(m.ok(), (m.ok() ? std::string() : m.error().message));
    CHECK(has(m->patch, "wavetable_live"));
    CHECK_FALSE(has(m->patch, "wavetable_pos"));
    CHECK(regionFactor(m->patch) == (i == kWtFold ? 8u : 16u));
    CHECK(m->key != e->key);
    c.settings = shape;
    auto ms = expand(t, c, "wt");
    REQUIRE_FALSE(ms.ok());
    CHECK(ms.error().message.find("Shape mode") != std::string::npos);
    CHECK(t.bakedOnly(shape, i).find("Shape mode") != std::string::npos);
    CHECK(t.bakedOnly(std::vector<double>{0.0, 0.0}, i).empty());
  }
  // The os setting picks the factor (1 … 4 = ×2 … ×16).
  {
    std::vector<uint8_t> mod(t.params.size(), 0);
    mod[kWtFold] = 1;
    c.modulated = mod;
    const std::vector<double> os4{0.0, 0.0, 2.0};
    c.settings = os4;
    auto m = expand(t, c, "wt");
    REQUIRE(m.ok());
    CHECK(regionFactor(m->patch) == 4);
  }
  c.settings = {};
  // Modulated phase / position are fine (runtime params).
  std::vector<uint8_t> mod(t.params.size(), 0);
  mod[kWtPhase] = mod[kWtPosition] = 1;
  c.modulated = mod;
  CHECK(expand(t, c, "wt").ok());
  // At the note source the chain wire gives x the note's phase: phasor(voice.hz) per voice. TMM.10-fix1: nested and
  // unwired, x is 0 — no phasor, no implicit note.
  c.modulated = {};
  CHECK_FALSE(has(expand(t, c, "wt_nested")->patch, "phasor"));
  c.form = Form::ChainNotes;
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
  // TMM.8: a modulated control is not baked (its live path applies it); a live warp takes sync along; Shape mode bakes
  // all three and builds the antiderivative arrays.
  {
    TableSource src;
    src.data = two;
    src.dataHash = dataHash(*two);
    const std::vector<double> params{0.0, 0.0, 0.4, 0.5, 0.3};
    src.params = params;
    src.sampleRate = 48000;
    std::vector<uint8_t> mod(5, 0);
    mod[kWtWarp] = 1;
    src.modulated = mod;
    TableJob j;
    REQUIRE(wavetableType().frameTable(src, 0, j).ok());
    CHECK(j.spec.bake.fold == 0.4f);
    CHECK(j.spec.bake.warp == 0.0f);
    CHECK(j.spec.bake.sync == 0.0f);
    CHECK_FALSE(j.spec.antiderivative);
    const std::vector<double> shapeSettings{1.0, 0.0};
    src.settings = shapeSettings;
    TableJob js;
    REQUIRE(wavetableType().frameTable(src, 0, js).ok());
    CHECK(js.spec.bake.warp == 0.5f);
    CHECK(js.spec.antiderivative);
    CHECK(js.key != j.key);
  }
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

TEST_CASE("wavetable: focused previews (TMM.16) — over the last table, the frames read and the keyframe redrawn equal the full table") {
  using Clock = std::chrono::steady_clock;
  std::mutex m;
  struct Got {
    uint64_t key;
    std::shared_ptr<const dsp::FrameTable> table;
  };
  std::vector<Got> got;
  TableBuilder tb([&](uint32_t, uint64_t key, std::shared_ptr<const dsp::FrameTable> t) {
    std::lock_guard lock(m);
    got.push_back({key, std::move(t)});
  });
  // Five keyframes, a baked fold and a Mirror warp (the bug report's setup), read at position 0.6.
  WavetableData d;
  const dsp::BasicCycle shapes[5] = {dsp::BasicCycle::Saw, dsp::BasicCycle::Square, dsp::BasicCycle::Triangle,
                                     dsp::BasicCycle::Pulse25, dsp::BasicCycle::Sine};
  for (int i = 0; i < 5; ++i) d.keyframes.push_back({0.25f * static_cast<float>(i), cycleOf(shapes[i])});
  const std::vector<double> params{0.0, 0.6, 0.55, -0.45, 0.0};
  const std::vector<double> settings{0.0, 1.0, 0.0};  // Phase mode, Mirror
  TableJob j0 = jobFor(wavetableData(d), params, settings);
  CHECK(j0.focus == doctest::Approx(0.6f));
  REQUIRE(TableBuilder::wantsPreview(j0.spec));
  tb.submit(1, std::move(j0));
  REQUIRE(tb.waitIdle());  // the full table: what the next preview builds over
  // A stroke on keyframe 2 (at 0.5): the job's preview rebuilds the frames around 0.6 and around 0.5.
  WavetableData e = d;
  for (uint32_t i = 300; i < 1400; ++i) e.keyframes[2].cycle[i] = 0.8f - 0.001f * static_cast<float>(i - 300);
  const DataPtr de = wavetableData(e);
  TableJob j1 = jobFor(de, params, settings);
  TableJob j0again = jobFor(wavetableData(d), params, settings);
  std::vector<uint32_t> focus;
  TableBuilder::focusFrames(j1, &j0again, focus);
  // 0.6·63 = 37.8: the reader's pair 37, 38; keyframe 2 at 0.5: 31, 32; one each side of the position: 36, 39.
  CHECK(focus == std::vector<uint32_t>{31, 32, 36, 37, 38, 39});
  const uint64_t key1 = j1.key;
  tb.submit(1, std::move(j1));
  std::shared_ptr<const dsp::FrameTable> preview;
  for (const auto until = Clock::now() + std::chrono::seconds(20); Clock::now() < until && !preview;) {
    {
      std::lock_guard lock(m);
      for (const Got& g : got)
        if (g.key == key1) preview = g.table;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(preview != nullptr);
  CHECK(tb.stats().focused == 1u);
  CHECK(preview->frames() == kWtDefaultFrames);
  // The full table of the new state, built directly.
  TableJob direct = jobFor(de, params, settings);
  dsp::FrameTableBuilder b;
  auto full = b.build(direct.spec);
  REQUIRE(full.ok());
  // Read where the voices read (0.6) and on the redrawn keyframe (0.5): the preview is the full table there.
  double worst = 0.0;
  for (const double u : {0.6, 0.5, 0.59, 0.61})
    for (uint32_t level : {0u, 5u})
      for (int i = 0; i < 64; ++i) {
        const double ph = (i + 0.37) / 64.0;
        worst = std::max(worst, std::fabs(dsp::readFrameTableReference(preview->view(), ph, u, level) -
                                          dsp::readFrameTableReference(full.value().view(), ph, u, level)));
      }
  MESSAGE("focused preview vs the full table where it is read: " << worst);
  CHECK(worst <= 1e-6);
  // The rest keeps the previous table until the full build (which follows once the drag pauses).
  REQUIRE(tb.waitIdle());
  {
    std::lock_guard lock(m);
    REQUIRE(!got.empty());
    CHECK(got.back().key == key1);
    CHECK(got.back().table->frames() == kWtDefaultFrames);
    got.clear();
  }
  // forget(): a later preview for the target never builds over an earlier instance's table (none to build over:
  // TMM.9's 8-frame preview first).
  tb.forget(1);
  WavetableData f = e;
  f.keyframes[2].cycle[5] = 0.25f;
  tb.submit(1, jobFor(wavetableData(f), params, settings));
  REQUIRE(tb.waitIdle());
  std::lock_guard lock(m);
  REQUIRE(got.size() == 2u);
  CHECK(got[0].table->frames() == TableBuilder::kPreviewFrames);
  CHECK(got[1].table->frames() == kWtDefaultFrames);
  CHECK(tb.stats().focused == 1u);
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
  // TMM.10-fix1: the note's phase is wired in (an unwired x is phase 0: no implicit note).
  const fs::path wt = writeTemp("wt.pqp",
                                "patch W {\n  out out : audio voice mono\n  wt1 = meta_wavetable(phasor(voice.hz))\n  out = wt1\n}\n");
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
