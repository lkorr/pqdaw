// What the views show at F12 (T5.x-bugreport): pq/views/bug_report/bug_report.h describeShot / describeProject. Runs
// only when the bug report key is pressed: tree walks, dynamic_cast and string formatting are fine here.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <typeinfo>
#include <vector>

#include "../shell/shell_widgets.h"
#include "pq/document/serialize.h"
#include "pq/ui/overlay.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/audio_clip/audio_clip_view.h"
#include "pq/views/browser/browser_panel.h"
#include "pq/views/bug_report/bug_report.h"
#include "pq/views/circuit/circuit_canvas.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/circuit/circuit_viewer.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/prefs/audio_prefs.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/tunings/tunings_panel.h"
#include "pq/views/view_context.h"

namespace pq::views::bugreport {

using ui::Widget;
using views::shell::formatPosition;

namespace {

bool shown(const Widget& w) {
  for (const Widget* p = &w; p; p = p->parent())
    if (!p->visible()) return false;
  return !w.rect().empty();
}

// The first shown widget of type T in the subtree (depth-first).
template <class T>
T* findShown(Widget& root) {
  if (!root.visible()) return nullptr;
  if (auto* t = dynamic_cast<T*>(&root); t && shown(root)) return t;
  for (size_t i = 0; i < root.childCount(); ++i)
    if (T* t = findShown<T>(root.child(i))) return t;
  return nullptr;
}

std::string rectText(const ui::IRect& r) {
  char b[64];
  std::snprintf(b, sizeof b, "[%d,%d %dx%d]", r.x, r.y, r.w, r.h);
  return b;
}

std::string position(const doc::Project& p, Ticks t) {
  return formatPosition(p.timeline->sigMap, t) + " (tick " + std::to_string(t) + ")";
}

std::string clipRefText(const doc::ClipRef& c) {
  return "t" + std::to_string(c.track.value) + ":c" + std::to_string(c.clip.value);
}

template <class List, class F>
std::string listOf(const List& items, size_t max, F&& f) {
  std::string s;
  size_t n = 0;
  for (const auto& it : items) {
    if (n == max) {
      s += ", …";
      break;
    }
    if (n++) s += ", ";
    s += f(it);
  }
  return s;
}

std::string tuningText(const doc::TuningDef* def) {
  if (!def) return "(dangling)";
  char b[96];
  if (const auto* e = def->edo()) {
    std::snprintf(b, sizeof b, "%u-EDO, ref %.3f Hz", e->n(), def->referenceHz);
  } else if (const auto* m = def->mos()) {
    std::snprintf(b, sizeof b, "MOS %d notes, generator %.3f c, period %.3f c, ref %.3f Hz", m->n, m->generator, m->period,
                  def->referenceHz);
  } else if (const auto* s = def->scale()) {
    std::snprintf(b, sizeof b, "Scala, %zu degrees, ref %.3f Hz", s->scale.pitches.size(), def->referenceHz);
    return std::string(b) + " \"" + s->scale.descriptionUtf8() + "\"";
  } else {
    return "?";
  }
  return b;
}

}  // namespace

std::string typeName(const Widget& w) {
  std::string n = typeid(w).name();  // "class pq::views::ArrangementView"
  for (const char* p : {"class ", "struct "})
    if (n.rfind(p, 0) == 0) n.erase(0, std::char_traits<char>::length(p));
  if (const size_t c = n.rfind("::"); c != std::string::npos) n.erase(0, c + 2);
  return n;
}

std::string widgetChain(const Widget* leaf) {
  std::vector<const Widget*> chain;
  for (const Widget* w = leaf; w; w = w->parent()) chain.push_back(w);
  std::string s;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const Widget& w = **it;
    if (w.name().empty() && *it != leaf && w.parent()) continue;  // unnamed layout wrappers: transparent (as in paths)
    if (!s.empty()) s += " > ";
    s += typeName(w);
    if (!w.name().empty()) s += " '" + w.name() + "'";
    s += " " + rectText(w.rect());
  }
  return s.empty() ? "(none)" : s;
}

std::string describeShot(Shell& shell, ui::Vec2 pointer, bool pointerInside) {
  ui::UiTree& tree = shell.tree();
  ViewContext& ctx = shell.context();
  const doc::Snapshot snap = ctx.doc();
  const doc::Project& p = *snap;
  std::string out;
  auto line = [&](std::string_view key, const std::string& value) {
    out += "- ";
    out += key;
    out += ": ";
    out += value;
    out += '\n';
  };
  char b[256];
  if (pointerInside) {
    std::snprintf(b, sizeof b, "(%.0f, %.0f) device px", static_cast<double>(pointer.x), static_cast<double>(pointer.y));
    line("pointer", b);
    line("under the pointer", widgetChain(tree.hitTest(pointer)));
  } else {
    line("pointer", "outside the window");
  }
  if (const Widget* c = tree.captured()) line("pointer captured by", c->path().empty() ? typeName(*c) : c->path());
  line("focus", tree.focus() ? widgetChain(tree.focus()) : std::string("(none)"));

  // Views: what fills the track area and the detail view, side panels, popups.
  Widget* root = tree.root();
  std::string views;
  auto add = [&](const std::string& v) {
    if (!views.empty()) views += ", ";
    views += v;
  };
  auto* arrangement = root ? findShown<ArrangementView>(*root) : nullptr;
  auto* mixer = root ? findShown<MixerView>(*root) : nullptr;
  auto* patch = root ? findShown<project_patch::ProjectPatchView>(*root) : nullptr;
  auto* clipView = root ? findShown<ClipView>(*root) : nullptr;
  auto* audioClip = root ? findShown<AudioClipView>(*root) : nullptr;
  auto* chain = root ? findShown<DeviceChainView>(*root) : nullptr;
  auto* browser = root ? findShown<BrowserPanel>(*root) : nullptr;
  auto* tunings = root ? findShown<TuningsPanel>(*root) : nullptr;
  if (patch) add("project patch");
  else if (mixer) add("mixer");
  else if (arrangement) add("arrangement");
  if (ctx.detailVisible()) {
    if (ctx.detailMode() == DetailMode::Clip) add(clipView ? "detail: clip view (piano roll)" : audioClip ? "detail: audio clip view" : "detail: clip (empty)");
    else add("detail: device chain");
  }
  if (browser) add("browser");
  if (tunings) add("tunings panel");
  if (ui::OverlayLayer* layer = ui::OverlayLayer::of(*root)) {
    for (size_t i = 0; i < layer->childCount(); ++i) {
      Widget& w = layer->child(i);
      if (!w.visible() || !layer->isOpen(w)) continue;
      std::string what = "popup " + typeName(w) + (w.name().empty() ? std::string() : " '" + w.name() + "'");
      if (auto* ce = dynamic_cast<circuit::CircuitEditor*>(&w)) what = "circuit (Device level, editor): " + ce->title();
      else if (auto* cv = dynamic_cast<circuit::CircuitViewer*>(&w)) what = "circuit (patch viewer): " + cv->title();
      else if (dynamic_cast<PreferencesWindow*>(&w)) what = "preferences";
      add(what);
    }
  }
  line("views", views.empty() ? std::string("(none)") : views);

  // Selections.
  if (arrangement) {
    const ArrangementView::Selection& s = arrangement->selection();
    std::string sel = std::to_string(s.clips.size()) + " clips";
    if (!s.clips.empty()) sel += " (" + listOf(s.clips, 8, clipRefText) + ")";
    sel += ", " + std::to_string(s.tracks.size()) + " tracks";
    if (!s.tracks.empty()) sel += " (" + listOf(s.tracks, 8, [](doc::TrackId t) { return "t" + std::to_string(t.value); }) + ")";
    if (s.time) sel += ", time " + formatPosition(p.timeline->sigMap, s.time->start) + " – " + formatPosition(p.timeline->sigMap, s.time->end) + (s.range ? " (range)" : "");
    line("arrangement selection", sel);
  }
  if (const doc::TrackId t = ctx.selectedTrack(); t.valid()) {
    const doc::Track* tr = doc::findTrackPtr(p, t);
    line("selected track", "t" + std::to_string(t.value) + (tr ? " \"" + tr->name + "\"" : std::string(" (gone)")));
  }
  if (ctx.detailClip().clip.valid()) line("clip in the clip view", clipRefText(ctx.detailClip()));
  if (clipView) {
    const auto sel = clipView->selection();
    line("piano roll", std::to_string(sel.size()) + " notes selected" +
                           (sel.empty() ? std::string() : " (" + listOf(sel, 12, [](uint32_t n) { return "n" + std::to_string(n); }) + ")"));
  }
  if (chain) {
    const auto sel = chain->selection();
    line("device chain", "track t" + std::to_string(chain->track().value) + ", " + std::to_string(sel.size()) + " devices selected" +
                             (sel.empty() ? std::string() : " (" + listOf(sel, 8, [](doc::DeviceId d) { return "d" + std::to_string(d.value); }) + ")"));
  }

  // Transport.
  {
    const PlayheadSnapshot ph = ctx.transport().playhead();
    const double bpm = p.timeline->tempoMap.bpmAt(ph.tick);
    std::string t = ph.playing ? "playing" : "stopped";
    t += " at " + position(p, ph.tick);
    std::snprintf(b, sizeof b, ", %.3f BPM", bpm);
    t += b;
    t += ", start marker " + formatPosition(p.timeline->sigMap, ctx.startMarker());
    t += ", loop " + formatPosition(p.timeline->sigMap, p.loop.start) + " – " + formatPosition(p.timeline->sigMap, p.loop.end) +
         (p.loop.enabled ? " (on)" : " (off)");
    line("transport", t);
  }
  line("grid", ctx.gridLabel() + (ctx.drawMode() ? ", draw mode on" : ""));

  // The visible editors' zoom / scroll.
  if (arrangement) {
    const TickRange v = arrangement->visibleTicks();
    std::snprintf(b, sizeof b, "%.6g logical px/tick, scroll y %d device px, visible ", arrangement->pxPerTick(), arrangement->scrollY());
    line("arrangement zoom", std::string(b) + formatPosition(p.timeline->sigMap, v.start) + " – " + formatPosition(p.timeline->sigMap, v.end));
  }
  if (clipView) {
    std::snprintf(b, sizeof b, "%.6g device px/tick, scroll tick %.0f, row height %d logical px%s", clipView->pxPerTick(),
                  clipView->scrollTick(), clipView->rowHeight(), clipView->fold() ? ", folded" : "");
    line("piano roll zoom", b);
  }
  if (auto* cc = root ? findShown<circuit::CircuitCanvas>(*root) : nullptr) {  // the first shown circuit canvas
    const auto& v = cc->view();
    std::snprintf(b, sizeof b, "zoom %.4g, origin (%.1f, %.1f) world units", v.zoom, v.x, v.y);
    line("circuit canvas '" + cc->name() + "'", b);
  }
  return out;
}

std::string describeProject(Shell& shell) {
  ViewContext& ctx = shell.context();
  const doc::Snapshot snap = ctx.doc();
  const doc::Project& p = *snap;
  size_t tracks = 0, clips = 0, devices = 0;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    ++tracks;
    clips += t.clips.size();
    doc::forEachDeviceIn(t.devices, [&](const doc::Device&) { ++devices; });
  });
  std::string out;
  out += "- project: \"" + ctx.projectName() + "\"" + (ctx.dirty() ? " (unsaved changes)" : "") + "\n";
  out += "- project path: " + (ctx.projectDir().empty() ? std::string("(untitled, never saved)") : "`" + ctx.projectDir() + "`") + "\n";
  char b[200];
  std::snprintf(b, sizeof b, "- contents: %zu tracks (incl. returns + master), %zu clips, %zu devices, %zu samples (%zu missing), %zu main tunings; schema %d\n",
                tracks, clips, devices, doc::samplesOf(p).size(), ctx.missingSamples().size(), p.tunings ? p.tunings->size() : 0,
                static_cast<int>(doc::kSchemaVersion));
  out += b;
  const doc::TrackId sel = ctx.selectedTrack();
  if (sel.valid()) {
    if (auto path = doc::findTrack(p, sel)) out += "- tuning of the selected track (t" + std::to_string(sel.value) + "): " + tuningText(doc::effectiveTuning(p, *path)) + "\n";
  } else if (p.tunings && !p.tunings->empty()) {
    if (const doc::TuningSlot* d = doc::defaultTuningOf(p))
      out += "- default tuning: \"" + d->name + "\", " + tuningText(&d->def) + "\n";
  }
  return out;
}

}  // namespace pq::views::bugreport
