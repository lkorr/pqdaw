// The displays' tone preview (pq/app/tone_service.h; VZ.3).

#include "pq/app/tone_service.h"

#include "pq/app/audio_output.h"

namespace pq::app {

void ToneService::hold(const void* owner, float hz, float dbfs, dsp::ToneWave wave) {
  const bool was = output_.tonePreview().sounding();
  output_.tonePreview().hold(owner, hz, dbfs, wave);
  if (!was) output_.tonePreviewChanged();  // a suspended stream starts for it (a retune needs no wake)
}

void ToneService::release(const void* owner) { output_.tonePreview().release(owner); }

}  // namespace pq::app
