// MixerStrip (T3.10): one track's mixer controls in the header layout (arrangement) or the vertical layout (mixer
// view); and the MixerController the strips share (edits, gestures, labels, menus, meter polling).

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../common/paint.h"
#include "internal.h"
#include "pq/views/prefs/audio_prefs.h"

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

// ---- T6.4: the input section (MIDI tracks, mixer view) -----------------------------------------------------------------

namespace {
Menu* openUnder(Widget& chooser, std::vector<MenuItem> items) {
  Menu* m = openContextMenu(chooser, {static_cast<float>(chooser.rect().x), static_cast<float>(chooser.rect().bottom())},
                            std::move(items));
  if (m) m->setMinWidthPx(chooser.rect().w);
  return m;
}
}  // namespace

void MixerController::openMidiInMenu(Widget& chooser, doc::TrackId id) {
  const doc::Track* t = doc::findTrackPtr(*ctx.doc(), id);
  if (!t) return;
  const doc::MidiInput cur = t->mixer.midiIn;
  ViewContext* c = &ctx;
  auto pick = [c, id, cur](doc::MidiInputKind kind, std::string port) {
    return [c, id, cur, kind, port] {
      doc::MidiInput in{kind, kind == doc::MidiInputKind::Port ? port : std::string(), cur.channel};
      if (Status s = c->store().dispatch(doc::SetTrackMidiInput{id, in}); !s) c->setStatus("MIDI From: " + s.error().message);
    };
  };
  std::vector<MenuItem> items;
  MenuItem all = MenuItem::item("All Ins", pick(doc::MidiInputKind::All, {}));
  all.checked = cur.kind == doc::MidiInputKind::All;
  items.push_back(std::move(all));
  std::vector<std::string> ports{std::string(doc::kComputerKeyboardPort)};
  if (AudioPrefsPort* prefs = ctx.audioPrefs())
    for (const MidiPortEntry& m : prefs->midiInputs())
      if (m.connected || m.enabled) ports.push_back(m.name);
  if (cur.kind == doc::MidiInputKind::Port && std::find(ports.begin(), ports.end(), cur.port) == ports.end())
    ports.push_back(cur.port);  // a port that is not connected now stays visible (and chosen)
  for (const std::string& name : ports) {
    MenuItem m = MenuItem::item(name, pick(doc::MidiInputKind::Port, name));
    m.checked = cur.kind == doc::MidiInputKind::Port && cur.port == name;
    items.push_back(std::move(m));
  }
  items.push_back(MenuItem::sep());
  MenuItem none = MenuItem::item("No Input", pick(doc::MidiInputKind::None, {}));
  none.checked = cur.kind == doc::MidiInputKind::None;
  items.push_back(std::move(none));
  (void)openUnder(chooser, std::move(items));
}

void MixerController::openChannelMenu(Widget& chooser, doc::TrackId id) {
  const doc::Track* t = doc::findTrackPtr(*ctx.doc(), id);
  if (!t) return;
  const doc::MidiInput cur = t->mixer.midiIn;
  ViewContext* c = &ctx;
  std::vector<MenuItem> items;
  for (uint8_t ch = 0; ch <= 16; ++ch) {
    doc::MidiInput in = cur;
    in.channel = ch;
    MenuItem m = MenuItem::item(ch == 0 ? std::string("All Channels") : "Channel " + std::to_string(ch), [c, id, in] {
      if (Status s = c->store().dispatch(doc::SetTrackMidiInput{id, in}); !s) c->setStatus("MIDI channel: " + s.error().message);
    });
    m.checked = cur.channel == ch;
    items.push_back(std::move(m));
  }
  (void)openUnder(chooser, std::move(items));
}

void MixerController::openNoteInputMenu(Widget& chooser, doc::TrackId id) {
  const doc::Track* t = doc::findTrackPtr(*ctx.doc(), id);
  if (!t) return;
  const doc::NoteInput cur = t->noteInput;
  ViewContext* c = &ctx;
  auto set = [c, id](doc::NoteInput in) {
    return [c, id, in] {
      if (Status s = c->store().dispatch(doc::SetNoteInput{id, in}); !s) c->setStatus("Note input: " + s.error().message);
    };
  };
  std::vector<MenuItem> items;
  doc::NoteInput keys = cur, hz = cur;
  keys.mode = doc::NoteInputMode::Keys;
  hz.mode = doc::NoteInputMode::Hz;
  MenuItem k = MenuItem::item("Keys (degrees of the tuning)", set(keys));
  k.checked = cur.mode == doc::NoteInputMode::Keys;
  items.push_back(std::move(k));
  MenuItem h = MenuItem::item("Hz (raw pitch)", set(hz));
  h.checked = cur.mode == doc::NoteInputMode::Hz;
  items.push_back(std::move(h));
  items.push_back(MenuItem::sep());
  const std::pair<doc::InputKeyMap, const char*> maps[] = {
      {doc::InputKeyMap::Tuning, "Key map: the tuning's (.kbm, else consecutive)"},
      {doc::InputKeyMap::Consecutive, "Key map: consecutive degrees"},
      {doc::InputKeyMap::TwelveTetNearest, "Key map: nearest to 12-TET"},
      {doc::InputKeyMap::TwelveTetHz, "Key map: 12-TET Hz (A4 = 440)"}};
  for (const auto& [map, label] : maps) {
    doc::NoteInput in = cur;
    in.keyMap = map;
    MenuItem m = MenuItem::item(label, set(in));
    m.checked = cur.keyMap == map;
    items.push_back(std::move(m));
  }
  (void)openUnder(chooser, std::move(items));
}

void MixerController::openMonitorMenu(Widget& chooser, doc::TrackId id) {
  const doc::Track* t = doc::findTrackPtr(*ctx.doc(), id);
  if (!t) return;
  ViewContext* c = &ctx;
  std::vector<MenuItem> items;
  for (const doc::MonitorMode m : {doc::MonitorMode::In, doc::MonitorMode::Auto, doc::MonitorMode::Off}) {
    const char* label = m == doc::MonitorMode::In ? "Monitor In (always)" : m == doc::MonitorMode::Auto ? "Monitor Auto (while armed)" : "Monitor Off";
    MenuItem it = MenuItem::item(label, [c, id, m] {
      if (Status s = c->store().dispatch(doc::SetTrackMonitor{id, m}); !s) c->setStatus("Monitor: " + s.error().message);
    });
    it.checked = t->mixer.monitor == m;
    items.push_back(std::move(it));
  }
  (void)openUnder(chooser, std::move(items));
}

// T6.7: an audio track's input: none, one device channel (mono), or a pair (stereo). The device's channel count from
// the transport (at least 2 are offered, so a track can be set up before a device with inputs is open; channels the
// device does not have are marked).
void MixerController::openAudioInMenu(Widget& chooser, doc::TrackId id) {
  const doc::Track* t = doc::findTrackPtr(*ctx.doc(), id);
  if (!t) return;
  const doc::AudioInput cur = t->mixer.audioIn;
  ViewContext* c = &ctx;
  const uint32_t have = ctx.transport().audioInputChannels();
  const uint32_t n = std::max<uint32_t>({have, 2u, static_cast<uint32_t>(cur.firstChannel + cur.channels)});
  auto item = [&](doc::AudioInput in) {
    std::string label = mixer::audioInLabel(in);
    if (!in.none() && static_cast<uint32_t>(in.firstChannel + in.channels) > have) label += " (not available)";
    MenuItem m = MenuItem::item(label, [c, id, in] {
      if (Status s = c->store().dispatch(doc::SetTrackAudioInput{id, in}); !s) c->setStatus("Audio From: " + s.error().message);
    });
    m.checked = cur == in;
    return m;
  };
  std::vector<MenuItem> items;
  items.push_back(item(doc::AudioInput{}));
  items.push_back(MenuItem::sep());
  for (uint32_t ch = 0; ch < n; ++ch) items.push_back(item(doc::AudioInput{static_cast<uint16_t>(ch), 1}));
  items.push_back(MenuItem::sep());
  for (uint32_t ch = 0; ch + 1 < n; ch += 2) items.push_back(item(doc::AudioInput{static_cast<uint16_t>(ch), 2}));
  (void)openUnder(chooser, std::move(items));
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
    if (wanted(s)) {
      s->meter()->setLevels(levels_[k++]);
      s->refreshLatency();  // T6.6: a compile after an edit changes it while the engine runs
    }
  if (updateLatencyRow())
    for (MixerStrip* s : strips_)
      if (s->layoutKind() == StripLayout::Vertical) s->invalidateArrange();
  ++stats.polls;
  stats.metersUpdated += k;
  return source->live();
}

bool MixerController::updateLatencyRow() {
  bool any = false;
  for (const MixerStrip* s : strips_)
    if (s->layoutKind() == StripLayout::Vertical && s->hasLatency()) any = true;
  if (any == rows.latency) return false;
  rows.latency = any;
  return true;
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
  midiIn_ = channel_ = noteInput_ = monitor_ = audioIn_ = nullptr;
  delay_ = nullptr;
  latency_ = nullptr;
  latencyShown_ = -1.0;
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
    if (kind == doc::TrackKind::Midi || kind == doc::TrackKind::Audio) {  // T6.7: audio tracks record too
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
    if (kind == doc::TrackKind::Midi && layout_ == StripLayout::Vertical) {  // T6.4: the input section
      auto& in = add<Chooser>("midi-in");
      in.setInfo("MIDI From", "Where this track's notes come from while it is armed (or Monitor In): All Ins (every "
                              "MIDI port and the computer keyboard), one port, or none.");
      in.onOpen = [this, ctl](Widget& self) { ctl->openMidiInMenu(self, id_); };
      midiIn_ = &in;
      auto& ch = add<Chooser>("midi-channel");
      ch.setInfo("MIDI Channel", "All channels, or one MIDI channel of the input.");
      ch.onOpen = [this, ctl](Widget& self) { ctl->openChannelMenu(self, id_); };
      channel_ = &ch;
      auto& mode = add<Chooser>("note-input");
      mode.setInfo("Note Input", "Keys: incoming keys are degrees of the track's tuning (they follow it, tuning "
                                 "automation included). Hz: they are raw pitches (the key's Hz, kept exactly).");
      mode.onOpen = [this, ctl](Widget& self) { ctl->openNoteInputMenu(self, id_); };
      noteInput_ = &mode;
      auto& mon = add<Chooser>("monitor");
      mon.setInfo("Monitor", "In: always hear the input. Auto: hear it while the track is armed. Off: never.");
      mon.onOpen = [this, ctl](Widget& self) { ctl->openMonitorMenu(self, id_); };
      monitor_ = &mon;
    }
    if (kind == doc::TrackKind::Audio && layout_ == StripLayout::Vertical) {  // T6.7: an audio track's input
      auto& in = add<Chooser>("audio-in");
      in.setInfo("Audio From", "The audio input this track hears (Monitor) and records while armed: one input of the "
                               "audio device (mono), a pair (stereo), or none. Inputs come from an ASIO device's input "
                               "channels (Preferences: Audio).");
      in.onOpen = [this, ctl](Widget& self) { ctl->openAudioInMenu(self, id_); };
      audioIn_ = &in;
      auto& mon = add<Chooser>("monitor");
      mon.setInfo("Monitor", "In: always hear the input. Auto: hear it while the track is armed, unless the track "
                             "plays a clip while the transport plays without recording. Off: never.");
      mon.onOpen = [this, ctl](Widget& self) { ctl->openMonitorMenu(self, id_); };
      monitor_ = &mon;
    }
    auto& out = add<Chooser>("output");
    out.onOpen = [this, ctl](Widget& self) { ctl->openOutputMenu(self, id_); };
    output_ = &out;
    // Header strips leave the chip to the header's chip section (under the title: bindHeaderChip).
    if (layout_ == StripLayout::Vertical && (kind == doc::TrackKind::Midi || kind == doc::TrackKind::Group)) {
      chip_ = &add<TrackTuningChip>(ctl->ctx, ctl_);
    }
    if (layout_ == StripLayout::Vertical) {  // T6.6: Track Delay and the latency readout
      delay_ = &add<LiveBox>("track-delay", LiveBox::Kind::Delay);
      valueHooks(*delay_, "Track Delay");
      delay_->onChange = [this, ctl](double ms) { ctl->change(doc::SetTrackDelay{id_, static_cast<float>(ms)}); };
      latency_ = &add<Label>("latency", "", FontRoleId::Small, TextTone::Secondary, TextAlign::Center);
      latency_->setInfo("Track Latency", "How late this track's output is behind the transport: the latency of its "
                                         "devices and of what it receives (Track Delay not included). The other tracks "
                                         "are delayed to match the latest one, so everything stays in time.");
    }
  }
  meter_ = &add<Meter>("meter");
  meter_->setInfo("Track Meter", "Peak and RMS level of the track's output (PQ Meter style). Click to reset the clip lamps.");
  meter_->onResetClip = [this] {  // T4.16-fix1: the lamps latch until this click
    if (!ctl_->source) return;
    ctl_->source->resetClip(id_);
    ctl_->pollMeters();
  };
}

void MixerStrip::refreshLatency() {  // T6.6
  if (!latency_) return;
  const double ms = ctl_->ctx.transport().trackLatencyMs(id_.value);
  if (ms == latencyShown_) return;
  latencyShown_ = ms;
  if (std::abs(ms) < 0.05) {  // M6 review L21: nothing to say (the info text explains the row)
    latencyShown_ = 0.0;
    latency_->setText("");
    return;
  }
  char buf[48];
  std::snprintf(buf, sizeof buf, "Lat %.1f ms", ms);
  latency_->setText(buf);
}

int32_t MixerStrip::ioRowCount() const noexcept {
  int32_t n = 0;
  for (const Widget* w : {static_cast<const Widget*>(midiIn_), static_cast<const Widget*>(audioIn_),
                          static_cast<const Widget*>(channel_), static_cast<const Widget*>(noteInput_),
                          static_cast<const Widget*>(monitor_), static_cast<const Widget*>(output_),
                          static_cast<const Widget*>(chip_)})
    if (w) ++n;
  return n;
}

void MixerStrip::resyncToggles() {
  const doc::Track* t = doc::findTrackPtr(*ctl_->ctx.doc(), id_);
  if (!t) return;
  if (activator_) activator_->setOn(!t->mixer.mute);
  if (solo_) solo_->setOn(t->mixer.solo);
  if (arm_) {
    arm_->setOn(ctl_->ctx.armShown(*t));  // KB.1: lit while auto-armed too
    arm_->setEnabled(!t->frozen);         // FZ.3: a frozen track records nothing (its commit would be refused)
  }
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
  if (arm_) {
    arm_->setOn(ctl_->ctx.armShown(t));  // KB.1: lit while auto-armed too
    arm_->setEnabled(!t.frozen);         // FZ.3: a frozen track records nothing (its commit would be refused)
  }
  if (post_) post_->setOn(!doc::ops::returnSendsPreFader(p, t.id));
  syncSends(p, t);
  if (output_) static_cast<Chooser*>(output_)->setText(mixer::outputLabel(p, t));
  if (midiIn_) static_cast<Chooser*>(midiIn_)->setText(mixer::midiInLabel(t.mixer.midiIn));  // T6.4
  if (audioIn_) static_cast<Chooser*>(audioIn_)->setText(mixer::audioInLabel(t.mixer.audioIn));  // T6.7
  if (channel_) static_cast<Chooser*>(channel_)->setText(mixer::channelLabel(t.mixer.midiIn));
  if (noteInput_) static_cast<Chooser*>(noteInput_)->setText(mixer::noteInputLabel(t.noteInput));
  if (monitor_) static_cast<Chooser*>(monitor_)->setText(mixer::monitorLabel(t.mixer.monitor));
  if (delay_ && !delay_->inGesture()) delay_->setValue(t.mixer.trackDelayMs);  // T6.6
  refreshLatency();
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
  // M6 review L21: the rows are shared by every strip of the view (MixerController::rows), so sends, pan and volume
  // line up across track, group, return and master strips; a strip without a control leaves its cell empty.
  const MixerController::VerticalRows& plan = ctl_->rows;
  row(midiIn_);  // T6.4: the input section first (Live's In/Out), then the output
  row(audioIn_);  // T6.7
  row(channel_);
  row(noteInput_);
  row(monitor_);
  row(output_);
  row(chip_);
  y = 24.0f + static_cast<float>(std::max(plan.io, ioRowCount())) * (B + G);
  if (delay_) row(delay_);  // T6.6 (the master has none: an empty cell)
  else y += B + G;
  if (latency_ && latency_->visible() != plan.latency) latency_->setVisible(plan.latency);
  if (plan.latency) {
    if (latency_) row(latency_);
    else y += B + G;
  }
  for (const doc::TrackId ret : plan.returns) {  // one row per return: a strip's own return (or none) stays empty
    const auto it = std::find_if(sends_.begin(), sends_.end(), [ret](const SendBox& sb) { return sb.target == ret; });
    if (it != sends_.end()) row(it->box);
    else y += B + G;
  }
  for (const SendBox& sb : sends_)  // a send to a return the plan does not know yet (before the next sync)
    if (std::find(plan.returns.begin(), plan.returns.end(), sb.target) == plan.returns.end()) row(sb.box);
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
