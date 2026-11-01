#pragma once

// The faces' voice reader over the engine's voice taps (TMM.5, the TMM.2 / TMM.4 join; docs/plans/MM.md "Per-voice
// dots without probes"): adapts pq::VoiceTapReader (pq/core/voice_tap.h: per voice slot the newest sampled value and
// a history) to views::VoiceTapReader (pq/views/meta/meta_face.h: the voices sounding now, each with its newest phase
// and the phases sampled since the last read).
//
// The tap is looked up through `resolve` at every read (a recompile may give the instance a new tap object; the host's
// lookup is a map find); the pq::VoiceTapReader is registered with it while this object exists (so the audio thread
// samples into it: about one record copy per block per tap), and re-created when the tap changes.
//   serial()  the tap's records written so far (summed over its voice groups: relaxed loads, no lock): changes with
//             every block a voice of the instance runs, and once more when the voices shown have gone stale (no
//             record for longer than the reader's staleness window), so the face reads them gone.
//   live()    a voice was sounding at the last read, or records arrived since: the host keeps polling while true.
//   fresh()   records arrived since the last read (the host's cheap check whether to wake its faces).
// UI thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "pq/core/voice_tap.h"
#include "pq/views/meta/meta_face.h"

namespace pq::views {

class TapVoices final : public VoiceTapReader {
public:
  using Resolve = std::function<std::shared_ptr<pq::VoiceTap>()>;  // the instance's tap now (null: none)
  explicit TapVoices(Resolve resolve);
  ~TapVoices() override;

  uint32_t read(std::span<FaceVoice> out) override;
  uint64_t serial() const noexcept override;
  bool live() const noexcept override;
  bool fresh() const noexcept;

  // The tap read now (tests).
  const pq::VoiceTap* tap() const noexcept { return tap_.get(); }

private:
  uint64_t written() const noexcept;  // records of the current tap (0 without one)
  // Re-resolves the tap (an instance compiled after the face was bound gets its tap; a recompile may give a new one);
  // a new tap gets a new reader. serial() and fresh() follow too, so a tap that appears later is seen.
  void follow() const noexcept;
  Resolve resolve_;
  mutable std::shared_ptr<pq::VoiceTap> tap_;
  mutable std::unique_ptr<pq::VoiceTapReader> reader_;
  mutable uint64_t tapSerial_ = 0;     // bumped when the tap object changes (serial() changes with it)
  mutable uint64_t readWritten_ = 0;   // written() at the last read
  mutable bool active_ = false;        // a voice sounded at the last read
  double lastRecord_ = -1e300;         // when records last arrived (seen by a read)
  mutable std::vector<uint64_t> seen_; // per voice slot: the record serial of its last read
  std::vector<float> hist_;
};

}  // namespace pq::views
