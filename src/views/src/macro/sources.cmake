# T8.1 macro mode (SPEC §6.2): the keyboard interval-entry mode of the piano roll.
set(PQ_VIEWS_MACRO_SOURCES
  src/macro/macro_mode.cpp     # the key listener: cursor, anchor, composition, layers, lock, undo ring, keypress labels
  src/macro/macro_doc.cpp      # its document access for JI notes / groups (the functions to adapt when the model changes)
  src/macro/macro_preset.cpp   # key presets (assets/macro/*.json)
  src/macro/macro_overlay.cpp) # what the clip view draws: cursor, anchor, label, the keys' labels; the control bar's lamp
