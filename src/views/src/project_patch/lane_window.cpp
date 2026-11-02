// The project patch view's lane windows (T5.x-one-circuit, SPEC §15 2026-10-31): a MIDI or audio track's source node
// shows the lane itself over the view's time window — bar and beat lines, the clips in their colours with their
// names, MIDI notes, audio waveforms, the playhead. See pq/views/project_patch/project_patch_view.h.
//
// TX.lane-edit: with the arrangement next to the view (the app, its scenes) a window is the arrangement's editing surface
// (lane_edit.cpp) and is drawn by the arrangement's own clip painters (src/arrangement/lane_paint.h: paintClip,
// paintAudioClip) on the window's time axis, without its TileAtlas tiles (keyed by the arrangement's zoom) — so the
// selection outline, the hovered part, audio fades / gain handles, drag previews and the time selection look the same in
// both views; in automation mode the envelopes are the arrangement's bands. The plain path below is for a view without an
// arrangement (tests of the model alone).
//
// What is reused: the MIDI notes are the arrangement's clip previews (arrangement::Model::preview: per clip snapshot
// and tuning, notes as content ticks with a normalised row, independent of the zoom), so a clip shown in both views is
// resolved once; the clip index per track (Model::clips) finds a window's clips by binary search; audio clips use the
// arrangement's audio layouts and the waveform overviews (pq/views/waveforms.h) through ui::drawWaveform, whose cost is
// the visible columns only. Painting is limited to the damaged rect (the canvas's clip) and the windows that meet it.

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>
#include <vector>

#include "../arrangement/internal.h"
#include "../arrangement/lane_paint.h"
#include "../common/paint.h"
#include "../common/waveform_data.h"
#include "internal.h"
#include "pq/document/audio_clip.h"
#include "pq/document/clip_time.h"
#include "pq/ui/theme.h"
#include "pq/ui/waveform.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/waveforms.h"

namespace pq::views::project_patch {

using ui::Rect;
using ui::Rgba;

namespace {

// The tuning a clip's notes resolve in: its override, else its track's (as the arrangement resolves it, so both views
// key the shared preview cache alike).
const doc::TuningDef* clipTuning(const doc::TuningDef* track, const doc::NoteClip& clip, const doc::Project& p) {
  if (clip.tuning.mode == doc::TuningMode::Private && clip.tuning.privateDef) return clip.tuning.privateDef.get();
  if (clip.tuning.mode != doc::TuningMode::Inherit)
    if (const doc::TuningSlot* s = doc::findTuning(p, clip.tuning.main)) return &s->def;
  return track;
}

// Per-row runs of notes waiting to be emitted (overlapping notes on one pixel row merge: a zoomed-out window draws one
// rect per run, not per note). UI thread only; kept across paints.
struct Run {
  float x0 = 0, x1 = 0;
  bool open = false;
};
std::vector<Run>& runScratch() {
  static std::vector<Run> runs;
  return runs;
}
std::vector<uint16_t>& usedScratch() {
  static std::vector<uint16_t> used;
  return used;
}

// A MIDI clip's notes over [vx0, vx1) of its columns (cx0 = the clip's left edge, device px per tick k).
uint32_t paintNotes(ui::DrawList& dl, const arrangement::Preview& pv, const doc::NoteClip& clip, Ticks from, Ticks to, double k, float cx0,
                    float vx0, float vx1, float by0, int32_t bodyH, Rgba noteCol) {
  const int32_t noteH = std::clamp(static_cast<int32_t>(std::lround(static_cast<float>(bodyH) * pv.rowFraction)), 1, bodyH);
  const auto span = static_cast<uint32_t>(bodyH - noteH);
  std::vector<Run>& runs = runScratch();
  std::vector<uint16_t>& used = usedScratch();
  if (runs.size() < static_cast<size_t>(bodyH) + 1) runs.resize(static_cast<size_t>(bodyH) + 1);
  used.clear();
  const Rgba offCol = ui::withAlpha(noteCol, 90);
  uint32_t emitted = 0;
  auto flush = [&](uint16_t row) {
    Run& r = runs[row];
    if (!r.open) return;
    r.open = false;
    const float y = by0 + static_cast<float>(row);
    dl.addRectFilled({r.x0, y, r.x1, y + static_cast<float>(noteH)}, noteCol);
    ++emitted;
  };
  doc::forEachClipSegment(clip, from, to, [&](const doc::ClipSegment& seg) {
    const double segX = cx0 + static_cast<double>(seg.at - clip.start) * k;
    const Ticks lookFrom = seg.contentFrom - pv.maxLength;
    auto it = std::lower_bound(pv.notes.begin(), pv.notes.end(), lookFrom, [](const arrangement::PreviewNote& n, Ticks t) { return n.start < t; });
    for (; it != pv.notes.end() && it->start < seg.contentTo; ++it) {
      const Ticks a = std::max(it->start, seg.contentFrom), b = std::min(it->end, seg.contentTo);
      if (b <= a) continue;
      float x0 = std::round(static_cast<float>(segX + static_cast<double>(a - seg.contentFrom) * k));
      float x1 = std::round(static_cast<float>(segX + static_cast<double>(b - seg.contentFrom) * k));
      if (x1 - x0 >= 3.0f) x1 -= 1.0f;  // a pixel gap between notes wide enough
      x0 = std::max(x0, vx0);
      x1 = std::min(std::max(x1, x0 + 1.0f), vx1);
      if (x1 <= x0) continue;
      const auto row = static_cast<uint16_t>((static_cast<uint32_t>(it->y) * span + 32767u) / 65535u);
      if (it->flags & arrangement::kPreviewDisabled) {
        const float y = by0 + static_cast<float>(row);
        dl.addRectFilled({x0, y, x1, y + static_cast<float>(noteH)}, offCol);
        continue;
      }
      Run& r = runs[row];
      if (r.open && x0 <= r.x1) {
        r.x1 = std::max(r.x1, x1);
      } else {
        if (r.open) flush(row);
        else used.push_back(row);
        r = {x0, x1, true};
      }
    }
  });
  for (const uint16_t row : used) flush(row);
  return emitted;
}

// An audio clip's waveform over [vx0, vx1) of its columns (as the arrangement draws it directly: pieces of the clip's
// time → source frame mapping, each drawn by ui::drawWaveform over its whole columns).
void paintWave(ui::DrawList& dl, const ui::WaveformData& data, const doc::AudioClip& c, const arrangement::AudioLayout& L, const TempoMap& tm,
               double k, float cx0, float vx0, float vx1, float by0, float by1, Rgba col) {
  const float gain = arrangement::clipGainLinear(c.gainDb);
  arrangement::forEachAudioPiece(c, L, tm, k, static_cast<double>(vx0 - cx0), static_cast<double>(vx1 - cx0), [&](const arrangement::AudioPiece& p) {
    const double ia = std::ceil(p.xa - 0.5), ib = std::ceil(p.xb - 0.5);
    if (ib <= ia || !(p.fb > p.fa)) return;
    const double s = (p.fb - p.fa) / (p.xb - p.xa);
    ui::WaveformDraw d;
    if (!p.reverse) {
      d.frame0 = p.fa + (ia - p.xa) * s;
      d.frame1 = p.fa + (ib - p.xa) * s;
    } else {
      d.frame1 = p.fb - (ia - p.xa) * s;
      d.frame0 = p.fb - (ib - p.xa) * s;
    }
    d.rect = {cx0 + static_cast<float>(ia), by0, cx0 + static_cast<float>(ib), by1};
    d.gain = gain;
    d.reverse = p.reverse;
    d.colour = col;
    d.clipX0 = vx0;
    d.clipX1 = vx1;
    (void)ui::drawWaveform(dl, data, d);
  });
}

}  // namespace

void paintLaneWindows(ui::PaintContext& ctx, const ProjectCanvas& canvas, ProjectPatchView& view) {
  if (ctx.res.theme == nullptr) return;
  const ui::Theme& th = *ctx.res.theme;
  const ProjectPatch& pp = view.patch();
  const Ticks start = view.laneStart(), span = view.laneSpan();
  if (span <= 0) return;
  const Ticks end = start + span;
  const ui::IRect clip = ui::intersect(ctx.clip, canvas.rect());
  ui::DrawList& dl = ctx.dl;
  const doc::Project& p = *view.context().doc();
  const TimeSigMap& sig = p.timeline->sigMap;
  ArrangementView* arr = view.arrangement();
  arrangement::Model* model = arr != nullptr ? &arr->model() : nullptr;
  // TX.lane-edit: with the arrangement next to the view the windows are its editing surface, drawn by its painters.
  auto* surf = static_cast<LaneEditSurface*>(view.laneSurface());
  const arrangement::Editor* ed = surf != nullptr ? &arr->editor() : nullptr;
  bool begun = false;
  const float us = ctx.scale.factor;
  const float line = std::max(1.0f, std::round(us));
  const bool fonts = ctx.res.uiFonts != nullptr && ctx.res.text != nullptr;
  const ui::FontId font = fonts ? ctx.res.uiFonts->small : ui::kNoFont;
  const float px = fonts ? static_cast<float>(ctx.res.text->metrics(font).pixelSize) : 11.0f;
  const bool header = fonts && canvas.lod() == circuit::CircuitCanvas::Lod::Near;
  const Rgba gridBar = ui::mulAlpha(th.arrangement.gridLine, 1.0f), gridBeat = ui::mulAlpha(th.arrangement.gridLine, 0.45f);
  if (model != nullptr) model->overlay.reset(dl.targetWidth(), dl.targetHeight());  // audio fades / handles of this paint
  bool anyWindow = false;

  for (uint32_t bi = 0; bi < pp.bands.size(); ++bi) {
    const Band& band = pp.bands[bi];
    if (!hasLaneWindow(band) || band.source == kNone) continue;
    const Node& n = pp.nodes[band.source];
    const Rect nr = canvas.deviceRect(n.x, n.y, n.w, n.h);
    if (!overlaps(nr, clip)) continue;
    const doc::Track* t = view.bandTrack(bi);
    if (t == nullptr) continue;
    const Rect body = laneBodyDevice(canvas, n);
    if (body.x1 - body.x0 < 2.0f || body.y1 - body.y0 < 1.0f) continue;
    ++view.mutableStats().lanesPainted;
    anyWindow = true;
    const bool trackOn = band.active && !t->mixer.mute;
    const double k = static_cast<double>(body.x1 - body.x0) / static_cast<double>(span);  // device px per tick
    auto X = [&](Ticks tick) { return body.x0 + static_cast<float>(static_cast<double>(tick - start) * k); };
    dl.addRectFilled(body, trackOn ? th.arrangement.lane : th.arrangement.laneShade);
    const int32_t row = surf != nullptr ? surf->rowOfBand(bi) : -1;
    const bool editing = row >= 0 && model != nullptr;
    // TX.lane-edit: the arrangement's selected tracks are lighter here too (one selection).
    if (editing && ed->trackSelected(t->id)) dl.addRectFilled(body, th.arrangement.laneSelected);

    const int32_t titleW = header ? ctx.res.text->shape(font, n.title).advance : 0;
    // Bar lines (and beat lines where they are at least 6 logical px apart).
    const int64_t bar0 = sig.ticksToBarBeat(start).bar;
    for (int64_t bar = bar0;; ++bar) {
      const Ticks bt = sig.barToTicks(bar);
      if (bt >= end) break;
      const TimeSignature ts = sig.signatureAt(bt);
      if (bt >= start) dl.addVLine(std::round(X(bt)), body.y0, body.y1, gridBar, line);
      const Ticks beat = ticksPerBeat(ts);
      if (static_cast<double>(beat) * k >= 6.0 * static_cast<double>(us))
        for (int32_t b = 1; b < ts.numerator; ++b) {
          const Ticks at = bt + beat * b;
          if (at >= end) break;
          if (at > start) dl.addVLine(std::round(X(at)), body.y0, body.y1, gridBeat, line);
        }
      if (header && bt >= start) {  // the bar number in the title row, right of the window's title
        const float x = std::round(X(bt)) + 2.0f * us;
        // TMM.13-fix2 (L14): right of the window's title as the canvas draws it (scene.cpp: 12 units in), 6 px clear.
        const float titleEnd = canvas.toDevice({n.x + 12.0f, n.y}).x + static_cast<float>(titleW) + 6.0f * us;
        if (x > titleEnd && x + 2.0f * px < nr.x1) {
          const std::string num = std::to_string(bar + 1);
          paint::text(ctx, font, toIRect({x, nr.y0 + 1.0f, std::min(nr.x1 - 2.0f, x + 4.0f * px), body.y0}), num, th.circuit.pinText,
                      ui::TextAlign::Left);
        }
      }
    }

    // Clips.
    if (model != nullptr && !begun) {
      model->beginPaint(0);  // unlimited builds: a window shows a few clips
      begun = true;
    }
    size_t i0 = 0, i1 = t->clips.size();
    if (model != nullptr) std::tie(i0, i1) = model->clips(*t).visible(start, end);
    const float titleH = laneClipTitleH(body.y1 - body.y0, us);

    if (editing) {
      // TX.lane-edit: the arrangement's clip painters over this window's time axis (no tiles: they are the arrangement's
      // zoom), the selection outline, the hovered part, audio handles; then the time selection and a drag's previews.
      const ui::IRect br = toIRect(body);
      const ui::IRect bc = ui::intersect(br, clip);
      if (bc.empty()) continue;
      const ArrangementRow& arow = surf->rows()[static_cast<size_t>(row)];
      arrangement::LanePaint lp{ctx, th, *model, *arr, p, br, bc, k, static_cast<double>(start),
                                start + static_cast<Ticks>(std::floor((static_cast<double>(bc.x - br.x) - 1.0) / k)),
                                start + static_cast<Ticks>(std::ceil((static_cast<double>(bc.right() - br.x) + 1.0) / k)),
                                static_cast<int32_t>(titleH)};
      lp.arrangementLanes = false;
      if (fonts && titleH >= px + 1.0f) {
        lp.font = font;
        const auto cap = static_cast<int32_t>(std::lround(0.727 * static_cast<double>(px)));
        lp.baselineOff = (lp.titleH + cap + 1) / 2;
      }
      const float top = body.y0, bottom = body.y1;
      const arrangement::ClipIndex& ix = model->clips(*t);
      const auto [a, b] = ix.visible(lp.from, lp.to);
      const arrangement::Editor::HoverPart& hp = ed->hoverPart();
      const bool hoverRow = !ed->dragging() && hp.ref.clip.valid() && hp.ref.track == t->id;
      for (size_t j = a; j < b; ++j) {
        const doc::ClipSlot& slot = t->clips[j];
        if (ed->hiddenWhileDragging(slot.id)) continue;  // drawn at its drag target below
        if (j > 0 && ix.start[j] < ix.maxEnd[j - 1]) arrangement::flushTiles(lp);  // overlapping: the earlier one complete
        const int hover = hoverRow && slot.id == hp.ref.clip ? hp.zone : arrangement::kNoHover;
        if (slot.audio) arrangement::paintAudioClip(lp, arow, slot.audio, {slot.audio->start, slot.audio->length, 0}, top, bottom, trackOn,
                                                    ed->selected(slot.id), hover);
        else arrangement::paintClip(lp, arow, slot.clip, top, bottom, trackOn, {slot.clip->start, slot.clip->length, slot.clip->contentStart},
                                    ed->selected(slot.id), hover);
      }
      for (size_t j = b; j < t->clips.size() && j < b + 2; ++j)  // a crossfade X reaching back into the painted part
        if (const doc::AudioClipPtr& next = t->clips[j].audio; next && !ed->hiddenWhileDragging(t->clips[j].id))
          arrangement::paintAudioCrossfade(lp, *next, top, bottom, ed->selected(t->clips[j].id) || (hoverRow && t->clips[j].id == hp.ref.clip));
      arrangement::flushTiles(lp);
      // The time selection (the insert marker a line) over the selected tracks' windows, as in the arrangement.
      if (const auto& ts = ed->selection().time; ts && ed->showTimeSelection() && ed->trackSelected(t->id)) {
        const bool marker = ts->end <= ts->start;
        const float x0 = std::max(static_cast<float>(bc.x), std::floor(lp.X(static_cast<double>(ts->start))));
        const float x1 = std::min(static_cast<float>(bc.right()), marker ? x0 + std::max(1.0f, std::round(us))
                                                                         : std::floor(lp.X(static_cast<double>(ts->end))));
        if (x1 > x0) dl.addRectFilled({x0, top, x1, bottom}, ui::withAlpha(th.clip.selectedBorder, marker ? 200 : 46));
      }
      if (ed->dragSurface() == surf)
        for (const arrangement::DragClip& dc : ed->dragClips()) {
          int32_t ri = 0;
          arrangement::ClipGeom g;
          if (!ed->preview(dc, ri, g) || ri != row) continue;
          if (dc.clip.audio) arrangement::paintAudioClip(lp, arow, dc.clip.audio, g, top, bottom, trackOn, true, arrangement::kNoHover);
          else arrangement::paintClip(lp, arow, dc.clip.clip, top, bottom, trackOn, g, true);
          arrangement::flushTiles(lp);
        }
      continue;
    }

    for (size_t ci = i0; ci < i1; ++ci) {
      const doc::ClipSlot& c = t->clips[ci];
      if (c.end() <= start || c.start() >= end) continue;
      const float cx0 = std::round(X(c.start())), cx1 = std::max(cx0 + 1.0f, std::round(X(c.end())));
      const float vx0 = std::max({cx0, body.x0, static_cast<float>(clip.x) - 1.0f});
      const float vx1 = std::min({cx1, body.x1, static_cast<float>(clip.right()) + 1.0f});
      if (vx1 <= vx0) continue;
      const Rgba colour = trackOn && c.active() ? arrangement::rgbOf(c.colour()) : arrangement::inactiveColour();
      const float ty1 = body.y0 + titleH;
      if (titleH > 0.0f) dl.addRectFilled({vx0, body.y0, vx1, ty1}, colour);
      dl.addRectFilled({vx0, ty1, vx1, body.y1}, arrangement::clipBodyColour(colour));
      if (cx0 >= body.x0 && cx0 >= vx0) dl.addVLine(cx0, body.y0, body.y1, ui::lerp(colour, ui::rgba(0, 0, 0), 0.55f), line);
      if (fonts && titleH >= px + 1.0f && vx1 - vx0 > 12.0f * us) {  // the name, from the window's edge when the clip starts left of it
        const float x = vx0 + 3.0f * us;
        paint::text(ctx, font, toIRect({x, body.y0, vx1 - 2.0f * us, ty1}), c.name(), model != nullptr ? model->clipTextColour(th, colour) : ui::rgba(18, 18, 18), ui::TextAlign::Left);
      }
      const float by0 = ty1, by1 = body.y1 - 1.0f;
      const int32_t innerH = static_cast<int32_t>(by1 - by0);
      if (innerH < 3 || cx1 - cx0 < 3.0f) continue;
      const Rgba noteCol = arrangement::clipNoteColour(colour);
      if (!c.isAudio()) {
        if (model == nullptr || c.clip->notes.empty()) continue;
        const arrangement::Preview* pv = model->preview(c.clip, clipTuning(view.bandTuning(bi), *c.clip, p));
        if (pv == nullptr || pv->notes.empty()) continue;
        (void)paintNotes(dl, *pv, *c.clip, std::max(start, c.start()), std::min(end, c.end()), k, cx0, vx0, vx1, by0, innerH, noteCol);
      } else {
        const doc::AudioClip& ac = *c.audio;
        if (model == nullptr || !view.context().sampleAvailable(ac.sample)) continue;
        const media::WaveformOverview* ov = view.context().waveforms().overview(ac.sample);
        if (ov == nullptr) continue;
        ArrangementRow arow;
        arow.id = t->id;
        arow.track = t;
        const arrangement::AudioLayout& L = model->audioLayout(arow, c.audio);
        ui::WaveformLevel lv[kMaxOverviewLevels];
        const ui::WaveformData data = waveformData(*ov, nullptr, lv);
        paintWave(dl, data, ac, L, p.timeline->tempoMap, k, cx0, vx0, vx1, by0, by1, noteCol);
      }
    }
  }
  if (!anyWindow) return;

  // TX.lane-edit: in automation mode the chosen parameter's envelope over each window's clips (the arrangement's bands,
  // on this surface), and its name in the window's title row.
  if (surf != nullptr && arr->automation().mode()) {
    arr->automation().paintSurface(ctx, *surf, clip);
    if (header)
      for (int32_t r = 0; r < static_cast<int32_t>(surf->rows().size()); ++r) {
        const uint32_t bi = surf->bandOf(r);
        const Node& n = pp.nodes[pp.bands[bi].source];
        const Rect nr = canvas.deviceRect(n.x, n.y, n.w, n.h);
        if (!overlaps(nr, clip)) continue;
        const Rect body = laneBodyDevice(canvas, n);
        const std::string label = paramShortName(p, arr->automation().chosen(surf->rows()[static_cast<size_t>(r)].id), view.context().deviceLibrary());
        const float w = std::min(body.x1 - body.x0, 8.0f * px);
        // TMM.13-fix2 (MM review contrast table: 2.70:1 on the title row): on a dark pad as wide as the name.
        const float tw = std::min(w, static_cast<float>(ctx.res.text->shape(font, label).advance) + 6.0f * us);
        const float padY0 = nr.y0 + 2.0f * us, padY1 = body.y0 - 1.0f * us;
        if (padY1 > padY0) ctx.dl.addRoundedRectFilled({body.x1 - tw - 1.0f * us, padY0, body.x1 + 2.0f * us, padY1}, 2.0f * us, th.surface.sunken);
        paint::text(ctx, font, toIRect({body.x1 - w, nr.y0 + 1.0f, body.x1, body.y0}), label, th.automation.line, ui::TextAlign::Right);
      }
  }

  // The playheads, on top.
  if (const std::optional<Ticks>& ph = view.lanePlayhead(); ph && *ph >= start && *ph < end)
    for (uint32_t bi = 0; bi < pp.bands.size(); ++bi) {
      const Band& band = pp.bands[bi];
      if (!hasLaneWindow(band) || band.source == kNone || view.bandTrack(bi) == nullptr) continue;
      const Node& n = pp.nodes[band.source];
      if (!overlaps(canvas.deviceRect(n.x, n.y, n.w, n.h), clip)) continue;
      const Rect body = laneBodyDevice(canvas, n);
      if (body.x1 - body.x0 < 2.0f || body.y1 - body.y0 < 1.0f) continue;
      const float x = lanePlayheadX(body, *ph, start, span);
      dl.addRectFilled({x, body.y0, x + std::max(1.0f, std::round(us)), body.y1}, th.arrangement.playhead);
    }
}

}  // namespace pq::views::project_patch
