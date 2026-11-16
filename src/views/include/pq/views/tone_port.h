#pragma once

// VZ.3: the tone preview as the views see it (like PreviewPort): the Spectrum's plot and the Spectrogram's keys /
// Ctrl+click hold a tone at the pitch under the pointer. The app implements it over AudioOutput's tone preview
// (pq/dsp/tone_preview.h: the monitor pair, never the project); tests and scenes use RecordingTonePort. One tone app-wide:
// a hold by another owner takes it over, a release by a stale owner is ignored.
//
// UI thread only.

#include <cstdint>

#include "pq/dsp/tone_preview.h"

namespace pq::views {

class TonePort {
public:
  virtual ~TonePort() = default;
  // Starts or retunes the tone (a drag calls it at every move).
  virtual void hold(const void* owner, float hz, float dbfs, dsp::ToneWave wave) = 0;
  virtual void release(const void* owner) = 0;
};

// A scripted port: records what was asked (tests).
class RecordingTonePort final : public TonePort {
public:
  const void* owner = nullptr;
  float hz = 0.0f, dbfs = 0.0f;
  dsp::ToneWave wave = dsp::ToneWave::Sine;
  int holds = 0, releases = 0;
  bool sounding() const noexcept { return owner != nullptr; }
  void hold(const void* o, float h, float db, dsp::ToneWave w) override {
    owner = o;
    hz = h;
    dbfs = db;
    wave = w;
    ++holds;
  }
  void release(const void* o) override {
    if (o != owner) return;
    owner = nullptr;
    ++releases;
  }
};

}  // namespace pq::views
