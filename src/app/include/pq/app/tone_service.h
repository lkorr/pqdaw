#pragma once

// VZ.3: the displays' tone preview in the app: views::TonePort over AudioOutput's tone (pq/dsp/tone_preview.h) — the
// monitor pair at the device's buffer, never the engine's mix (no track, recording or render hears it). A hold wakes
// a suspended stream; the stream stays awake while the tone sounds (its 30 ms release included).
//
// UI thread only.

#include "pq/views/tone_port.h"

namespace pq::app {

class AudioOutput;

class ToneService final : public views::TonePort {
public:
  explicit ToneService(AudioOutput& output) : output_(output) {}
  void hold(const void* owner, float hz, float dbfs, dsp::ToneWave wave) override;
  void release(const void* owner) override;

private:
  AudioOutput& output_;
};

}  // namespace pq::app
