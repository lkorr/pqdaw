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
  // Whether any reader got records since its last read.
  bool anyFresh() const;
  size_t readers() const noexcept { return readers_.size(); }

private:
  ProjectLink& link_;
  std::map<std::pair<uint32_t, std::string>, std::unique_ptr<views::TapVoices>, std::less<>> readers_;
};

}  // namespace pq::app
