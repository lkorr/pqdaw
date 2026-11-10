// GU.3 (docs/plans/GU.md "Phases" 3, "Decided"): group panels in the document — the model's rules, SetGroupPanel and
// its builders (one Meta step each), the final pass that takes elements off with their blocks (deleted, moved out,
// moved away: one undo step restores both), save / load (schema 21) and the 20 → 21 migration, a panel carried by
// copy / paste (bindings remapped) and by group preset files, and the instrument group's polyphony as settings.
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/library_files.h"
#include "pq/document/main_path.h"
#include "pq/document/panel.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"

using namespace pq::doc;
using pq::Result;

namespace {

struct Doc {
  DocumentStore store;
  TrackId keys;
  Doc() { keys = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi, {}, "Keys")).tracks.at(0); }
  const Project& doc() const { return *store.current(); }
  const Track& track() const { return *findTrackPtr(doc(), keys); }
  ops::Edit edit(const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const size_t before = store.history().cursor();
    REQUIRE(store.dispatch(e->command).ok());
    CHECK(store.history().cursor() == before + (e->empty() ? 0u : 1u));  // one undo step
    return *e;
  }
  void cmd(Command c) {
    const pq::Status st = store.dispatch(std::move(c));
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  DeviceId block(std::string patch, DeviceId group = {}) {
    const DeviceId d = edit(ops::addDevice(doc(), store.ids(), keys, std::move(patch), DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
    if (group.valid()) cmd(SetDeviceGroup{d, group});
    return d;
  }
  DeviceId group() const { return mainPath(doc(), track()).blocks.at(0)->id; }  // the instrument group
  const Device& dev(DeviceId id) const { return *findDevice(doc(), id).device; }
  const Panel* panel(DeviceId g) const { return dev(g).panel.get(); }
};

PanelElement element(std::string kind, float x, float y, float w, float h, std::vector<Binding> b = {}) {
  PanelElement e;
  e.kind = std::move(kind);
  e.x = x, e.y = y, e.w = w, e.h = h;
  e.bindings = std::move(b);
  return e;
}

// A group holding a value block (v) and, one level down, a nested group (n) holding another value block (deep); the
// panel: a knob on v, a number on deep's control, a toggle on the group's own "mono" setting, a label, a tabs element
// and a knob shown on tab 1.
struct PanelDoc : Doc {
  DeviceId g, v, n, deep;
  std::vector<uint32_t> ids;
  PanelDoc() {
    g = group();
    v = block(std::string(kValueDevice), g);
    n = block(std::string(kGroupDevice), g);
    deep = block(std::string(kValueDevice), n);
    std::vector<PanelElement> es;
    es.push_back(element("knob", 8, 8, 48, 56, {controlBinding(v, std::string(kValueParam))}));
    es.push_back(element("number", 64, 8, 56, 20, {controlBinding(deep, std::string(kValueParam))}));
    es.push_back(element("toggle", 64, 32, 56, 20, {settingBinding(g, std::string(kGroupMono))}));
    es.push_back(element("label", 8, 70, 80, 14));
    es.back().setField(styleText("text", "Osc"));
    es.push_back(element("tabs", 130, 8, 80, 18));
    es.back().setField(styleText("items", "A|B"));
    es.push_back(element("knob", 130, 30, 48, 56, {controlBinding(v, std::string(kValueParam))}));
    edit(ops::addPanelElements(doc(), g, std::move(es), &ids));
    PanelElement shown = *panel(g)->find(ids[5]);
    shown.when = Visibility{ids[4], {1.0}};
    edit(ops::setPanelElement(doc(), g, std::move(shown)));
  }
};

}  // namespace

TEST_CASE("panels: the model's rules — ids, kinds, rects, style fields, bindings, conditions; groups only; bindings "
          "inside the group (any depth) or the group's own settings") {
  Panel p;
  CHECK(checkPanel(p).ok());  // empty: none
  p.elements.push_back(element("knob", 0, 0, 10, 10));
  CHECK_FALSE(checkPanel(p).ok());  // elements need a size
  p.width = 100, p.height = 50;
  CHECK_FALSE(checkPanel(p).ok());  // id 0
  p.elements[0].id = 3;
  CHECK(checkPanel(p).ok());
  {
    Panel q = p;
    q.elements.push_back(q.elements[0]);
    CHECK_FALSE(checkPanel(q).ok());  // an id twice
    q.elements[1].id = 4;
    q.elements[1].kind.clear();
    CHECK_FALSE(checkPanel(q).ok());  // no kind
    q.elements[1].kind = "label";
    q.elements[1].w = 0;
    CHECK_FALSE(checkPanel(q).ok());  // no width
    q.elements[1].w = 5;
    q.elements[1].when = Visibility{9, {0}};
    CHECK_FALSE(checkPanel(q).ok());  // a condition naming no element
    q.elements[1].when = Visibility{4, {0}};
    CHECK_FALSE(checkPanel(q).ok());  // itself
    q.elements[1].when = Visibility{3, {}};
    CHECK_FALSE(checkPanel(q).ok());  // no values
    q.elements[1].when = Visibility{3, {0, 1}};
    CHECK(checkPanel(q).ok());
    q.elements[1].bindings.push_back(Binding{BindingKind::Control, DeviceId{5}, {}, {}});
    CHECK_FALSE(checkPanel(q).ok());  // a control without a name
    q.elements[1].bindings[0].name = "value";
    CHECK(checkPanel(q).ok());
    q.elements[1].bindings.push_back(Binding{BindingKind::Panel, DeviceId{5}, "x", {}});
    CHECK_FALSE(checkPanel(q).ok());  // a panel binding names a group, no meta path
  }
  // Fields: sorted, named, typed.
  PanelElement e = element("knob", 0, 0, 1, 1);
  e.setField(styleText("pointer", "dot"));
  e.setField(styleNumber("track", 3.0));
  e.setField(styleNumber("track", 4.0));
  REQUIRE(e.style.size() == 2);
  CHECK(e.style[0].name == "pointer");
  CHECK(e.number("track", 1.0) == 4.0);
  CHECK(e.text("pointer", "arc") == "dot");
  CHECK(e.text("track", "x") == "x");  // a number is not a text
  CHECK(e.number("missing", 7.0) == 7.0);

  PanelDoc d;
  // Only groups have panels.
  CHECK_FALSE(ops::setGroupPanel(d.doc(), d.v, std::make_shared<const Panel>(p)).ok());
  CHECK_FALSE(d.store.dispatch(SetGroupPanel{d.v, std::make_shared<const Panel>(p)}).ok());
  // Bindings: inside the group at any depth (deep), the group itself (a setting); not a block outside it, nor the
  // group's own panel as a child panel.
  const DeviceId out = d.block("atom:tanh");
  CHECK(bindingAllowed(d.track().devices, d.g, controlBinding(d.deep, "value")));
  CHECK(bindingAllowed(d.track().devices, d.g, settingBinding(d.g, "voices")));
  CHECK_FALSE(bindingAllowed(d.track().devices, d.g, controlBinding(out, "x")));
  CHECK_FALSE(bindingAllowed(d.track().devices, d.g, Binding{BindingKind::Panel, d.g, {}, {}}));
  CHECK(bindingAllowed(d.track().devices, d.g, Binding{BindingKind::Panel, d.n, {}, {}}));
  CHECK_FALSE(bindingAllowed(d.track().devices, d.g, Binding{BindingKind::Panel, d.v, {}, {}}));  // not a group
  CHECK_FALSE(bindingAllowed(d.track().devices, d.n, controlBinding(d.v, "value")));  // v is beside n, not in it
  std::vector<PanelElement> outside;
  outside.push_back(element("knob", 0, 0, 10, 10, {controlBinding(out, "x")}));
  const auto refused = ops::addPanelElements(d.doc(), d.g, std::move(outside));
  REQUIRE_FALSE(refused.ok());
  CHECK(refused.error().message.find("not inside the group") != std::string::npos);
}

TEST_CASE("panels: SetGroupPanel and its builders are one Meta step each (no compile); ids are allocated, the panel "
          "grows to hold its elements; removing an element drops the conditions naming it") {
  PanelDoc d;
  const Panel& p = *d.panel(d.g);
  REQUIRE(p.elements.size() == 6);
  CHECK(d.ids == std::vector<uint32_t>{1, 2, 3, 4, 5, 6});
  CHECK(p.width == 130 + 80 + ops::kPanelMargin);  // the tabs' right edge
  CHECK(p.height == 30 + 56 + ops::kPanelMargin);
  CHECK(p.find(6)->when == Visibility{5, {1.0}});
  // A panel edit is Meta.
  ChangeSet seen;
  const ListenerId sub = d.store.subscribe([&](const StoreEvent& e) { seen = e.changes; });
  PanelElement moved = *p.find(1);
  moved.x = 20;
  d.edit(ops::setPanelElement(d.doc(), d.g, moved));
  CHECK_FALSE(seen.structural());
  CHECK(seen.has(kChangeMeta));
  CHECK(d.panel(d.g)->find(1)->x == 20);
  // Unchanged: an empty edit.
  CHECK(ops::setPanelElement(d.doc(), d.g, moved)->empty());
  // Remove the tabs: the knob it showed has no condition any more.
  const uint32_t tabs[] = {5};
  d.edit(ops::removePanelElements(d.doc(), d.g, tabs));
  CHECK(d.panel(d.g)->find(5) == nullptr);
  CHECK_FALSE(d.panel(d.g)->find(6)->when.has_value());
  // Size.
  d.edit(ops::setPanelSize(d.doc(), d.g, 400, 120));
  CHECK(d.panel(d.g)->width == 400);
  CHECK_FALSE(ops::setPanelSize(d.doc(), d.g, 0, 120).ok());
  // Every element removed: no panel (the group is its mini circuit again); undo brings it back.
  std::vector<uint32_t> all;
  for (const PanelElement& e : d.panel(d.g)->elements) all.push_back(e.id);
  d.edit(ops::removePanelElements(d.doc(), d.g, all));
  CHECK(d.dev(d.g).panel == nullptr);
  CHECK_FALSE(d.dev(d.g).hasPanel());
  REQUIRE(d.store.undo());
  CHECK(d.dev(d.g).hasPanel());
  // A taken id is refused.
  std::vector<PanelElement> clash;
  clash.push_back(element("label", 0, 0, 5, 5));
  clash.back().id = 1;
  CHECK_FALSE(ops::addPanelElements(d.doc(), d.g, std::move(clash)).ok());
  d.store.unsubscribe(sub);
}

TEST_CASE("panels: deleting a bound block takes its elements off in the same command (one undo step brings both back); "
          "a block moved out of the group or to another track takes its elements off; deleting the group takes the panel") {
  SUBCASE("delete") {
    PanelDoc d;
    const Snapshot before = d.store.current();
    d.edit(ops::deleteDevices(d.doc(), std::vector<DeviceId>{d.v}));
    REQUIRE(d.panel(d.g) != nullptr);
    CHECK(d.panel(d.g)->elements.size() == 4);  // both knobs on v gone
    CHECK_FALSE(panelBinds(*d.panel(d.g), d.v));
    REQUIRE(d.store.undo());
    CHECK(d.store.current().get() == before.get());
    CHECK(d.panel(d.g)->elements.size() == 6);
    // A nested group deleted: its contents go, and with them the number on `deep`.
    d.edit(ops::deleteDevices(d.doc(), std::vector<DeviceId>{d.n}));
    CHECK(d.panel(d.g)->elements.size() == 5);
    CHECK_FALSE(panelBinds(*d.panel(d.g), d.deep));
  }
  SUBCASE("moved out of the group") {
    PanelDoc d;
    d.cmd(SetDeviceGroup{d.deep, d.g});  // one level up: still inside g
    CHECK(d.panel(d.g)->elements.size() == 6);
    d.cmd(SetDeviceGroup{d.deep, {}});  // out of the group altogether
    CHECK(d.panel(d.g)->elements.size() == 5);
    REQUIRE(d.store.undo());
    CHECK(d.panel(d.g)->elements.size() == 6);
  }
  SUBCASE("moved to another track") {
    PanelDoc d;
    const TrackId other = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Midi, {}, "Other")).tracks.at(0);
    // (a member leaves its group first: MoveDevice keeps the tag, and a group tag names a group of the same track)
    const size_t h0 = d.store.history().cursor();
    d.cmd(Batch{"Move", {SetDeviceGroup{d.v, {}}, MoveDevice{d.v, other, kAppend}}});
    CHECK(d.store.history().cursor() == h0 + 1);
    REQUIRE(findDevice(d.doc(), d.v).track->id == other);
    CHECK(d.panel(d.g)->elements.size() == 4);
    REQUIRE(d.store.undo());
    CHECK(d.panel(d.g)->elements.size() == 6);
  }
  SUBCASE("the group's own setting stays; the group deleted takes everything") {
    PanelDoc d;
    d.edit(ops::deleteDevices(d.doc(), std::vector<DeviceId>{d.v, d.n}));
    REQUIRE(d.panel(d.g) != nullptr);
    CHECK(d.panel(d.g)->elements.size() == 3);  // the toggle on mono, the label, the tabs
    d.edit(ops::deleteDevices(d.doc(), std::vector<DeviceId>{d.g}));
    CHECK(findDevice(d.doc(), d.g).device == nullptr);
  }
}

TEST_CASE("panels: save / load round trip (schema 21); a schema-20 file migrates; the reader refuses a panel on a "
          "block that is not a group and a binding outside the group") {
  PanelDoc d;
  PanelElement styled = *d.panel(d.g)->find(1);
  styled.setField(styleText("pointer", "pie"));
  styled.setField(styleNumber("track", 4.5));
  styled.setField(styleText("colour", "signal"));
  styled.bindings.push_back(Binding{BindingKind::Signal, d.v, {}, "y"});
  styled.bindings.push_back(Binding{});  // an unbound slot keeps its place
  d.edit(ops::setPanelElement(d.doc(), d.g, styled));
  auto text = serializeProject(d.doc());
  REQUIRE(text.ok());
  CHECK(text->rfind("{\n  \"schemaVersion\": " + std::to_string(kSchemaVersion) + ",", 0) == 0);
  CHECK(text->find("\"panel\": {") != std::string::npos);
  auto back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  const Device* g2 = findDevice(*back->project, d.g).device;
  REQUIRE(g2 != nullptr);
  REQUIRE(g2->panel != nullptr);
  CHECK(*g2->panel == *d.panel(d.g));
  CHECK(*back->project == d.doc());
  // The same file as schema 20 (no panel key existed then): migrates (20 → 21 changes nothing).
  {
    auto plain = serializeProject(*[&] {
      Doc e;
      return e.store.current();
    }());
    REQUIRE(plain.ok());
    std::string old = *plain;
    old.replace(old.find("\"schemaVersion\": " + std::to_string(kSchemaVersion)), 19, "\"schemaVersion\": 20");
    auto m = parseProject(old);
    REQUIRE_MESSAGE(m.ok(), (m.ok() ? std::string() : m.error().message));
    CHECK(m->fileSchemaVersion == 20);
  }
  // Refused: a binding to a block outside the group.
  {
    const DeviceId out = d.block("atom:tanh");
    std::string bad = *serializeProject(d.doc());
    const std::string from = "\"device\": " + std::to_string(d.v.value), to = "\"device\": " + std::to_string(out.value);
    const size_t at = bad.find(from, bad.find("\"panel\""));
    REQUIRE(at != std::string::npos);
    bad.replace(at, from.size(), to);
    auto r = parseProject(bad);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("not inside the group") != std::string::npos);
  }
  // Refused: a panel on a value block.
  {
    std::string bad = *serializeProject(d.doc());
    const std::string key = "\"patch\": \"Value\"";
    const size_t at = bad.find(key);
    REQUIRE(at != std::string::npos);
    bad.insert(at, "\"panel\": {\"size\": [10, 10], \"elements\": [{\"id\": 1, \"kind\": \"label\", \"rect\": [0, 0, 5, 5]}]}, ");
    CHECK_FALSE(parseProject(bad).ok());
  }
}

TEST_CASE("panels: copy / paste carries the panel with fresh bindings (remapped to the copies); a group preset file "
          "round trips it") {
  PanelDoc d;
  const DeviceId pick[] = {d.g};
  Result<BlockSet> set = ops::copyBlocks(d.doc(), pick);
  REQUIRE(set.ok());
  REQUIRE(set->devices.front()->panel != nullptr);
  const TrackId other = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Midi, {}, "Other")).tracks.at(0);
  const ops::Edit e = d.edit(ops::pasteBlocks(d.doc(), d.store.ids(), other, *set));
  REQUIRE(e.devices.size() == set->devices.size());
  const DeviceId g2 = e.devices[0];
  const Device& copy = d.dev(g2);
  REQUIRE(copy.panel != nullptr);
  REQUIRE(copy.panel->elements.size() == 6);  // nothing pruned: every binding found its copy inside the new group
  const std::vector<DevicePtr>& list = findTrackPtr(d.doc(), other)->devices;
  for (const PanelElement& el : copy.panel->elements)
    for (const Binding& b : el.bindings) {
      CHECK(bindingAllowed(list, g2, b));
      CHECK(b.device != d.v);
      CHECK(b.device != d.deep);
      CHECK(b.device != d.g);
    }
  CHECK(copy.panel->find(3)->bindings[0].device == g2);  // the group's own setting follows the group
  CHECK(copy.panel->find(6)->when == Visibility{5, {1.0}});  // element ids are the panel's own: kept
  // A group preset holds it.
  auto text = serializeGroupPreset(*set, "Panelled");
  REQUIRE(text.ok());
  auto back = parsePreset(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(back->blocks == *set);
  REQUIRE(back->device->panel != nullptr);
  CHECK(*back->device->panel == *d.panel(d.g));
}

TEST_CASE("panels: an instrument group's polyphony is its settings — specs for the inspector and panels; an edit is "
          "one Structural step (one recompile)") {
  PanelDoc d;
  REQUIRE(groupSettings().size() == 5);
  const SettingSpec* voices = groupSetting(kGroupVoices);
  REQUIRE(voices != nullptr);
  CHECK(voices->min == 1.0);
  CHECK(voices->max == 128.0);
  CHECK(voices->def == kDefaultGroupVoices);
  CHECK(groupSetting(kGroupMono)->isSwitch());
  CHECK(groupSetting(kGroupSteal)->choices.size() == 3);
  CHECK(groupSetting(kGroupGlide)->unit == "s");
  CHECK(groupSetting("nope") == nullptr);
  int structural = 0;
  const ListenerId sub = d.store.subscribe([&](const StoreEvent& e) { structural += e.changes.structural() ? 1 : 0; });
  d.edit(ops::setDeviceSetting(d.doc(), d.g, std::string(kGroupVoices), 6.0));
  CHECK(structural == 1);
  CHECK(groupPolyphony(d.dev(d.g)).voices == 6);
  d.store.unsubscribe(sub);
}

TEST_CASE("panels (GU.5): a group's panel window is view state — saved as the view's \"panels\" and read back exactly; "
          "a window of a group that is gone is not written; the reader refuses one naming no group or a place out of "
          "range") {
  PanelDoc d;
  PanelWindowView w;
  w.open = true;
  w.edit = true;
  w.x = 412.0f, w.y = 36.5f, w.w = 520.0f, w.h = 300.0f;
  ViewState view;
  view.panelWindows[d.g.value] = w;
  PanelWindowView m;
  m.minimised = true;  // closed but placed and minimised: kept
  m.w = 200.0f, m.h = 120.0f;
  view.panelWindows[d.n.value] = m;
  view.panelWindows[999999u] = w;  // no such group: dropped at the write
  auto text = serializeProject(d.doc(), view);
  REQUIRE(text.ok());
  CHECK(text->find("\"panels\"") != std::string::npos);
  auto back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(back->view.panelWindows.size() == 2);
  CHECK(back->view.panelWindows.at(d.g.value) == w);
  CHECK(back->view.panelWindows.at(d.n.value) == m);
  // Without windows the file has no "panels" key (older files unchanged).
  auto plain = serializeProject(d.doc(), ViewState{});
  REQUIRE(plain.ok());
  CHECK(plain->find("\"panels\"") == std::string::npos);
  // A window on a block that is no group, and a size out of range: refused.
  {
    std::string bad = *text;
    const std::string key = "\"id\": " + std::to_string(d.g.value) + ", \"open\"";
    const size_t at = bad.find(key);
    REQUIRE(at != std::string::npos);
    bad.replace(at, key.size(), "\"id\": " + std::to_string(d.v.value) + ", \"open\"");
    auto r = parseProject(bad);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("no group has this id") != std::string::npos);
  }
  {
    std::string bad = *text;
    const size_t at = bad.find("\"w\": 520");
    REQUIRE(at != std::string::npos);
    bad.replace(at, 8, "\"w\": -5");
    auto r = parseProject(bad);
    REQUIRE_FALSE(r.ok());
  }
}
