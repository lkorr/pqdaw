// GU.9 v2: records the reference renders of TODAY'S racks (T4.8's Rack / Chain model, before GU.9 removes it) into
// tests/app/fixtures/gu9_racks/*.wav. Run once on the commit before GU.9 (this file is deleted by the next commit; the
// renders stay and tests/app/test_racks.cpp compares the parallel-group racks against them).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "pq/app/project_link.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/media/wav_file.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kFrames = 12000;

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  return **lib;
}

std::vector<engine::TimedNote> chord() { return {{0, 9000, 220.0f, 0.8f, 0.5f}, {2000, 8000, 277.18f, 0.9f, 0.5f}}; }

golden::Channels render(const Project& p, TrackId track) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [track](TrackId t) { return t == track ? 0u : 1u; };
  auto low = lower(p, lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  for (const LoweredParam& x : low->params) {
    links.push_back({x.track, x.name, static_cast<uint32_t>(values.size())});
    values.push_back(x.value);
  }
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  co.voice.maxVoices = 8;
  co.paramLinks = links;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(low->graph);
  REQUIRE(build.ok());
  engine::NoteListSource notes(chord()), none(std::vector<engine::TimedNote>{});
  engine::ProjectConfig pc;
  pc.sampleRate = kRate;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  pc.numProjectParams = static_cast<uint32_t>(values.size());
  pc.eventSources = {notes.source(), none.source()};
  auto prog = engine::ProjectProgram::create(pc, build->layout);
  REQUIRE(prog.ok());
  for (uint32_t u = 0; u < build->images.size(); ++u) REQUIRE((*prog)->publish(u, std::move(build->images[u])).ok());
  std::copy(values.begin(), values.end(), (*prog)->projectParams().begin());
  golden::Channels out(2);
  std::vector<float> l(kBlock), r(kBlock);
  float* ch[2] = {l.data(), r.data()};
  for (int64_t t = 0; t < kFrames; t += kBlock) {
    engine::AudioBlock ab{nullptr, 0, ch, 2, kBlock, t};
    DenormalGuard dg;
    (*prog)->process(ab);
    out[0].insert(out[0].end(), l.begin(), l.end());
    out[1].insert(out[1].end(), r.begin(), r.end());
  }
  return out;
}

template <class E>
E run(DocumentStore& s, Result<E> e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
  return *e;
}
const Rack& rackOf(DocumentStore& s, DeviceId r) { return *findDevice(*s.current(), r).device->rack; }
void save(const golden::Channels& c, const char* name) {
  const auto dir = std::filesystem::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / "gu9_racks";
  std::filesystem::create_directories(dir);
  REQUIRE(media::writeWav(dir / name, c, static_cast<uint32_t>(kRate)).ok());
  float peak = 0.0f;
  for (const auto& ch : c)
    for (float v : ch) peak = std::max(peak, std::fabs(v));
  MESSAGE(name << " peak " << peak);
  CHECK(peak > 1e-3f);
}

}  // namespace

TEST_CASE("GU.9 reference renders of T4.8 racks") {
  {  // An Audio Effect Rack after the default instrument: [Utility (gain mapped to macro 1, 0.2 … 0.8, macro 64)] |
     // [Saturator]; then chain 2 at −6 dB, pan 0.4.
    DocumentStore s;
    const TrackId t = run(s, ops::createTrack(*s.current(), s.ids(), TrackKind::Midi)).tracks.at(0);
    const DeviceId r =
        run(s, ops::addDevice(*s.current(), s.ids(), t, std::string(kAudioEffectRack), DeviceKind::AudioEffect)).devices.at(0);
    run(s, ops::addChain(*s.current(), s.ids(), r));
    run(s, ops::addChain(*s.current(), s.ids(), r));
    const ChainId a = rackOf(s, r).chains.at(0)->id, b = rackOf(s, r).chains.at(1)->id;
    const DeviceId util =
        run(s, ops::addDevice(*s.current(), s.ids(), {}, "utility", DeviceKind::AudioEffect, kAppend, a)).devices.at(0);
    run(s, ops::addDevice(*s.current(), s.ids(), {}, "saturator", DeviceKind::AudioEffect, kAppend, b));
    run(s, ops::mapMacro(*s.current(), s.ids(), r, 0, util, "gain"));
    const MappingId m = rackOf(s, r).mappings.at(0).id;
    run(s, ops::setMacroMapping(*s.current(), m, 0.2, 0.8, 0.0));
    run(s, ops::setMacroValue(*s.current(), r, 0, 64.0));
    save(render(*s.current(), t), "audio_rack.wav");
    run(s, ops::setChainMixer(*s.current(), b, -6.0f, 0.4f));
    save(render(*s.current(), t), "audio_rack_mix.wav");
  }
  {  // An Instrument Rack alone on a MIDI track: [Basic Synth] | [Basic Synth, Utility (gain knob 0.6)]; then chain 2's
     // velocity zone 0 … 100, fading out 100 → 110 (the notes: velocity 0.8 = 101.6 in the fade, 0.9 = 114.3 outside).
    DocumentStore s;
    const TrackId t = run(s, ops::createTrack(*s.current(), s.ids(), TrackKind::Midi)).tracks.at(0);
    std::vector<DeviceId> all;
    for (const DevicePtr& d : findTrackPtr(*s.current(), t)->devices) all.push_back(d->id);
    run(s, ops::deleteDevices(*s.current(), all));
    const DeviceId r =
        run(s, ops::addDevice(*s.current(), s.ids(), t, std::string(kInstrumentRack), DeviceKind::Instrument, 0)).devices.at(0);
    run(s, ops::addChain(*s.current(), s.ids(), r));
    run(s, ops::addChain(*s.current(), s.ids(), r));
    const ChainId a = rackOf(s, r).chains.at(0)->id, b = rackOf(s, r).chains.at(1)->id;
    run(s, ops::addDevice(*s.current(), s.ids(), {}, std::string(kDefaultInstrument), DeviceKind::Instrument, kAppend, a));
    run(s, ops::addDevice(*s.current(), s.ids(), {}, std::string(kDefaultInstrument), DeviceKind::Instrument, kAppend, b));
    const DeviceId util =
        run(s, ops::addDevice(*s.current(), s.ids(), {}, "utility", DeviceKind::AudioEffect, kAppend, b)).devices.at(0);
    run(s, ops::setDeviceParam(*s.current(), util, "gain", 0.6));
    save(render(*s.current(), t), "instrument_rack.wav");
    run(s, ops::setChainZone(*s.current(), b, ZoneKind::Velocity, Zone{0.0, 0.0, 100.0, 110.0}));
    save(render(*s.current(), t), "instrument_rack_zone.wav");
  }
}
