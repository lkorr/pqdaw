#pragma once

// The faces' voice dots in the running app (TMM.5, the TMM.2 / TMM.4 join): one views::TapVoices per meta instance
// (device, name path), reading the tap the playback link owns for it — request 0's (a VoiceTap: the Phase
// accumulator's `voice_tap` of its phase; a FrameTable slot's: what the Wavetable's `wavetable_pos` read). The project
// patch view binds them (ProjectPatchView::faceVoices); the shell wakes its faces when any of them received records
// (wakeIfFresh), so a note played while nothing animates starts the dots.
// UI thread only. Readers live as long as the hub (their pointers are bound into faces).

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "pq/document/model.h"
#include "pq/views/meta/tap_voices.h"

namespace pq::app {

class ProjectLink;

class FaceVoiceHub {
public:
  explicit FaceVoiceHub(ProjectLink& link) : link_(link) {}
  // A stable reader for the instance (created on first use).
  views::VoiceTapReader* voices(doc::DeviceId device, std::string_view path);
  // Whether any reader got records since its last read. TMM.13-fix1 M12: readers of instances the link has no slot for
  // (deleted, or not compiled yet) are parked when the link's slots change (TapVoices::park: no tap or table kept,
  // not polled here) and woken when their instance has a slot again. Readers are never freed (faces hold them).
  bool anyFresh();
  size_t readers() const noexcept { return readers_.size(); }
  size_t polled() const noexcept;  // readers anyFresh polls (not parked)

private:
  struct Entry {
    std::unique_ptr<views::TapVoices> reader;
    bool parked = false;
  };
  ProjectLink& link_;
  std::map<std::pair<uint32_t, std::string>, Entry, std::less<>> readers_;
  uint64_t prunedAt_ = ~uint64_t{0};  // the link's meta generation at the last prune
};

}  // namespace pq::app
