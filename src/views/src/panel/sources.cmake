# GU.3: group panels — the element registry and the generic kinds, the curve editor, the panel's face (and its
# element tile cache, the group inspector's panel, the PanelHost widget), the document's panel target.
set(PQ_VIEWS_PANEL_SOURCES
  src/panel/elements.cpp
  src/panel/curve_element.cpp
  src/panel/rich_elements.cpp  # GU.5b: dial, arc, orbit
  src/panel/embed_element.cpp  # GU.6: an embedded child panel
  src/panel/engine_elements.cpp  # GU.7: the native engines' faces as elements, the meter
  src/panel/response_elements.cpp  # EQ.4: the response and phase displays, the band type chooser
  src/panel/transfer_element.cpp  # DY.3: the static transfer curve display
  src/panel/tempo_slider.cpp  # DT.1: the tempo-marked time slider (the Delay Tap's face)
  src/panel/cable_source.cpp  # DY.2: the cable source picker (a sidechain's source track)
  src/panel/group_responses.cpp  # EQ.4: groups' analytic responses, one lowering per structural revision
  src/panel/group_panel_face.cpp
  src/panel/doc_target.cpp
  src/panel/block_frame.cpp  # GU.13: a leaf block's panel (and a rack's macros) in its Device-view frame
  src/panel/style_schemas.cpp  # GU.8: named style sets per element kind
  src/panel/panel_window.cpp)  # GU.5: the panel window, its editor and inspector
