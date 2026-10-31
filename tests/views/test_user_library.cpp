// T5.11: the user library in the views (pq/views/browser/library.h "T5.11", library_load.h) over a User Library in
// %TEMP% (never the real Documents): Save Preset / Save as Default Preset / Save as New Device from the Device view, the
// default preset applied to a new device, presets listed under their device in Instruments / Audio Effects, loading
// presets, user devices, tunings and clips from the browser (one undo step each), hot-swap of a device's preset (wires
// kept) and of a tuning, the filter / tag chips and tag editing, the resizable width, a saved file reaching the browser
// through the real indexer (≤ 500 ms in `perf`), the golden browser-hotswap. T5.12: clips dragged out of the
// arrangement become a Clips payload; dropped on a browser folder they are saved as .pqclip (an audio clip with a copy
// of its sample).

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "../../src/views/src/browser/browser_parts.h"  // the panel parts (private to views)
#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/library_files.h"
#include "pq/library/indexer.h"
#include "pq/library/scan.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/browser/browser_panel.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/library_load.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
namespace fs = std::filesystem;

namespace {

const doc::DeviceLibrary& devLib() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

std::string u8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

// The shell over a MIDI track (default instrument + Saturator) and an audio track, with a User Library in %TEMP%.
struct Lib {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1400, 900};
  std::unique_ptr<Shell> shell;
  std::string root;
  doc::TrackId keys, audio;
  doc::DeviceId sat;
  explicit Lib(const char* name) {
    root = views::test::tempDir(std::string("userlib_") + name);
    ctx.setDeviceLibrary(&devLib());
    ctx.library().setUserLibrary(root);
    doc::DocumentStore& st = ctx.store();
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks[0];
    audio = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "Audio")).tracks[0];
    sat = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "saturator", doc::DeviceKind::AudioEffect)).devices[0];
    reindex();
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    ctx.setSelectedTrack(keys);
    ctx.setBrowserVisible(true);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
    d.tree().layoutNow();
    d.step();
  }
  ~Lib() {
    d.tree().setRoot(std::make_unique<Widget>());
    std::error_code ec;
    fs::remove_all(fs::path(std::u8string(root.begin(), root.end())), ec);
  }
  doc::ops::Edit run(const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(ctx.store().dispatch(e->command).ok());
    return *e;
  }
  // A synchronous index of the places (what the indexer would publish).
  void reindex() {
    library::ScanRequest req;
    req.places = ctx.library().places();
    library::DiskSource src;
    library::ScanResult r = library::rebuild(nullptr, req, src, library::probeFile);
    ctx.library().setIndex(r.index);
  }
  BrowserPanel& browser() const {
    BrowserPanel* b = findBrowser(*shell);
    REQUIRE(b != nullptr);
    return *b;
  }
  DeviceChainView& chain() const {
    DeviceChainView* v = findDeviceChain(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const doc::Track& track(doc::TrackId id) const { return *doc::findTrackPtr(*ctx.doc(), id); }
  const doc::Device* device(doc::DeviceId id) const { return doc::findDevice(*ctx.doc(), id).device; }
  size_t history() const { return ctx.store().history().cursor(); }
  std::string path(std::string_view rel) const { return root + "\\" + std::string(rel); }
  bool exists(std::string_view rel) const {
    const std::string p = path(rel);
    return fs::exists(fs::path(std::u8string(p.begin(), p.end())));
  }
  std::vector<std::string> names() {
    std::vector<std::string> v;
    BrowserPanel& b = browser();
    for (const BrowserRow& r : b.rows()) v.push_back(b.cellText(r, BrowserColumn::Name));
    return v;
  }
};

}  // namespace

TEST_CASE("user library: Save Preset writes Presets/<Device>/, the browser renames it in place; it loads back exactly") {
  Lib l("save");
  l.run(doc::ops::setDeviceParam(*l.ctx.doc(), l.sat, "drive", 0.7));
  const size_t h0 = l.history();
  REQUIRE(l.chain().savePreset(l.sat, false));
  CHECK(l.history() == h0);  // saving is not an edit
  REQUIRE(l.exists("Presets\\Saturator\\Saturator.pqpreset"));
  const std::string file = l.path("Presets\\Saturator\\Saturator.pqpreset");
  CHECK(l.ctx.library().pendingRename == file);
  // The index shows it: the browser opens the name field on it.
  l.reindex();
  CHECK(l.ctx.library().pendingRename.empty());
  CHECK(l.browser().selectedPath() == file);
  CHECK(l.browser().list().renaming());
  // The file is the device (ids aside).
  auto f = doc::parsePreset(*doc::readTextFile(file));
  REQUIRE(f.ok());
  CHECK(f->device->patch == "saturator");
  CHECK(f->device->params == l.device(l.sat)->params);
  // Loaded from the browser into the selected track: one undo step, a new device with the same values.
  l.browser().list().renameField()->onCancel();
  auto id = loadDeviceItem(l.ctx, file, DeviceSlot{l.keys, doc::kAppend, {}});
  REQUIRE(id.ok());
  CHECK(l.history() == h0 + 1);
  CHECK(*id != l.sat);
  CHECK(l.device(*id)->params == l.device(l.sat)->params);
  // A second save does not overwrite the first.
  REQUIRE(l.chain().savePreset(l.sat, false));
  CHECK(l.exists("Presets\\Saturator\\Saturator 2.pqpreset"));
}

TEST_CASE("user library: a new device starts from its default preset") {
  Lib l("defaults");
  l.run(doc::ops::setDeviceParam(*l.ctx.doc(), l.sat, "drive", 0.25));
  REQUIRE(l.chain().savePreset(l.sat, true));
  REQUIRE(l.exists("Defaults\\Saturator.pqpreset"));
  const doc::DeviceInfo* info = devLib().catalog().find("saturator");
  REQUIRE(info != nullptr);
  const size_t h0 = l.history();
  REQUIRE(l.chain().addDevice(*info));
  CHECK(l.history() == h0 + 1);
  const doc::DevicePtr added = l.track(l.keys).devices.back();
  CHECK(added->id != l.sat);
  REQUIRE(added->param("drive") != nullptr);
  CHECK(added->param("drive")->value == doctest::Approx(0.25));
  // Another device without a default starts plain.
  REQUIRE(l.chain().addDevice(*devLib().catalog().find("utility")));
  CHECK(l.track(l.keys).devices.back()->params.empty());
}

TEST_CASE("user library: Save as New Device joins the device menu and the browser; the project embeds its text") {
  Lib l("userdev");
  REQUIRE_FALSE(l.chain().saveAsNewDevice(l.sat));  // a stock device: nothing to save as a new device
  const std::string text = devLib().stockText("saturator") + "\n// mine\n";
  l.run(doc::ops::editDevicePatch(*l.ctx.doc(), l.sat, text));
  REQUIRE(l.chain().saveAsNewDevice(l.sat));
  REQUIRE(l.exists("Devices\\Saturator.pqdevice"));
  l.reindex();
  const auto& users = l.ctx.library().userDevices();
  REQUIRE(users.size() == 1);
  CHECK(users[0].file.text == text);
  CHECK(users[0].file.origin == "devices/saturator.pqp");
  // In the device menu under User Devices; choosing it adds a device playing that text.
  std::vector<MenuItem> menu = l.chain().deviceMenu();
  const MenuItem* item = nullptr;
  for (const MenuItem& m : menu)
    if (m.label == "   Saturator" && &m != &menu.front()) item = &m;  // the last "Saturator": the user device
  REQUIRE(item != nullptr);
  const size_t before = l.track(l.keys).devices.size();
  item->onChoose();
  REQUIRE(l.track(l.keys).devices.size() == before + 1);
  const doc::DevicePtr made = l.track(l.keys).devices.back();
  REQUIRE(made->source != nullptr);
  CHECK(made->source->text == text);
  // Audio Effects lists it (a user device of that kind).
  l.browser().setSource({BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::AudioEffects)});
  const auto names = l.names();
  CHECK(std::find(names.begin(), names.end(), "Saturator.pqdevice") != names.end());
}

TEST_CASE("user library: Instruments / Audio Effects list each device with its presets under it") {
  Lib l("tree");
  REQUIRE(l.chain().savePreset(l.sat, false));
  l.browser().list().beginRename(-1);
  l.reindex();
  if (l.browser().list().renaming()) l.browser().list().renameField()->onCancel();
  BrowserPanel& b = l.browser();
  b.setSource({BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::AudioEffects)});
  auto names = l.names();
  const auto it = std::find(names.begin(), names.end(), "Saturator");
  REQUIRE(it != names.end());
  CHECK(std::find(names.begin(), names.end(), "Audio Effect Rack") != names.end());
  CHECK(std::find(names.begin(), names.end(), "Saturator.pqpreset") == names.end());  // folded
  const int32_t row = static_cast<int32_t>(it - names.begin());
  b.setRowOpen(b.rows()[static_cast<size_t>(row)], true);
  names = l.names();
  REQUIRE(static_cast<size_t>(row) + 1 < names.size());
  CHECK(names[static_cast<size_t>(row) + 1] == "Saturator.pqpreset");
  CHECK(b.rows()[static_cast<size_t>(row) + 1].depth == 1);
  // Enter on the device row adds the device to the selected track (one undo step).
  b.select(row, false);
  const size_t h0 = l.history();
  REQUIRE(b.loadSelection());
  CHECK(l.history() == h0 + 1);
  CHECK(l.track(l.keys).devices.back()->patch == "saturator");
  // Its drag payload names the catalog device; the Device view takes it.
  const DragPayload p = b.selectionPayload();
  REQUIRE(p.paths.size() == 1);
  CHECK(catalogPatchOf(p.paths[0]) == "saturator");
  CHECK(l.chain().dragOver(p, {}, mod::kNone).effect == DropEffect::Copy);
  CHECK(l.chain().drop(p, {}, mod::kNone) == DropEffect::Copy);
  CHECK(l.history() == h0 + 2);
}

TEST_CASE("user library: hot-swap replaces the device's preset in place, keeping its id, position and wires") {
  Lib l("hotswap");
  const doc::DeviceId util = l.run(doc::ops::addDevice(*l.ctx.doc(), l.ctx.store().ids(), l.keys, "utility",
                                                       doc::DeviceKind::AudioEffect)).devices[0];
  l.run(doc::ops::setDeviceParam(*l.ctx.doc(), l.sat, "drive", 0.9));
  REQUIRE(l.chain().savePreset(l.sat, false));  // "Saturator.pqpreset": drive 0.9
  l.run(doc::ops::setDeviceParam(*l.ctx.doc(), l.sat, "drive", 0.1));
  const doc::WireId w = l.run(doc::ops::connectWire(*l.ctx.doc(), l.ctx.store().ids(), util, "out", l.sat, "drive")).wires[0];
  l.reindex();
  if (l.browser().list().renaming()) l.browser().list().renameField()->onCancel();
  const uint32_t index = doc::findDevice(*l.ctx.doc(), l.sat).index;
  // The title bar's preset slot puts the browser in hot-swap mode for this device.
  devices::DevicePanel* panel = l.chain().panelOf(l.sat);
  REQUIRE(panel != nullptr);
  REQUIRE(panel->onPresetClick);
  panel->onPresetClick();
  BrowserPanel& b = l.browser();
  CHECK(b.hotSwap().kind == HotSwapTarget::Kind::Device);
  CHECK(b.hotSwapBar().visible());
  REQUIRE(l.names() == std::vector<std::string>{"Saturator.pqpreset"});
  const size_t h0 = l.history();
  b.select(0);  // selecting loads it in place
  CHECK(l.history() == h0 + 1);
  const doc::DeviceLoc loc = doc::findDevice(*l.ctx.doc(), l.sat);
  REQUIRE(loc.device != nullptr);
  CHECK(loc.index == index);
  REQUIRE(loc.device->param("drive") != nullptr);
  CHECK(loc.device->param("drive")->value == doctest::Approx(0.9));
  CHECK(doc::findWire(*l.ctx.doc(), w) != nullptr);
  CHECK(l.ctx.library().presetName(l.sat) == "Saturator");
  CHECK(panel->presetName() == "Saturator");
  // Esc leaves hot-swap mode.
  l.d.tree().setFocus(&b.listWidget());
  l.d.press("Escape");
  CHECK(b.hotSwap().kind == HotSwapTarget::Kind::None);
}

TEST_CASE("user library: tunings save to the library and hot-swap onto a track") {
  Lib l("tunings");
  doc::TuningDef def;
  def.system = *tuning::Edo::make(31);
  l.run(doc::ops::addMainTuning(*l.ctx.doc(), l.ctx.store().ids(), "31", def, doc::Colour{0x3060C0}));
  const doc::TuningId t31 = l.ctx.doc()->tunings->back()->id;
  auto saved = saveTuningToLibrary(l.ctx, TuningTarget::forMain(t31));
  REQUIRE(saved.ok());
  CHECK(l.exists("Tunings\\31.pqtuning"));
  l.reindex();
  if (l.browser().list().renaming()) l.browser().list().renameField()->onCancel();
  HotSwapTarget h;
  h.kind = HotSwapTarget::Kind::Tuning;
  h.tuning = TuningTarget::forTrack(l.keys);
  h.label = "Keys";
  REQUIRE(l.ctx.library().requestHotSwap(h));
  BrowserPanel& b = l.browser();
  REQUIRE(l.names() == std::vector<std::string>{"31.pqtuning"});
  const size_t h0 = l.history();
  b.select(0);
  CHECK(l.history() == h0 + 1);
  const doc::Track& k = l.track(l.keys);
  REQUIRE(k.tuning.mode == doc::TuningMode::Private);
  REQUIRE(k.tuning.privateDef->edo() != nullptr);
  CHECK(k.tuning.privateDef->edo()->n() == 31);
  // Without a hot-swap target, Enter adds it as a new main tuning.
  b.setHotSwap({});
  const size_t tunings = l.ctx.doc()->tunings->size();
  auto r = loadTuningItem(l.ctx, *saved, nullptr);
  REQUIRE(r.ok());
  CHECK(l.ctx.doc()->tunings->size() == tunings + 1);
}

TEST_CASE("user library: clips save and load (an audio clip copies its sample); clips dropped on a folder save there") {
  Lib l("clips");
  // A note clip and an audio clip (the fixture sample).
  const doc::ClipRef nc = l.run(doc::ops::insertEmptyClip(*l.ctx.doc(), l.ctx.store().ids(), l.keys, 0, kPpq * 4, "Riff")).clips[0];
  const std::string wav = views::test::fixtureDir("audio") + "/samples/vox.wav";
  REQUIRE(fs::exists(fs::path(std::u8string(wav.begin(), wav.end()))));
  doc::SampleRef s;
  s.id = l.ctx.store().ids().sample();
  s.path = wav;
  s.frames = 72000;
  s.rate = 48000.0;
  s.channels = 1;
  s.displayName = "vox";
  const doc::ops::NewAudioClip na{l.audio, 0, s.id, "Kick"};
  const doc::ClipRef ac = l.run(doc::ops::addAudioClips(*l.ctx.doc(), l.ctx.store().ids(), std::span<const doc::SampleRef>(&s, 1),
                                                        std::span<const doc::ops::NewAudioClip>(&na, 1))).clips[0];
  // Dropped on the browser (a Clips payload, as the arrangement makes it): saved into the User Library's Clips folder.
  DragPayload p;
  p.kind = DragKind::Clips;
  p.ids = {packClipRef(nc), packClipRef(ac)};
  const std::vector<std::string> made = l.browser().saveDroppedClips(p, l.path("Clips"), false);
  REQUIRE(made.size() == 2);
  CHECK(l.exists("Clips\\Riff.pqclip"));
  CHECK(l.exists("Clips\\Kick.pqclip"));
  CHECK(l.exists("Clips\\Samples\\vox.wav"));
  auto back = l.ctx.library().readClip(l.path("Clips\\Kick.pqclip"));
  REQUIRE(back.ok());
  REQUIRE(back->sample.has_value());
  const std::string copied = l.path("Clips\\Samples\\vox.wav");
  CHECK(fs::equivalent(fs::path(std::u8string(back->sample->path.begin(), back->sample->path.end())),
                       fs::path(std::u8string(copied.begin(), copied.end()))));
  // Loaded at the start marker: the audio clip onto the audio track (its sample joins the table), one undo step.
  const size_t h0 = l.history();
  auto placed = loadClipItem(l.ctx, l.path("Clips\\Kick.pqclip"), l.audio, kPpq * 8);
  REQUIRE(placed.ok());
  CHECK(l.history() == h0 + 1);
  CHECK(l.track(l.audio).clips.size() == 2);
  CHECK(doc::samplesOf(*l.ctx.doc()).size() == 2);
  // The note clip onto the audio track: a new MIDI track after it takes it (one undo step together).
  const size_t tracks = l.ctx.doc()->tracks.size();
  auto riff = loadClipItem(l.ctx, l.path("Clips\\Riff.pqclip"), l.audio, 0);
  REQUIRE(riff.ok());
  CHECK(l.history() == h0 + 2);
  CHECK(l.ctx.doc()->tracks.size() == tracks + 1);
  // Alt (render) without a renderer: refused with a status message, nothing written.
  CHECK(l.browser().saveDroppedClips(p, l.path("Clips"), true).empty());
}

TEST_CASE("arrangement: clips dragged out of the arrangement become a Clips drag session") {
  Lib l("dragout");
  const doc::ClipRef nc = l.run(doc::ops::insertEmptyClip(*l.ctx.doc(), l.ctx.store().ids(), l.keys, 0, kPpq * 4, "Riff")).clips[0];
  auto* arr = dynamic_cast<ArrangementView*>(l.d.find("main/body/top/track-area/arrangement"));
  REQUIRE(arr != nullptr);
  l.d.tree().layoutNow();
  int32_t row = -1;
  for (size_t i = 0; i < arr->rows().size(); ++i)
    if (arr->rows()[i].id == l.keys) row = static_cast<int32_t>(i);
  REQUIRE(row >= 0);
  const IRect lane = arr->rowLaneRect(row);
  const Vec2 from{arr->xOfTick(kPpq) , static_cast<float>(lane.y + 4)};
  const size_t h0 = l.history();
  l.d.move(from);
  l.d.down(from);
  l.d.move({from.x + 30, from.y});
  CHECK_FALSE(l.d.tree().dragging());  // still a move inside the arrangement
  // Over the browser: the move becomes a Clips payload.
  BrowserPanel& b = l.browser();
  const IRect br = b.listWidget().rect();
  const Vec2 over{static_cast<float>(br.x + br.w / 2), static_cast<float>(br.y + br.h - 4)};
  l.d.move(over);
  REQUIRE(l.d.tree().dragging());
  CHECK(l.d.tree().dragPayload()->kind == DragKind::Clips);
  REQUIRE(l.d.tree().dragPayload()->ids.size() == 1);
  CHECK(unpackClipRef(l.d.tree().dragPayload()->ids[0]).clip == nc.clip);
  CHECK(l.d.tree().dropFeedback().effect == DropEffect::Copy);
  l.d.up(over);
  CHECK(l.history() == h0);  // the clip did not move
  CHECK(l.track(l.keys).clips.at(0).start() == 0);
  CHECK(l.exists("Riff.pqclip"));  // the place shown (the User Library's root)
  // Leaving the window hands the payload to the drag-out handler (the app's OLE drag).
  std::vector<DragPayload> out;
  l.d.tree().setDragOutHandler([&](DragPayload p) { out.push_back(std::move(p)); });
  l.d.move(from);
  l.d.down(from);
  l.d.move({from.x + 30, from.y});
  l.d.move(over);
  REQUIRE(l.d.tree().dragging());
  l.d.move({-20.0f, over.y});
  CHECK_FALSE(l.d.tree().dragging());
  REQUIRE(out.size() == 1);
  CHECK(out[0].kind == DragKind::Clips);
  l.d.up({-20.0f, over.y});
  l.d.tree().setDragOutHandler(nullptr);
}

TEST_CASE("browser: filter chips, tag chips and tag editing") {
  Lib l("chips");
  // Two samples of different lengths in the User Library.
  const std::string src = views::test::fixtureDir("audio") + "/samples";
  std::error_code ec;
  const std::string samplesDir = l.path("Samples");
  fs::create_directories(fs::path(std::u8string(samplesDir.begin(), samplesDir.end())), ec);
  for (const char* f : {"beat.wav", "vox.wav"}) {
    const std::string from = src + "/" + f, to = l.path(std::string("Samples\\") + f);
    fs::copy_file(fs::path(std::u8string(from.begin(), from.end())), fs::path(std::u8string(to.begin(), to.end())), ec);
    REQUIRE_FALSE(ec);
  }
  l.reindex();
  BrowserPanel& b = l.browser();
  b.setSource({BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::Samples)});
  const size_t all = l.names().size();
  REQUIRE(all == 2);
  // Duration < 1 s: none; 1–10 s: both. Mono: only the vox.
  BrowserFilters f;
  f.duration = 1;
  b.setFilters(f);
  CHECK(l.names().empty());
  f.duration = 2;
  b.setFilters(f);
  CHECK(l.names().size() == 2);
  b.setFilters({});
  // Clicking the channels chip steps it to Mono; a right-click resets it.
  l.d.tree().layoutNow();
  const IRect chip = b.filterChips().chipRect(3);
  REQUIRE_FALSE(chip.empty());
  l.d.click({static_cast<float>(chip.x + 2), static_cast<float>(chip.y + chip.h / 2)});
  CHECK(b.filters().channels == 1);
  CHECK(l.names() == std::vector<std::string>{"vox.wav"});
  CHECK(filterChipLabel(0, 2) == "1\xE2\x80\x93" "10 s");
  l.d.down({static_cast<float>(chip.x + 2), static_cast<float>(chip.y + chip.h / 2)}, PointerButton::Right);
  l.d.up({static_cast<float>(chip.x + 2), static_cast<float>(chip.y + chip.h / 2)}, PointerButton::Right);
  CHECK(b.filters().channels == 0);
  CHECK(l.names().size() == all);
  // A tag typed into the row (the context menu's New Tag…), then its chip filters.
  const int32_t loop = b.rowOf(l.path("Samples\\beat.wav"));
  REQUIRE(loop >= 0);
  b.list().beginTagEdit(loop);
  REQUIRE(b.list().renaming());
  b.list().renameField()->setText("drums");
  b.list().renameField()->onCommit("drums");
  REQUIRE(b.allTags() == std::vector<std::string>{"drums"});
  l.d.step();
  l.d.tree().layoutNow();
  const IRect tag = b.filterChips().chipRect(4);
  REQUIRE_FALSE(tag.empty());
  CHECK(b.filterChips().chipLabel(4) == "#drums");
  l.d.click({static_cast<float>(tag.x + 2), static_cast<float>(tag.y + tag.h / 2)});
  CHECK(b.filters().tags == std::vector<std::string>{"drums"});
  CHECK(l.names() == std::vector<std::string>{"beat.wav"});
}

TEST_CASE("browser: the right edge resizes the browser (the width is a view preference)") {
  Lib l("width");
  l.d.tree().layoutNow();
  Widget* handle = l.d.find("main/body/top/browser/resize");
  REQUIRE(handle != nullptr);
  const IRect slot = l.d.rect("main/body/top/browser");
  const float s = l.d.tree().scale();
  const Vec2 at{static_cast<float>(handle->rect().x + 1), static_cast<float>(handle->rect().y + 40)};
  l.d.drag(at, {at.x + 100.0f * s, at.y});
  l.d.tree().layoutNow();
  CHECK(l.d.rect("main/body/top/browser").w == slot.w + static_cast<int32_t>(std::lround(100.0f * s)));
  CHECK(l.ctx.viewPrefs().browserWidth == doctest::Approx(440.0f + 100.0f));
  const Vec2 at2{static_cast<float>(handle->rect().x + 1), at.y};
  l.d.drag(at2, {at2.x - 2000.0f, at2.y});  // clamped
  CHECK(l.ctx.viewPrefs().browserWidth == doctest::Approx(300.0f));
}

TEST_CASE("user library: a saved file reaches the browser through the indexer's watcher") {
  Lib l("watch");
  library::IndexerConfig ic;
  ic.places = l.ctx.library().places();
  library::Indexer indexer(ic);
  indexer.start();
  REQUIRE(indexer.waitIdle(std::chrono::seconds(10)));
  l.ctx.library().setIndexer(&indexer);
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(l.chain().savePreset(l.sat, false));
  const std::string file = l.path("Presets\\Saturator\\Saturator.pqpreset");
  double ms = -1;
  for (int i = 0; i < 1000 && ms < 0; ++i) {
    if (const auto idx = indexer.index(); idx && idx->findPath(file) != library::Index::kNone)
      ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    else std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(ms >= 0);
  MESSAGE("saved preset indexed after " << ms << " ms");
  l.ctx.library().indexChanged();
  CHECK(l.browser().selectedPath() == file);  // the pending rename found it
  l.ctx.library().setIndexer(nullptr);
  indexer.stop();
}

TEST_CASE("user library: a saved file shows in the browser within 500 ms" * doctest::test_suite("perf")) {
  Lib l("watch_perf");
  library::IndexerConfig ic;
  ic.places = l.ctx.library().places();
  library::Indexer indexer(ic);
  indexer.start();
  REQUIRE(indexer.waitIdle(std::chrono::seconds(10)));
  std::vector<double> ms;
  for (int k = 0; k < 5; ++k) {
    const auto t0 = std::chrono::steady_clock::now();
    auto path = l.ctx.library().savePreset(*l.device(l.sat), "Saturator");
    REQUIRE(path.ok());
    double got = -1;
    for (int i = 0; i < 2000 && got < 0; ++i) {
      if (const auto idx = indexer.index(); idx && idx->findPath(*path) != library::Index::kNone)
        got = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ms.push_back(got);
  }
  std::sort(ms.begin(), ms.end());
  MESSAGE("save → indexed: median " << ms[ms.size() / 2] << " ms, max " << ms.back() << " ms");
  CHECK(ms.back() >= 0);
  CHECK(ms.back() <= 500.0);
  indexer.stop();
}

TEST_CASE("render: browser-hotswap (a device's presets in hot-swap mode) matches the golden") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize("browser-hotswap", w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene("browser-hotswap", views::test::fixtureDir("audio"), w, h, {}, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/browser-hotswap.png";
  auto ref = readPng(path);
  if (!ref) (void)writePng("browser-hotswap.actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden("browser-hotswap", *img, *ref, true, &d));
  MESSAGE("browser-hotswap: " << pq::ui::test::describe(d));
}
