# Arrangement view (T3.8; T3.9 editing, T3.10 headers/mixer and T3.15 automation lanes extend it).
set(PQ_VIEWS_ARRANGEMENT_SOURCES
  src/arrangement/arrangement_view.cpp
  src/arrangement/audio_paint.cpp
  src/arrangement/automation.cpp
  src/arrangement/automation_edit.cpp
  src/arrangement/automation_follow.cpp  # TL.6: automated controls follow the playhead (Device and band views)
  src/arrangement/automation_paint.cpp
  src/arrangement/automation_tiles.cpp
  src/arrangement/editing.cpp
  src/arrangement/file_drop.cpp
  src/arrangement/lanes.cpp
  src/arrangement/model.cpp
  src/arrangement/overview.cpp
  src/arrangement/ruler.cpp
  src/arrangement/takes.cpp  # T6.10: take lanes and comping
  src/arrangement/track_header.cpp
  src/arrangement/tracks.cpp)
