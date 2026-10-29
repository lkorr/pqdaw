// MixerStrip (T3.10): one track's mixer controls in the header layout (arrangement) or the vertical layout (mixer
// view); and the MixerController the strips share (edits, gestures, labels, menus, meter polling).

#include <algorithm>
#include <cmath>

#include "../common/paint.h"
#include "internal.h"

namespace pq::views {

using namespace pq::ui;
namespace mm = mixer_metrics;
using mixer::Chooser;
using mixer::LiveBox;
using mixer::LiveToggle;
using mixer::TrackTuningChip;

// --- MixerController --------------------------------------------------------------------------------------------------

bool MixerController::apply(const doc::ops::EditResult& e, std::string_view what) {
  if (!e) {
    ctx.setStatus(std::string(what) + ": " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status s = ctx.store().dispatch(e->command); !s) {
    ctx.setStatus(std::string(what) + ": " + s.error().message);
    return false;
  }
  return true;
}

void MixerController::beginGesture(std::string_view label) {
  if (gesture_ || ctx.store().inGesture()) return;
  gesture_ = ctx.store().beginGesture(std::string(label)).ok();
}

void MixerController::change(const doc::Command& c) {
  if (Status s = ctx.store().dispatch(c); !s) ctx.setStatus(s.error().message);
}

void MixerController::endGesture() {
  if (!gesture_) return;
  gesture_ = false;
  (void)ctx.store().commitGesture();
}

void MixerController::cancelGesture() {
  if (!gesture_) return;
  gesture_ = false;
  (void)ctx.store().cancelGesture();  // the document goes back to the gesture's base: no history entry
}

void MixerController::activatorClicked(doc::TrackId id) {
  std::vector<doc::TrackId> tracks;
  if (selectedTracks) tracks = selectedTracks();
  if (std::find(tracks.begin(), tracks.end(), id) == tracks.end()) tracks = {id};
  (void)apply(doc::ops::toggleTracksMute(*ctx.doc(), tracks), "Track Activator");
}

std::string_view MixerController::activatorLabel(const doc::Snapshot& s, doc::TrackId id) {
  if (s != labelsFor_) {
    labelsFor_ = s;
    labels_.clear();
    uint32_t n = 0;
    struct Walk {
      std::vector<std::pair<uint32_t, std::string>>& out;
      uint32_t& n;
      void operator()(const std::vector<doc::TrackPtr>& list) {
        for (const doc::TrackPtr& t : list) {
          out.emplace_back(t->id.value, std::to_string(++n));
          (*this)(t->children);
        }
      }
    } walk{labels_, n};
    walk(s->tracks);
    for (size_t i = 0; i < s->returns.size(); ++i) {
      std::string l;
      for (size_t k = i + 1; k > 0; k = (k - 1) / 26) l.insert(l.begin(), static_cast<char>('A' + (k - 1) % 26));
      labels_.emplace_back(s->returns[i]->id.value, std::move(l));
    }
    std::sort(labels_.begin(), labels_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  const auto it = std::lower_bound(labels_.begin(), labels_.end(), id.value, [](const auto& e, uint32_t v) { return e.first < v; });
  return (it != labels_.end() && it->first == id.value) ? std::string_view(it->second) : std::string_view{};
}

void MixerController::openTuningMenu(Widget& chip, doc::TrackId id) {
  const doc::Project& p = *ctx.doc();
  const doc::Track* t = doc::findTrackPtr(p, id);
  if (!t) return;
  ViewContext* c = &ctx;
  auto assign = [c, id](doc::TuningAssignment a) {
    return [c, id, a = std::move(a)] {
      if (Status s = c->store().dispatch(doc::SetTrackTuning{id, a}); !s) c->setStatus("Tuning: " + s.error().message);
    };
  };
  std::vector<MenuItem> items;
  MenuItem inherit = MenuItem::item("Inherit", assign(doc::TuningAssignment::inherit()));
  inherit.checked = t->tuning.mode == doc::TuningMode::Inherit;
  items.push_back(std::move(inherit));
  if (p.tunings)
    for (const doc::TuningSlotPtr& slot : *p.tunings) {
      MenuItem m = MenuItem::item(slot->name, assign(doc::TuningAssignment::mainTuning(slot->id)));
      m.checked = t->tuning.mode == doc::TuningMode::Main && t->tuning.main == slot->id;
      items.push_back(std::move(m));
    }
  if (t->tuning.mode == doc::TuningMode::Private && t->tuning.privateDef) {
    MenuItem m = MenuItem::item("Private: " + mixer::tuningShortName(*t->tuning.privateDef));
    m.checked = true;
    m.enabled = false;
    items.push_back(std::move(m));
  }
  items.push_back(MenuItem::sep());
  items.push_back(MenuItem::item("MOS Wheel…", [c, w = &chip, id] {
    (void)openMosWheel(*w, *c, TuningTarget::forTrack(id), w->rect());
  }));
  (void)openContextMenu(chip, {static_cast<float>(chip.rect().x), static_cast<float>(chip.rect().bottom())}, std::move(items));
}

void MixerController::openOutputMenu(Widget& chooser, doc::TrackId id) {
  const doc::Project& p = *ctx.doc();
  const auto path = doc::findTrack(p, id);
  if (!path) return;
  const doc::Track& t = doc::trackAt(p, *path);
  const doc::Track* parent = path->list == doc::TrackList::Main ? doc::parentOf(p, *path) : nullptr;
  // Groups the track may feed: not itself, not one of its own descendants (a cycle).
  std::vector<const doc::Track*> groups;
  struct Walk {
    std::vector<const doc::Track*>& out;
    doc::TrackId self;
    void operator()(const std::vector<doc::TrackPtr>& list) {
      for (const doc::TrackPtr& g : list) {
        if (g->id == self) continue;  // skips its subtree too
        if (g->kind == doc::TrackKind::Group) out.push_back(g.get());
        (*this)(g->children);
      }
    }
  } walk{groups, id};
  if (t.kind != doc::TrackKind::Return) walk(p.tracks);
  ViewContext* c = &ctx;
  auto route = [c, id](doc::Output o) {
    return [c, id, o] {
      if (Status s = c->store().dispatch(doc::SetTrackOutput{id, o}); !s) c->setStatus("Output: " + s.error().message);
    };
  };
  const doc::Output cur = t.mixer.output;
  // What each item stores: the default (parent group / master) is kept as Default.
  auto outFor = [&](const doc::Track* g) {
    if (g == parent) return doc::Output{};
    if (!g) return parent ? doc::Output{doc::OutputKind::Master, {}} : doc::Output{};
    return doc::Output{doc::OutputKind::Track, g->id};
  };
  auto isCurrent = [&](const doc::Track* g) {
    if (cur.kind == doc::OutputKind::Default) return g == parent;
    if (cur.kind == doc::OutputKind::Master) return g == nullptr;
    return cur.kind == doc::OutputKind::Track && g && g->id == cur.track;
  };
  std::vector<MenuItem> items;
  MenuItem master = MenuItem::item("Master", route(outFor(nullptr)));
  master.checked = isCurrent(nullptr);
  items.push_back(std::move(master));
  for (const doc::Track* g : groups) {
    MenuItem m = MenuItem::item(g->name, route(outFor(g)));
    m.checked = isCurrent(g);
    items.push_back(std::move(m));
  }
  items.push_back(MenuItem::sep());
  MenuItem none = MenuItem::item("Sends Only", route(doc::Output{doc::OutputKind::None, {}}));
  none.checked = cur.kind == doc::OutputKind::None;
  items.push_back(std::move(none));
  if (Menu* m = openContextMenu(chooser, {static_cast<float>(chooser.rect().x), static_cast<float>(chooser.rect().bottom())}, std::move(items)))
    m->setMinWidthPx(chooser.rect().w);
}

void MixerController::addStrip(MixerStrip* s) { strips_.push_back(s); }

void MixerController::removeStrip(MixerStrip* s) noexcept { std::erase(strips_, s); }

bool MixerController::pollMeters() {
  if (!source) return false;
  const bool viewShown = view && view->shown();
  auto wanted = [&](const MixerStrip* s) {
    if (!s->meter() || !s->tree()) return false;
    return s->layoutKind() == StripLayout::Vertical ? viewShown : !viewShown;
  };
  ids_.clear();
  for (const MixerStrip* s : strips_)
    if (wanted(s)) ids_.push_back(s->trackId());
  levels_.resize(ids_.size());
  source->read(ids_, levels_);
  size_t k = 0;
  for (MixerStrip* s : strips_)
    if (wanted(s)) s->meter()->setLevels(levels_[k++]);
  ++stats.polls;
  stats.metersUpdated += k;
  return source->live();
}

// --- MixerStrip --------------------------------------------------------------------------------------------------------

MixerStrip::MixerStrip(std::string name, std::shared_ptr<MixerController> controller, StripLayout layout)
    : Widget(std::move(name)), ctl_(std::move(controller)), layout_(layout) {
  setLayout(LayoutKind::Custom);
  ctl_->addStrip(this);
}

MixerStrip::~MixerStrip() { ctl_->removeStrip(this); }

void MixerStrip::build(doc::TrackKind kind) {
  clearChildren();
  volume_ = nullptr;
  fader_ = nullptr;
  pan_ = nullptr;
  activator_ = solo_ = arm_ = post_ = nullptr;
  sends_.clear();
  output_ = nullptr;
  chip_ = nullptr;
  meter_ = nullptr;
  MixerController* ctl = ctl_.get();
  const bool master = kind == doc::TrackKind::Master;
  // T4.16a: in the arrangement's headers the wheel scrolls the tracks (it would otherwise change whatever box passes
  // under the pointer); the mixer view's controls take it, as Live's mixer does.
  const bool wheel = layout_ == StripLayout::Vertical;
  auto valueHooks = [ctl, wheel](ValueControl& v, const char* label) {
    v.setWheelAdjusts(wheel);
    v.onGestureBegin = [ctl, label] { ctl->beginGesture(label); };
    v.onGestureEnd = [ctl] { ctl->endGesture(); };
    v.onGestureCancel = [ctl] { ctl->cancelGesture(); };
  };

  volume_ = &add<LiveBox>("volume", LiveBox::Kind::Volume);
  valueHooks(*volume_, "Track Volume");
  volume_->onChange = [this, ctl](double pos) {
    ctl->change(doc::SetTrackVolume{id_, static_cast<float>(faderToVolume(pos))});
    if (fader_ && !fader_->inGesture()) fader_->setValue(pos);
  };
  if (layout_ == StripLayout::Vertical) {
    ValueSpec s;
    s.min = 0.0;
    s.max = 1.0;
    s.def = volumeToFader(0.0);
    s.keyStep = 0.01;
    fader_ = &add<Fader>("fader", s, Fader::Orientation::Vertical);
    fader_->formatter = [](double pos) { return formatVolume(faderToVolume(pos)); };
    fader_->setInfo("Track Volume", "Drag the fader (Shift: fine); Delete for 0 dB.");
    valueHooks(*fader_, "Track Volume");
    fader_->onChange = [this, ctl](double pos) {
      ctl->change(doc::SetTrackVolume{id_, static_cast<float>(faderToVolume(pos))});
      if (volume_ && !volume_->inGesture()) volume_->setValue(pos);
    };
  }
  pan_ = &add<LiveBox>("pan", LiveBox::Kind::Pan);
  valueHooks(*pan_, "Track Panning");
  pan_->onChange = [this, ctl](double v) { ctl->change(doc::SetTrackPan{id_, static_cast<float>(v)}); };

  if (!master) {
    activator_ = &add<LiveToggle>("activator", LiveToggle::Kind::Activator);
    activator_->onToggle = [this, ctl](bool) {
      ctl->activatorClicked(id_);
      resyncToggles();
    };
    solo_ = &add<LiveToggle>("solo", LiveToggle::Kind::Solo);
    solo_->onToggle = [this, ctl](bool) {
      const bool additive = (static_cast<LiveToggle*>(solo_)->pressMods() & mod::kCtrl) != 0;
      (void)ctl->apply(doc::ops::soloTrack(*ctl->ctx.doc(), id_, additive), "Solo");
      resyncToggles();
    };
    if (kind == doc::TrackKind::Midi) {
      arm_ = &add<LiveToggle>("arm", LiveToggle::Kind::Arm);
      arm_->onToggle = [this, ctl](bool) {
        const bool additive = (static_cast<LiveToggle*>(arm_)->pressMods() & mod::kCtrl) != 0;
        (void)ctl->apply(doc::ops::armTrack(*ctl->ctx.doc(), id_, additive), "Arm");
        resyncToggles();
      };
    }
    if (kind == doc::TrackKind::Return) {
      post_ = &add<LiveToggle>("post", LiveToggle::Kind::Post);
      post_->onToggle = [this, ctl](bool on) {
        (void)ctl->apply(doc::ops::setReturnSendsPreFader(*ctl->ctx.doc(), id_, !on), "Pre/Post");
        resyncToggles();
      };
    }
    auto& out = add<Chooser>("output");
    out.onOpen = [this, ctl](Widget& self) { ctl->openOutputMenu(self, id_); };
    output_ = &out;
    // Header strips leave the chip to the header's chip section (under the title: bindHeaderChip).
    if (layout_ == StripLayout::Vertical && (kind == doc::TrackKind::Midi || kind == doc::TrackKind::Group)) {
      chip_ = &add<TrackTuningChip>(ctl->ctx, ctl_);
    }
  }
  meter_ = &add<Meter>("meter");
  meter_->setInfo("Track Meter", "Peak and RMS level of the track's output (PQ Meter style). Click to reset the clip lamps.");
}

void MixerStrip::resyncToggles() {
  const doc::Track* t = doc::findTrackPtr(*ctl_->ctx.doc(), id_);
  if (!t) return;
  if (activator_) activator_->setOn(!t->mixer.mute);
  if (solo_) solo_->setOn(t->mixer.solo);
  if (arm_) arm_->setOn(t->mixer.arm);
  if (post_) post_->setOn(!doc::ops::returnSendsPreFader(*ctl_->ctx.doc(), id_));
}

void MixerStrip::syncSends(const doc::Project& p, const doc::Track& t) {
  // Targets: every return but itself (the master has no sends).
  bool same = true;
  size_t n = 0;
  if (t.kind != doc::TrackKind::Master)
    for (const doc::TrackPtr& r : p.returns) {
      if (r->id == t.id) continue;
      same = same && n < sends_.size() && sends_[n].target == r->id;
      ++n;
    }
  same = same && n == sends_.size();
  if (!same) {
    for (SendBox& s : sends_) (void)removeChild(*s.box);
    sends_.clear();
    MixerController* ctl = ctl_.get();
    size_t letter = 0;
    if (t.kind != doc::TrackKind::Master)
      for (const doc::TrackPtr& r : p.returns) {
        ++letter;
        if (r->id == t.id) continue;
        std::string l;
        for (size_t k = letter; k > 0; k = (k - 1) / 26) l.insert(l.begin(), static_cast<char>('A' + (k - 1) % 26));
        auto& box = add<LiveBox>("send[" + std::to_string(sends_.size()) + "]", LiveBox::Kind::Send, l);
        const doc::TrackId target = r->id;
        box.setWheelAdjusts(layout_ == StripLayout::Vertical);  // as build(): the headers leave the wheel to the scroll
        box.onGestureBegin = [ctl] { ctl->beginGesture("Send Level"); };
        box.onGestureEnd = [ctl] { ctl->endGesture(); };
        box.onGestureCancel = [ctl] { ctl->cancelGesture(); };
        box.onChange = [this, ctl, target](double pos) {
          ctl->change(doc::SetSendLevel{id_, target, static_cast<float>(faderToVolume(pos))});
        };
        sends_.push_back({target, &box});
      }
    invalidateArrange();
    markDirty();
  }
  for (SendBox& s : sends_) {
    if (s.box->inGesture()) continue;
    float db = doc::kMinVolumeDb;
    for (const doc::Send& sd : t.mixer.sends)
      if (sd.target == s.target) db = sd.levelDb;
    s.box->setValue(volumeToFader(db));
  }
}

void MixerStrip::bind(const doc::Project& p, const doc::Track& t) {
  id_ = t.id;
  if (!built_ || kind_ != t.kind) {
    kind_ = t.kind;
    built_ = true;
    build(kind_);
    invalidateArrange();
    markDirty();
  }
  if (layout_ == StripLayout::Vertical && (title_ != t.name || colour_ != t.colour.rgb)) {
    title_ = t.name;
    colour_ = t.colour.rgb;
    markDirty();
  }
  const double vol = volumeToFader(t.mixer.volumeDb);
  if (volume_ && !volume_->inGesture() && !(fader_ && fader_->inGesture())) volume_->setValue(vol);
  if (fader_ && !fader_->inGesture() && !volume_->inGesture()) fader_->setValue(vol);
  if (pan_ && !pan_->inGesture()) pan_->setValue(t.mixer.pan);
  if (activator_) {
    activator_->setOn(!t.mixer.mute);
    static_cast<LiveToggle*>(activator_)->setLabel(ctl_->activatorLabel(ctl_->ctx.doc(), t.id));
  }
  if (solo_) solo_->setOn(t.mixer.solo);
  if (arm_) arm_->setOn(t.mixer.arm);
  if (post_) post_->setOn(!doc::ops::returnSendsPreFader(p, t.id));
  syncSends(p, t);
  if (output_) static_cast<Chooser*>(output_)->setText(mixer::outputLabel(p, t));
  if (chip_) {
    static_cast<TrackTuningChip*>(chip_)->setTrack(t.id);
  }
  // T3.15: which controls are automated (a track parameter's lane lives on its own track) and overridden.
  uint32_t automated = 0, overridden = 0;
  for (const doc::LanePtr& l : t.lanes) {
    int32_t bit = -1;
    switch (l->param.kind) {
      case doc::ParamKind::TrackVolume: bit = 0; break;
      case doc::ParamKind::TrackPan: bit = 1; break;
      case doc::ParamKind::TrackMute: bit = 2; break;
      case doc::ParamKind::TrackSend:
        for (size_t i = 0; i < sends_.size() && i < 29; ++i)
          if (sends_[i].target.value == l->param.target) bit = 3 + static_cast<int32_t>(i);
        break;
      default: break;
    }
    if (bit < 0) continue;
    automated |= 1u << bit;
    if (ctl_->ctx.automationOverridden(l->param)) overridden |= 1u << bit;
  }
  if (automated != automated_ || overridden != overridden_) {
    automated_ = automated;
    overridden_ = overridden;
    markDirty();
  }
}

namespace mixer {

bool TrackTuningChip::onPointer(const PointerEvent& e) {
  if (e.button == PointerButton::Right && target()) {
    if (e.type == PointerType::Down) ctl_->openTuningMenu(*this, target()->track);
    return true;
  }
  return TuningChip::onPointer(e);
}

void bindHeaderChip(Widget& section, const std::shared_ptr<MixerController>& ctl, const doc::Project&, const doc::Track& t) {
  if (t.kind != doc::TrackKind::Midi && t.kind != doc::TrackKind::Group) {
    if (section.childCount()) section.clearChildren();
    return;
  }
  auto* chip = section.childCount() ? static_cast<TrackTuningChip*>(&section.child(0)) : &section.add<TrackTuningChip>(ctl->ctx, ctl);
  chip->setTrack(t.id);
}

}  // namespace mixer

// --- layout -------------------------------------------------------------------------------------------------------------

namespace {

// Places `w` at logical (x, y, w, h) inside `r`, or hides it when it does not fit vertically.
void put(LayoutPass& pass, Widget* w, const IRect& r, float x, float y, float width, float height) {
  if (!w) return;
  const UiScale s(pass.scale());
  const bool fits = s.px(y + height) <= r.h;
  if (w->visible() != fits) w->setVisible(fits);
  if (!fits) return;
  const int32_t x0 = r.x + s.px(x), y0 = r.y + s.px(y);
  pass.place(*w, {x0, y0, r.x + s.px(x + width) - x0, r.y + s.px(y + height) - y0});
}

}  // namespace

void MixerStrip::arrange(LayoutPass& pass) {
  if (layout_ == StripLayout::Header) arrangeHeader(pass);
  else arrangeVertical(pass);
}

// Header columns (logical px): A = 4..68 (volume; pan below), the separator 70..72, B = 74..162
// (activator 74..112, S 114..132, arm / Pre-Post 134..162; output below), the meter 166..174.
void MixerStrip::arrangeHeader(LayoutPass& pass) {
  const IRect r = rect();
  const float L = mm::kLine, T = mm::kBoxTop, B = mm::kBox;
  put(pass, volume_, r, 4, T, 64, B);
  put(pass, activator_, r, 74, T, 38, B);
  put(pass, solo_, r, 114, T, 18, B);
  put(pass, arm_, r, 134, T, 28, B);
  put(pass, post_, r, 134, T, 28, B);
  if (kind_ == doc::TrackKind::Master) {
    put(pass, pan_, r, 74, T, 38, B);  // the master's pan sits where the others have their activator
  } else {
    put(pass, pan_, r, 4, L + T, 64, B);
    put(pass, output_, r, 74, L + T, 88, B);
  }
  for (size_t i = 0; i < sends_.size(); ++i) {
    const auto line = static_cast<float>(2 + i / 3);
    put(pass, sends_[i].box, r, 4 + 54.0f * static_cast<float>(i % 3), line * L + T, 50, B);
  }
  if (meter_) {
    const UiScale s(pass.scale());
    const int32_t top = s.px(T), h = std::max(s.px(B), r.h - 2 * top);
    if (!meter_->visible()) meter_->setVisible(true);
    pass.place(*meter_, {r.x + s.px(166), r.y + top, s.px(174) - s.px(166), h});
  }
}

// Vertical strip (logical px, width kStripWidth): title 0..20, output, tuning, sends, pan, volume, then fader + meter
// filling down to the buttons at the bottom (activator, S, arm / Pre-Post).
void MixerStrip::arrangeVertical(LayoutPass& pass) {
  const IRect r = rect();
  const UiScale s(pass.scale());
  const float W = mm::kStripWidth, B = 16.0f, G = 3.0f;
  const float H = static_cast<float>(r.h) / pass.scale();
  float y = 24.0f;
  auto row = [&](Widget* w) {
    if (!w) return;
    put(pass, w, r, 4, y, W - 8, B);
    y += B + G;
  };
  row(output_);
  row(chip_);
  for (const SendBox& sb : sends_) row(sb.box);
  row(pan_);
  row(volume_);
  const float buttonsY = H - 4 - B;
  put(pass, activator_, r, 4, buttonsY, 24, B);
  put(pass, solo_, r, 31, buttonsY, 17, B);
  put(pass, arm_, r, 51, buttonsY, W - 55, B);
  put(pass, post_, r, 51, buttonsY, W - 55, B);
  const float fy0 = y + 2, fy1 = (kind_ == doc::TrackKind::Master ? H - 4 : buttonsY - 6);
  const float fh = std::max(0.0f, fy1 - fy0);
  if (fader_) {
    if (fader_->visible() != (fh >= 24)) fader_->setVisible(fh >= 24);
    if (fh >= 24) pass.place(*fader_, {r.x + s.px(6), r.y + s.px(fy0), s.px(34), s.px(fy0 + fh) - s.px(fy0)});
  }
  if (meter_) {
    if (meter_->visible() != (fh >= 24)) meter_->setVisible(fh >= 24);
    if (fh >= 24) pass.place(*meter_, {r.x + s.px(46), r.y + s.px(fy0), s.px(12), s.px(fy0 + fh) - s.px(fy0)});
  }
}

void MixerStrip::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const auto& m = th->mixer;
  const IRect r = rect();
  if (r.empty()) return;
  DrawList& dl = ctx.dl;
  if (layout_ == StripLayout::Header) {
    // The separator column between the volume and the activator (Live's dark rule), full height.
    const int32_t x0 = r.x + ctx.scale.px(70.0f), x1 = r.x + ctx.scale.px(72.0f);
    dl.addRectFilled(IRect{x0, r.y, std::max(1, x1 - x0), r.h}.toRect(), m.separator);
    if (meter_ && meter_->visible()) dl.addRectFilled(meter_->rect().toRect(), m.meterWell);
    return;
  }
  dl.addRectFilled(r.toRect(), m.background);
  const int32_t div = std::max(1, ctx.scale.px(1.0f));
  dl.addRectFilled(IRect{r.right() - div, r.y, div, r.h}.toRect(), m.separator);
  const IRect title{r.x, r.y, r.w - div, ctx.scale.px(20.0f)};
  const Rgba col = hex(colour_);
  dl.addRectFilled(title.toRect(), col);
  if (const ThemeFonts* f = ctx.res.uiFonts)
    paint::text(ctx, f->bodyStrong, title, title_, clipTextFor(*th, col), TextAlign::Left, ctx.scale.px(5.0f), ctx.scale.px(3.0f));
  if (meter_ && meter_->visible()) dl.addRectFilled(meter_->rect().toRect(), m.meterWell);
}

void MixerStrip::paintOver(PaintContext& ctx) {
  if (!automated_ || !ctx.res.theme) return;
  const Theme& th = *ctx.res.theme;
  const float size = static_cast<float>(std::max(2, ctx.scale.px(4.0f)));
  auto mark = [&](const Widget* w, uint32_t bit) {
    if (!w || !w->visible() || !(automated_ & (1u << bit))) return;
    const IRect r = intersect(w->rect(), rect());
    if (r.w < 2 * static_cast<int32_t>(size) || r.h < 2 * static_cast<int32_t>(size)) return;
    const float x = static_cast<float>(r.x + 1), y = static_cast<float>(r.y + 1);
    ctx.dl.addRectFilled({x, y, x + size, y + size}, (overridden_ & (1u << bit)) ? th.automation.ledOverridden : th.automation.led);
  };
  mark(volume_, 0);
  mark(fader_, 0);
  mark(pan_, 1);
  mark(activator_, 2);
  for (size_t i = 0; i < sends_.size() && i < 29; ++i) mark(sends_[i].box, static_cast<uint32_t>(3 + i));
}

bool MixerStrip::onPointer(const PointerEvent& e) {
  // Vertical strips: a click on the title selects the track (Shift / Ctrl as in the arrangement).
  if (layout_ == StripLayout::Vertical && e.type == PointerType::Down && e.button == PointerButton::Left) {
    if (ctl_->selectTrack) ctl_->selectTrack(id_, e.mods);
    return true;
  }
  return false;
}

}  // namespace pq::views
