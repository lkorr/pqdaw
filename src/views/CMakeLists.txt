# pq_views (T3.7): the DAW's views, composed from ui + document (+ tuning, core; T5.5: media, for waveform overviews). Never depends on engine: the
# transport is reached through pq/views/transport_port.h, which app implements (docs/ARCHITECTURE.md §1).
#
# Layout (one subtree per view family, so parallel tasks do not touch each other's files):
#   include/pq/views/                                 context, transport port, view scenes (shared API)
#   include/pq/views/shell/        src/shell/         main window layout, control bar, info/detail/status (T3.7)
#   include/pq/views/arrangement/  src/arrangement/   arrangement, track headers, automation lanes (T3.8–T3.10, T3.15)
#   include/pq/views/mixer/        src/mixer/         mixer view (T3.10)
#   include/pq/views/piano_roll/   src/piano_roll/    clip view / piano roll (T3.11, T3.12)
#   include/pq/views/audio_clip/   src/audio_clip/    the audio clip view (T5.6)
#   include/pq/views/mos_wheel/    src/mos_wheel/     MOS wheel chart, circle, popover (T3.13, T3.14)
#   include/pq/views/prefs/        src/prefs/         preferences window (T3.18)
#   include/pq/views/tunings/      src/tunings/       Tunings panel (T3.19)
#   include/pq/views/circuit/      src/circuit/       circuit canvas, symbols, patch viewer (T4.2, T4.3)
#   include/pq/views/devices/      src/devices/       device panels, displays, the Device view (T4.6)
#   include/pq/views/device_chain/ src/device_chain/  the Device view: device chain, generic panels, panel hook (T4.1)
#   include/pq/views/project_patch/ src/project_patch/  the project patch view (T4.13)
#   src/common/                                       private drawing helpers shared by every view
# A subtree lists its sources in its own src/<subtree>/sources.cmake as
#   set(PQ_VIEWS_<SUBTREE>_SOURCES src/<subtree>/a.cpp ...)   # paths relative to this directory
# which is included below once it exists.
set(PQ_VIEWS_SOURCES
  src/view_context.cpp
  src/view_scenes.cpp
  src/waveforms.cpp
  src/shell/shell.cpp
  src/shell/control_bar.cpp
  src/shell/menu_bar.cpp
  src/shell/panels.cpp
  src/shell/default_content.cpp)
foreach(sub arrangement audio_clip mixer piano_roll mos_wheel prefs tunings circuit devices device_chain project_patch)
  include(${CMAKE_CURRENT_SOURCE_DIR}/src/${sub}/sources.cmake OPTIONAL)
  string(TOUPPER ${sub} _sub)
  list(APPEND PQ_VIEWS_SOURCES ${PQ_VIEWS_${_sub}_SOURCES})
endforeach()

pq_add_module(pq_views
  SOURCES ${PQ_VIEWS_SOURCES}
  DEPS pq_ui pq_doc pq_graph pq_dsp pq_tuning pq_media pq_core
  PRIVATE_DEPS nlohmann_json::nlohmann_json)  # T4.6: panel descriptions (JSON)
