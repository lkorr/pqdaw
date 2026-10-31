// M4 review probe (document model): exercises device / rack / wire commands through the public pq_doc API and checks
// the document invariants after each (serialize -> parse round trip = what a save + reopen does).
// Build: build_probe.ps1 next to this file (links the release pq_doc / pq_graph / pq_tuning / pq_core libs).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"

using namespace pq::doc;

static int g_fail = 0;

static ops::Edit must(pq::Result<ops::Edit> e, DocumentStore& s, const char* what) {
  if (!e) {
    std::printf("  %s: builder refused: %s\n", what, e.error().message.c_str());
    std::exit(2);
  }
  const pq::Status st = s.dispatch(e->command);
  if (!st) {
    std::printf("  %s: dispatch refused: %s\n", what, st.error().message.c_str());
    std::exit(2);
  }
  return *e;
}

static void roundTrip(const Project& p, const char* label) {
  auto text = serializeProject(p);
  if (!text) {
    std::printf("  [%s] serialize FAILED: %s\n", label, text.error().message.c_str());
    ++g_fail;
    return;
  }
  auto back = parseProject(*text);
  if (!back) {
    std::printf("  [%s] save -> reopen FAILED: %s\n", label, back.error().message.c_str());
    ++g_fail;
  } else {
    std::printf("  [%s] save -> reopen ok\n", label);
  }
}

static TrackId groupOf(const Project& p) {
  for (const TrackPtr& t : p.tracks)
    if (t->kind == TrackKind::Group) return t->id;
  return {};
}

// Probe 1: MoveDevice from a group track (inside its rack) to a CHILD of that group. apply.cpp MoveDevice takes
// `Track& ft = mutableTrack(from)` and then `mutableTrack(to)`; when `to` is below `from`, the second call copies the
// group again and frees the object `ft` refers to; pruneMappings(ft.devices) then runs on freed memory and the live
// tree keeps the rack's mapping onto the moved device.
static void probeMoveIntoChild(bool intoChild) {
  std::printf("probe move-out-of-rack %s:\n", intoChild ? "group -> its CHILD track" : "group -> an unrelated track");
  DocumentStore s;
  const TrackId m = must(ops::createTrack(*s.current(), s.ids(), TrackKind::Midi), s, "midi").tracks.at(0);
  const TrackId other = must(ops::createTrack(*s.current(), s.ids(), TrackKind::Midi), s, "midi2").tracks.at(0);
  must(ops::groupTracks(*s.current(), s.ids(), std::vector<TrackId>{m}), s, "group");
  const TrackId g = groupOf(*s.current());
  const DeviceId rack =
      must(ops::addDevice(*s.current(), s.ids(), g, std::string(kAudioEffectRack), DeviceKind::AudioEffect), s, "rack")
          .devices.at(0);
  const DeviceId u =
      must(ops::addChainWithDevice(*s.current(), s.ids(), rack, "utility", DeviceKind::AudioEffect), s, "chain")
          .devices.at(0);
  must(ops::mapMacro(*s.current(), s.ids(), rack, 0, u, "width"), s, "map");
  must(ops::moveDevice(*s.current(), u, intoChild ? m : other, kAppend), s, "move");
  const Project& p = *s.current();
  const DeviceLoc where = findDevice(p, u);
  std::printf("  device %u now on track %u (inside a rack: %s)\n", u.value, where.track ? where.track->id.value : 0,
              where.rack ? "yes" : "no");
  const MappingLoc ml = mappingOf(p, u, "width");
  std::printf("  rack's mapping onto it still present: %s\n", ml.mapping ? "YES (invariant broken)" : "no (pruned)");
  if (ml.mapping) ++g_fail;
  roundTrip(p, "after move");
}

int main() {
  probeMoveIntoChild(false);
  probeMoveIntoChild(true);
  std::printf("failures: %d\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
