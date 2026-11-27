// SX.4: the rendered waveforms' source (pq/app/clip_waves.h).
#include "pq/app/clip_waves.h"

#include <memory>

#include "pq/app/project_link.h"

namespace pq::app {

namespace {

views::ClipWaveFn sourceOver(std::shared_ptr<media::SamplePool> owned, media::SamplePool* pool, ClipRenderSettings base) {
  return [owned = std::move(owned), pool, base](const doc::Project& p, const views::ClipWaveRequest& q)
             -> Result<std::vector<std::vector<float>>> {
    ClipRenderSettings s = base;
    s.sampleRate = q.config.sampleRate;
    s.blockFrames = q.config.blockFrames;
    s.envelope = false;
    s.cancel = q.cancel;
    return renderClipAlone(p, q.clip, q.samplePath, *pool, s);
  };
}

}  // namespace

views::ClipWaveFn clipWaveSource(ProjectLink& link) {
  return sourceOver(nullptr, &link.samplePool(), link.clipRenderSettings());
}

views::ClipWaveConfig clipWaveConfig(const ProjectLink& link) {
  const ClipRenderSettings s = link.clipRenderSettings();
  return views::ClipWaveConfig{s.sampleRate, s.blockFrames};
}

views::ClipWaveFn sceneClipWaveSource() {
  auto pool = std::make_shared<media::SamplePool>();
  media::SamplePool* raw = pool.get();
  return sourceOver(std::move(pool), raw, ClipRenderSettings{});
}

}  // namespace pq::app
