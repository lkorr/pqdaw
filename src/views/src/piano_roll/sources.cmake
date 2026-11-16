# T3.11 clip view / piano roll; T3.12 scale / MOS layouts and live morph.
set(PQ_VIEWS_PIANO_ROLL_SOURCES
  src/piano_roll/row_model.cpp
  src/piano_roll/roll_state.cpp
  src/piano_roll/roll_paint.cpp
  src/piano_roll/roll_input.cpp
  src/piano_roll/roll_ji.cpp
  src/piano_roll/roll_pen.cpp
  src/piano_roll/roll_groups.cpp        # T7.14: JI groups as frames (Tune to JI, warps, walk, proportions, ratios)
  src/piano_roll/roll_vibrato.cpp       # T7.14: vibrato by dragging (the ~ grip, the envelope's corners)
  src/piano_roll/roll_bend.cpp          # bug-bend-shape: a bend's shape node (its length and where it turns)
  src/piano_roll/roll_live.cpp          # bug-live-shape: shape drags edit the document while they run
  src/piano_roll/roll_help.cpp          # KB.help: the ? panel (layout, painting)
  src/piano_roll/roll_help_content.cpp  # KB.help: its text (the table of sections and entries)
  src/piano_roll/roll_playhead.cpp      # bug-roll-playhead: the transport's playhead in clip time, the scrub area
  src/piano_roll/clip_view.cpp)
