#pragma once

// SX.4: the arrangement's rendered waveforms (pq/views/waveforms.h "RENDERED waveforms"): the render the views'
// cache calls on its background thread, renderClipAlone (pq/app/project_link.h) without the envelope.
//
//   clipWaveSource(link)   over the playback link's sample pool and settings (the app: ShellApp installs it)
//   sceneClipWaveSource()  over a private sample pool at 48 kHz / 64-frame blocks (pqdaw --screenshot, the goldens)
//
// Not RT code. The source must not outlive what it renders over (ShellApp removes it before the link goes).

#include "pq/views/waveforms.h"

namespace pq::app {

class ProjectLink;

views::ClipWaveFn clipWaveSource(ProjectLink& link);
views::ClipWaveConfig clipWaveConfig(const ProjectLink& link);
views::ClipWaveFn sceneClipWaveSource();

}  // namespace pq::app
