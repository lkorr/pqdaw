# T4.2 circuit canvas (symbols, scene, canvas widget, viewer), T4.3 tidy.
set(PQ_VIEWS_CIRCUIT_SOURCES
  src/circuit/symbols.cpp
  src/circuit/circuit_scene.cpp
  src/circuit/circuit_canvas.cpp
  src/circuit/cables.cpp  # TX.cables: hanging cables (rest shape, rope simulation)
  src/circuit/orbit.cpp  # TX.orbit-ports: orbiting main ports (outline geometry, placement rule)
  src/circuit/circuit_viewer.cpp
  src/circuit/symbol_sheet.cpp
  src/circuit/patch_editor.cpp
  src/circuit/circuit_editor.cpp
  src/circuit/probes.cpp
  src/circuit/device_circuit.cpp  # T4.4-fix1: the editor on a device of the project
  src/circuit/node_info.cpp  # T4.15: node tooltips, recognised constant forms
  src/circuit/device_scene.cpp  # T4.15 / TMM.12: a device patch's scene as it plays
  src/circuit/node_catalog.cpp  # TX.node-menu: the catalog (atoms, meta modules, devices, user library), search, typed specs
  src/circuit/node_menu.cpp)  # TX.node-menu: the node menu widget
