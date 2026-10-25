// Track commands of the arrangement (T3.10): insert MIDI / return tracks after the selection (Ctrl+Shift+T, Ctrl+Alt+T),
// group / ungroup (Ctrl+G, Ctrl+Shift+G), rename in place (Ctrl+R), delete, colour (the palette popover), and the
// headers' right-click menu. Each edit is one pq::doc::ops builder = one undo step; the resulting tracks become the
// selection.

#include <algorithm>
#include <cmath>

#include "../common/colour_grid.h"
#include "../common/paint.h"
#include "internal.h"
#include "pq/ui/theme.h"

namespace pq::views::arrangement {

using namespace pq::ui;


void Editor::selectTracks(std::vector<doc::TrackId> tracks) {
  using Selection = ArrangementView::Selection;
  Selection s;
  s.tracks = std::move(tracks);
  setSelection(std::move(s), false);
}

bool Editor::newTrack(doc::TrackKind kind) {
  const doc::Project& p = *ctx_.doc();
  // After the last selected track of the same list (main tree / returns), else at the end.
  doc::TrackId after;
  for (auto it = sel_.tracks.rbegin(); it != sel_.tracks.rend() && !after.valid(); ++it) {
    const auto path = doc::findTrack(p, *it);
    if (!path) continue;
    if (kind == doc::TrackKind::Return ? path->list == doc::TrackList::Returns : path->list == doc::TrackList::Main) after = *it;
  }
  // The next colour of the palette's vivid row (Live gives each new track another colour).
  uint32_t count = 0;
  doc::forEachTrack(p, [&](const doc::Track&, const doc::Track*) { ++count; });
  doc::Colour colour{0x808080};
  if (const PaintResources* res = view_.tree() ? view_.tree()->resources() : nullptr; res && res->theme && !res->theme->clipPalette.empty()) {
    const Rgba c = res->theme->clipPalette[(count * 5) % std::min<size_t>(res->theme->clipPalette.size(), kClipPaletteColumns - 1)];
    colour.rgb = (static_cast<uint32_t>(redOf(c)) << 16) | (static_cast<uint32_t>(greenOf(c)) << 8) | blueOf(c);
  }
  const auto e = doc::ops::createTrack(p, ctx_.store().ids(), kind, after, {}, colour);
  const char* what = kind == doc::TrackKind::Return ? "Insert Return Track" : "Insert MIDI Track";
  if (!apply(e, what, false)) return false;
  selectTracks(e->tracks);
  return true;
}

bool Editor::groupSelection() {
  if (sel_.tracks.empty()) return false;
  const auto e = doc::ops::groupTracks(*ctx_.doc(), ctx_.store().ids(), sel_.tracks);
  if (!apply(e, "Group Tracks", false)) return false;
  selectTracks(e->tracks);
  return true;
}

bool Editor::ungroupSelection() {
  const doc::Project& p = *ctx_.doc();
  std::vector<doc::TrackId> groups;
  for (doc::TrackId id : sel_.tracks)
    if (const doc::Track* t = doc::findTrackPtr(p, id); t && t->kind == doc::TrackKind::Group) groups.push_back(id);
  if (groups.empty()) return false;
  const auto e = doc::ops::ungroupTracks(p, groups);
  if (!apply(e, "Ungroup Tracks", false)) return false;
  selectTracks(e->tracks);
  return true;
}

bool Editor::deleteTracks() {
  const doc::Project& p = *ctx_.doc();
  std::vector<doc::TrackId> tracks;
  for (doc::TrackId id : sel_.tracks)
    if (const doc::Track* t = doc::findTrackPtr(p, id); t && t->kind != doc::TrackKind::Master) tracks.push_back(id);
  if (tracks.empty()) return false;
  if (!apply(doc::ops::deleteTracks(p, tracks), tracks.size() == 1 ? "Delete Track" : "Delete Tracks", false)) return false;
  selectTracks({});
  return true;
}

bool Editor::renameSelection() {
  for (doc::TrackId id : sel_.tracks)
    if (TrackHeader* h = view_.header(id)) {
      h->beginRename();
      return true;
    }
  ctx_.setStatus("Rename: the track is not shown (unfold its group)");
  return false;
}

bool Editor::setTracksColour(doc::Colour c) {
  if (sel_.tracks.empty()) return false;
  return apply(doc::ops::setTracksColour(*ctx_.doc(), sel_.tracks, c), "Track Colour", false);
}

Menu* Editor::openTrackMenu(doc::TrackId id, Vec2 at) {
  auto action = [](std::string label, std::string actionId, std::string keys) {
    MenuItem m = MenuItem::item(std::move(label), {}, std::move(keys));
    m.action = std::move(actionId);
    return m;
  };
  const doc::Track* t = doc::findTrackPtr(*ctx_.doc(), id);
  const bool master = t && t->kind == doc::TrackKind::Master;
  std::vector<MenuItem> items;
  items.push_back(action("Rename", "arrangement.rename", "Ctrl+R"));
  items.push_back(MenuItem::sep());
  items.push_back(action("Insert MIDI Track", "track.newMidi", "Ctrl+Shift+T"));
  items.push_back(action("Insert Return Track", "track.newReturn", "Ctrl+Alt+T"));
  items.push_back(MenuItem::sep());
  items.push_back(action("Group Tracks", "track.group", "Ctrl+G"));
  items.push_back(action("Ungroup Tracks", "track.ungroup", "Ctrl+Shift+G"));
  items.push_back(MenuItem::sep());
  items.push_back(MenuItem::item("Colour…", [this, at] { (void)openColourPicker(at); }));
  items.push_back(MenuItem::sep());
  MenuItem del = MenuItem::item("Delete", [this] { (void)deleteTracks(); }, "Delete");
  del.enabled = !master;
  items.push_back(std::move(del));
  return ui::openContextMenu(view_, at, std::move(items));
}

Widget* Editor::openColourPicker(Vec2 at) {
  OverlayLayer* layer = OverlayLayer::of(view_);
  if (!layer || sel_.tracks.empty()) return nullptr;
  const doc::Track* first = doc::findTrackPtr(*ctx_.doc(), sel_.tracks.front());
  auto pop = std::make_unique<Popover>(layer->uniqueName("track-colours"));
  Popover* popPtr = pop.get();
  pop->add<paint::ColourGrid>(first ? first->colour.rgb : 0u, [this, layer, popPtr](uint32_t rgb) {
    (void)setTracksColour(doc::Colour{rgb});
    layer->close(*popPtr);
  });
  const float w = paint::colourGridWidth() + 16.0f;
  const float h = paint::colourGridHeight() + 16.0f;
  pop->setSize(Length::fixed(w), Length::fixed(h));
  OverlayOptions o;
  o.anchor = {static_cast<int32_t>(std::lround(at.x)), static_cast<int32_t>(std::lround(at.y)), 0, 0};
  o.placement = Placement::AtPoint;
  return &layer->open(std::move(pop), std::move(o));
}

}  // namespace pq::views::arrangement
