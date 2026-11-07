# GU.3: group panels — the element registry and the generic kinds, the curve editor, the panel's face (and its
# element tile cache, the group inspector's panel, the PanelHost widget), the document's panel target.
set(PQ_VIEWS_PANEL_SOURCES
  src/panel/elements.cpp
  src/panel/curve_element.cpp
  src/panel/rich_elements.cpp  # GU.5b: dial, arc, orbit
  src/panel/embed_element.cpp  # GU.6: an embedded child panel
  src/panel/engine_elements.cpp  # GU.7: the native engines' faces as elements, the meter
  src/panel/group_panel_face.cpp
  src/panel/doc_target.cpp
  src/panel/panel_window.cpp)  # GU.5: the panel window, its editor and inspector
