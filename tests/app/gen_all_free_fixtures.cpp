// TX.all-free, one-off: writes the schema-16 projects of tests/app/fixtures/all_free with the code BEFORE TX.all-free
// (PQ_WRITE_ALLFREE_FIXTURES=1). Removed in the commit that changes the model (the files are what it made).
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/document/edit_ops.h"
#include "pq/document/serialize.h"

using namespace pq;
using namespace pq::doc;

namespace {

namespace fs = std::filesystem;

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  return **lib;
}

struct Gen {
  DocumentStore store;
  IdSource& ids = store.ids();
  void apply(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const Status st = store.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  void cmd(const Command& c) {
    const Status st = store.dispatch(c);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  TrackId midi(std::string name) {
    auto e = ops::createTrack(*store.current(), ids, TrackKind::Midi, {}, std::move(name));
    apply(e);
    return e->tracks.front();
  }
  TrackId track(TrackKind k, std::string name) {
    auto e = ops::createTrack(*store.current(), ids, k, {}, std::move(name));
    apply(e);
    return e->tracks.front();
  }
  DeviceId device(TrackId t, const char* patch, uint32_t index = kAppend) {
    auto e = ops::addDevice(*store.current(), ids, t, patch, DeviceKind::AudioEffect, index);
    apply(e);
    return e->devices.front();
  }
  DeviceId meta(TrackId t, const char* type, uint32_t index = kAppend) {
    auto e = ops::addMetaDevice(*store.current(), ids, t, type, index);
    apply(e);
    return e->devices.front();
  }
  DeviceId first(TrackId t) { return findTrackPtr(*store.current(), t)->devices.front()->id; }
  void notes(TrackId t, std::vector<int32_t> degrees, pq::Ticks step) {
    std::vector<Note> n;
    for (size_t i = 0; i < degrees.size(); ++i)
      n.push_back(playback_fixture::note(ids, static_cast<pq::Ticks>(i) * step, step, degrees[i] % 12, degrees[i] / 12,
                                         70.0f + 7.0f * static_cast<float>(i % 5)));
    n.push_back(playback_fixture::note(ids, 0, 3 * pq::kPpq, 0, 0, 90.0f));
    cmd(AddClip{t, playback_fixture::clip(ids, 0, 2 * pq::kTicksPerWhole, n, "notes")});
  }
  void wire(DeviceId a, const char* port, DeviceId b, const char* param) {
    apply(ops::connectWire(*store.current(), ids, a, port, b, param));
  }
  void save(const char* name) {
    const fs::path dir = fs::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / "all_free" / name;
    fs::create_directories(dir);
    const Status st = saveProject(dir.string(), store);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    MESSAGE("wrote " << dir.string());
  }
};

bool enabled() {
  const char* v = std::getenv("PQ_WRITE_ALLFREE_FIXTURES");
  return v != nullptr && *v == '1';
}

}  // namespace

TEST_CASE("all-free fixtures: write the schema-16 projects") {
  if (!enabled()) return;
  const pq::Ticks eighth = pq::kPpq / 2;
  {  // chains on every track kind: MIDI (a switched-off device in the chain), a group with a member, a return, master
    Gen g;
    const TrackId keys = g.midi("Keys");
    g.device(keys, "saturator");
    const DeviceId off = g.device(keys, "utility");
    g.device(keys, "eq_eight");
    g.apply(ops::setDevicesActive(*g.store.current(), std::vector<DeviceId>{off}, false));
    g.notes(keys, {0, 4, 7, 12, 7, 4, 2, 0}, eighth);
    const TrackId verb = g.track(TrackKind::Return, "Verb");
    g.device(verb, "saturator");
    g.cmd(SetSendLevel{keys, verb, -6.0f});
    const TrackId bass = g.midi("Bass");
    g.device(bass, "auto_filter");
    g.notes(bass, {0, 0, 5, 7}, pq::kPpq);
    auto ge = ops::groupTracks(*g.store.current(), g.ids, std::vector<TrackId>{bass}, "Bus");
    g.apply(ge);
    const TrackId bus = ge->tracks.front();
    REQUIRE(findTrackPtr(*g.store.current(), bus)->kind == TrackKind::Group);
    g.device(bus, "utility");
    g.device(g.store.current()->master->id, "utility");
    g.save("chains");
  }
  {  // free devices and wires: a free LFO into a param, a free saturator fed from the instrument and mixed back in, a
     // switched-off free utility passing the instrument through into the EQ, a note-source wire
    Gen g;
    const TrackId keys = g.midi("Keys");
    const DeviceId inst = g.first(keys);
    const DeviceId u1 = g.device(keys, "utility");
    const DeviceId eq = g.device(keys, "eq_eight");
    const DeviceId lfo = g.device(keys, "lfo_device");
    const DeviceId sat = g.device(keys, "saturator");
    const DeviceId pass = g.device(keys, "utility");
    g.apply(ops::setDevicesFree(*g.store.current(), std::vector<DeviceId>{lfo, sat, pass}, true));
    g.apply(ops::setDevicesActive(*g.store.current(), std::vector<DeviceId>{pass}, false));
    g.cmd(SetDeviceSetting{eq, "on1", 1.0});
    g.cmd(SetDeviceSetting{eq, "type1", 6.0});
    g.cmd(SetDeviceParam{eq, "freq1", 600.0});
    g.cmd(SetDeviceParam{lfo, "rate", 3.0});
    g.wire(lfo, "lfo", eq, "freq1");
    g.wire(inst, "out", sat, "x");
    g.wire(sat, "out", u1, "x");
    g.wire(inst, "out", pass, "x");
    g.wire(pass, "out", eq, "x");
    g.apply(ops::connectNoteWire(*g.store.current(), g.ids, keys, "velocity", u1, "gain"));
    g.notes(keys, {0, 3, 7, 10, 12, 10, 7, 3}, eighth);
    g.save("free_wires");
  }
  {  // a meta voice run: Phase accumulator → Wavetable → Envelope, then a stock effect; a free VCA modulating it
    Gen g;
    const TrackId keys = g.midi("Run");
    g.apply(ops::deleteDevices(*g.store.current(), std::vector<DeviceId>{g.first(keys)}));
    g.meta(keys, "phase_acc");
    g.meta(keys, "wavetable");
    g.meta(keys, "envelope");
    g.device(keys, "saturator");
    g.notes(keys, {0, 7, 12, 19, 12, 7}, eighth);
    g.save("meta_run");
  }
  {  // a group with chain links (Ctrl+G of a series) and a stock effect after it
    Gen g;
    const TrackId keys = g.midi("Grouped");
    const DeviceId a = g.device(keys, "utility");
    const DeviceId b = g.device(keys, "saturator");
    const DeviceId c = g.device(keys, "eq_eight");
    g.device(keys, "auto_filter");
    g.cmd(SetDeviceParam{a, "width", 0.3});
    auto e = ops::groupInCircuit(g.store.current(), g.ids, library(), std::vector<DeviceId>{a, b, c});
    g.apply(e);
    g.notes(keys, {0, 2, 4, 5, 7, 9, 11, 12}, eighth);
    g.save("group");
  }
  {  // racks: an Instrument Rack with two chains, then an Audio Effect Rack with two chains
    Gen g;
    const TrackId keys = g.midi("Racks");
    g.apply(ops::deleteDevices(*g.store.current(), std::vector<DeviceId>{g.first(keys)}));
    auto ir = ops::addDevice(*g.store.current(), g.ids, keys, std::string(kInstrumentRack), DeviceKind::Instrument);
    g.apply(ir);
    const DeviceId irack = ir->devices.front();
    g.apply(ops::addChainWithDevice(*g.store.current(), g.ids, irack, std::string(kDefaultInstrument), DeviceKind::Instrument));
    g.apply(ops::addChainWithDevice(*g.store.current(), g.ids, irack, std::string(kDefaultInstrument), DeviceKind::Instrument));
    const DeviceId arack = g.device(keys, std::string(kAudioEffectRack).c_str());
    g.apply(ops::addChainWithDevice(*g.store.current(), g.ids, arack, "saturator", DeviceKind::AudioEffect));
    g.apply(ops::addChainWithDevice(*g.store.current(), g.ids, arack, "utility", DeviceKind::AudioEffect));
    g.notes(keys, {0, 4, 7, 11, 14}, eighth);
    g.save("racks");
  }
}
