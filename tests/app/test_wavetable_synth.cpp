// GU.8 (docs/plans/GU.md "Phases" 8): the example instrument, the Wavetable Synth (pq/document/modules.h
// wavetableSynthBlocks) — an instrument group of modules (Phase Accumulator → Wavetable → Envelope, two LFO pairs).
//   spawn    spawned on a MIDI track with an empty path (ops::pasteBlocksAtNotes, what the browser and the node menu
//            do), it takes the notes: the track's path is the synth group
//   render   a three-note chord plays through it: non-silent while the notes sound, interpreter == JIT bit for bit, with
//            both LFOs driving (LFO 2's vibrato depth raised off its default 0)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/modules.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kBlocks = 600;  // 0.8 s

void run(DocumentStore& s, const ops::EditResult& e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
}

golden::Channels render(const Snapshot& s, bool jit) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  app::ProjectLink& link = **l;
  REQUIRE(link.waitForMetaTables());
  if (jit) REQUIRE(link.waitForJit(180.0));
  link.play(0);
  golden::Channels out(2);
  std::vector<float> L(kBlock), R(kBlock);
  int64_t t = 0;
  for (uint32_t b = 0; b < kBlocks; ++b) {
    float* ch[2] = {L.data(), R.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
    }
    t += kBlock;
    out[0].insert(out[0].end(), L.begin(), L.end());
    out[1].insert(out[1].end(), R.begin(), R.end());
  }
  CHECK(link.lastError().empty());
  return out;
}

}  // namespace

TEST_CASE("GU.8 Wavetable Synth: spawned at a MIDI track's notes, a chord plays (non-silent, interpreter == JIT)") {
  DocumentStore s;
  auto tr = ops::createTrack(*s.current(), s.ids(), TrackKind::Midi);
  run(s, tr);
  const TrackId t = tr->tracks.at(0);
  {  // the default instrument out (dissolved: the source → mixer link stays), so the path is empty
    const MainPath path = mainPath(*s.current(), *findTrackPtr(*s.current(), t));
    std::vector<DeviceId> heads;
    for (const Device* d : path.blocks) heads.push_back(d->id);
    if (!heads.empty()) run(s, ops::dissolveDevices(*s.current(), s.ids(), nullptr, heads));
  }
  REQUIRE(mainPath(*s.current(), *findTrackPtr(*s.current(), t)).blocks.empty());
  IdSource local;
  const BlockSet set = wavetableSynthBlocks(local);
  const ops::EditResult e = ops::pasteBlocksAtNotes(*s.current(), s.ids(), t, set);
  run(s, e);
  const DeviceId synth = e->devices.at(0);
  const MainPath path = mainPath(*s.current(), *findTrackPtr(*s.current(), t));
  REQUIRE(path.blocks.size() == 1);
  CHECK(path.blocks[0]->id == synth);
  CHECK(path.complete);
  CHECK(isGroupDevice(*path.blocks[0]));
  CHECK(path.blocks[0]->hasPanel());
  // LFO 2's vibrato on (its Depth, a value block of the synth, to 15 cents of its 0 … 1200).
  DeviceId vibrato;
  forEachDevice(*s.current(), [&](const Device& d, const Track&) {
    if (d.group == synth && d.name == "Depth" && d.curve(kValueParam) != nullptr && d.curve(kValueParam)->max == 1200.0) vibrato = d.id;
  });
  REQUIRE(vibrato.valid());
  run(s, ops::setDeviceParam(*s.current(), vibrato, std::string(kValueParam), 15.0 / 1200.0));
  // A chord (degrees 0, 4, 7 of the default 12-EDO), half a second.
  {
    IdSource& ids = s.ids();
    std::vector<Note> n;
    for (const int32_t deg : {0, 4, 7}) n.push_back(playback_fixture::note(ids, 0, pq::kPpq, deg, 0, 100.0f));
    Batch b{"chord", {}};
    b.commands.push_back(AddClip{t, playback_fixture::clip(ids, 0, pq::kTicksPerWhole, n, "Chord")});
    REQUIRE(s.dispatch(b).ok());
  }
  const golden::Channels a = render(s.current(), false), b = render(s.current(), true);
  double diff = 0.0, peak = 0.0;
  for (size_t c = 0; c < 2; ++c)
    for (size_t i = 0; i < a[c].size(); ++i) {
      diff = std::max(diff, std::fabs(static_cast<double>(a[c][i]) - b[c][i]));
      peak = std::max(peak, static_cast<double>(std::fabs(a[c][i])));
    }
  MESSAGE("Wavetable Synth chord: peak " << peak << ", interpreter vs JIT " << diff);
  CHECK(peak > 0.05);
  CHECK(diff == 0.0);
}
