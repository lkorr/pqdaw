# pq_views (T3.7): the DAW's views, composed from ui + document (+ tuning, core). Never depends on engine: the
# transport is reached through pq/views/transport_port.h, which app implements (docs/ARCHITECTURE.md §1).
#
# Layout (one subtree per view family, so parallel tasks do not touch each other's files):
#   include/pq/views/                                 context, transport port, view scenes (shared API)
#   include/pq/views/shell/        src/shell/         main window layout, control bar, info/detail/status (T3.7)
#   include/pq/views/arrangement/  src/arrangement/   arrangement, track headers, automation lanes (T3.8–T3.10, T3.15)
#   include/pq/views/mixer/        src/mixer/         mixer view (T3.10)
#   include/pq/views/piano_roll/   src/piano_roll/    clip view / piano roll (T3.11, T3.12)
#   include/pq/views/mos_wheel/    src/mos_wheel/     MOS wheel chart, circle, popover (T3.13, T3.14)
#   src/common/                                       private drawing helpers shared by every view
# A subtree lists its sources in its own src/<subtree>/sources.cmake as
#   set(PQ_VIEWS_<SUBTREE>_SOURCES src/<subtree>/a.cpp ...)   # paths relative to this directory
# which is included below once it exists.
set(PQ_VIEWS_SOURCES
  src/view_context.cpp
  src/view_scenes.cpp
  src/shell/shell.cpp
  src/shell/control_bar.cpp
  src/shell/panels.cpp
  src/shell/default_content.cpp)
foreach(sub arrangement mixer piano_roll mos_wheel)
  include(${CMAKE_CURRENT_SOURCE_DIR}/src/${sub}/sources.cmake OPTIONAL)
  string(TOUPPER ${sub} _sub)
  list(APPEND PQ_VIEWS_SOURCES ${PQ_VIEWS_${_sub}_SOURCES})
endforeach()

pq_add_module(pq_views
  SOURCES ${PQ_VIEWS_SOURCES}
  DEPS pq_ui pq_doc pq_tuning pq_core)
