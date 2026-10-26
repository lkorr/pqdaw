// Projects for the project patch view's scenes, tests and benchmarks (T4.13): the user's sketch (a group of a synth
// track and a pad track with the group's own processing, a return fed by sends, a mastering chain, modulation across
// tracks) and a wide project of many tracks. Built through the edit operations, like a user would.

#include <optional>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::project_patch {

namespace {

struct Runner {
  doc::DocumentStore& store;
  Status status = Ok{};
  std::optional<doc::ops::Edit> run(const doc::ops::EditResult& e) {
    if (!status) return std::nullopt;
    if (!e) {
      status = e.error();
      return std::nullopt;
    }
    if (Status s = store.dispatch(e->command); !s) {
      status = std::move(s);
      return std::nullopt;
    }
    return *e;
  }
  doc::TrackId track(doc::TrackKind kind, doc::TrackId after, const char* name, uint32_t colour) {
    auto e = run(doc::ops::createTrack(*store.current(), store.ids(), kind, after, name, doc::Colour{colour}));
    return e && !e->tracks.empty() ? e->tracks[0] : doc::TrackId{};
  }
  doc::DeviceId device(doc::TrackId t, const char* patch, doc::DeviceKind kind) {
    auto e = run(doc::ops::addDevice(*store.current(), store.ids(), t, patch, kind));
    return e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
  }
  void clip(doc::TrackId t, Ticks start, Ticks end, const char* name, std::vector<int> degrees) {
    auto c = run(doc::ops::insertEmptyClip(*store.current(), store.ids(), t, start, end, name));
    if (!c || c->clips.empty()) return;
    std::vector<doc::Note> notes;
    Ticks at = 0;
    for (const int d : degrees) {
      doc::Note n;
      n.start = at;
      n.length = kPpq;
      n.pitch = tuning::PitchId::fromDegree(d, 0);
      notes.push_back(n);
      at += kPpq;
    }
    (void)run(doc::ops::addNotes(*store.current(), store.ids(), c->clips[0], notes));
  }
};

}  // namespace

Status buildSketchProject(doc::DocumentStore& store) {
  Runner r{store};
  using K = doc::DeviceKind;
  const doc::TrackId keys = r.track(doc::TrackKind::Midi, {}, "Keys", 0xF6A23B);
  const doc::TrackId pad = r.track(doc::TrackKind::Midi, keys, "Pad", 0x8ED36C);
  const doc::TrackId mod = r.track(doc::TrackKind::Midi, pad, "Mod", 0xB38BEA);
  if (!r.status) return r.status;
  // The keys play Operator through a Saturator; the pad the default synth through an EQ Eight.
  (void)r.device(keys, "operator", K::Instrument);
  const doc::DeviceId sat = r.device(keys, "saturator", K::AudioEffect);
  (void)r.device(pad, "eq_eight", K::AudioEffect);
  // The modulation track holds only an LFO (no instrument: an audio track that makes no sound).
  if (const doc::Track* m = doc::findTrackPtr(*store.current(), mod); m != nullptr && !m->devices.empty()) {
    const doc::DeviceId inst[1] = {m->devices[0]->id};
    (void)r.run(doc::ops::deleteDevices(*store.current(), inst));
  }
  const doc::DeviceId lfo = r.device(mod, "lfo_device", K::AudioEffect);
  // Keys and Pad in a group with its own processing.
  const doc::TrackId two[2] = {keys, pad};
  auto g = r.run(doc::ops::groupTracks(*store.current(), store.ids(), two, "Group"));
  if (!g || g->tracks.empty()) {
    if (r.status) return Error{ErrorCode::Unknown, "sketch: grouping failed", {}};
    return r.status;
  }
  const doc::TrackId grp = g->tracks[0];
  (void)r.run(doc::ops::setTracksColour(*store.current(), std::vector<doc::TrackId>{grp}, doc::Colour{0x3FA9F5}));
  (void)r.device(grp, "eq_eight", K::AudioEffect);
  const doc::DeviceId gutil = r.device(grp, "utility", K::AudioEffect);
  // A return fed by sends, and the mastering chain.
  const doc::TrackId ret = r.track(doc::TrackKind::Return, {}, "Space", 0x6FC7C7);
  (void)r.device(ret, "eq_eight", K::AudioEffect);
  const doc::Snapshot s = store.current();
  if (s && s->master) (void)r.device(s->master->id, "saturator", K::AudioEffect);
  if (!r.status) return r.status;
  if (Status st = store.dispatch(doc::SetSendLevel{keys, ret, -6.0f}); !st) return st;
  if (Status st = store.dispatch(doc::SetSendLevel{pad, ret, -12.0f}); !st) return st;
  if (Status st = store.dispatch(doc::SetTrackVolume{pad, -4.5f}); !st) return st;
  if (Status st = store.dispatch(doc::SetTrackPan{keys, -0.35f}); !st) return st;
  // Modulation anywhere: the LFO into the group's width and into the keys' saturator drive.
  (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), lfo, "lfo", gutil, "width", 0.4));
  (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), lfo, "lfo", sat, "drive", 0.25));
  // Clips the source nodes show.
  r.clip(keys, 0, 4 * kTicksPerWhole, "Riff", {0, 4, 7, 11});
  r.clip(keys, 6 * kTicksPerWhole, 10 * kTicksPerWhole, "Riff 2", {2, 5, 9});
  r.clip(pad, 2 * kTicksPerWhole, 12 * kTicksPerWhole, "Pad", {0, 7});
  return r.status;
}

Status buildWideProject(doc::DocumentStore& store, uint32_t tracks) {
  Runner r{store};
  using K = doc::DeviceKind;
  doc::TrackId after;
  std::vector<doc::TrackId> made;
  for (uint32_t i = 0; i < tracks; ++i) {
    const std::string name = "Track " + std::to_string(i + 1);
    after = r.track(doc::TrackKind::Midi, after, name.c_str(), 0x3FA9F5 + 0x101010u * (i % 8));
    if (!r.status) return r.status;
    made.push_back(after);
    (void)r.device(after, i % 2 == 0 ? "saturator" : "eq_eight", K::AudioEffect);
    if (i % 3 == 0) (void)r.device(after, "utility", K::AudioEffect);
    r.clip(after, static_cast<Ticks>(i % 4) * kTicksPerWhole, static_cast<Ticks>(i % 4 + 4) * kTicksPerWhole, "Clip", {0, 3, 7});
  }
  // Groups of eight.
  for (uint32_t g = 0; g + 8 <= made.size(); g += 8) {
    const std::vector<doc::TrackId> members(made.begin() + g, made.begin() + g + 8);
    auto e = r.run(doc::ops::groupTracks(*store.current(), store.ids(), members, "Group " + std::to_string(g / 8 + 1)));
    if (e && !e->tracks.empty()) (void)r.device(e->tracks[0], "utility", K::AudioEffect);
  }
  const doc::TrackId a = r.track(doc::TrackKind::Return, {}, "A", 0x6FC7C7);
  const doc::TrackId b = r.track(doc::TrackKind::Return, a, "B", 0xC76F9F);
  if (!r.status) return r.status;
  for (size_t i = 0; i < made.size(); i += 2) (void)store.dispatch(doc::SetSendLevel{made[i], i % 4 == 0 ? a : b, -10.0f});
  return r.status;
}

}  // namespace pq::views::project_patch
