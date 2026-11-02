# T4.13 the project patch view: the model (lowered graph -> bands, nodes, edges, layout), its scene, the view.
set(PQ_VIEWS_PROJECT_PATCH_SOURCES
  src/project_patch/model.cpp
  src/project_patch/scene.cpp
  src/project_patch/view.cpp
  src/project_patch/canvas.cpp
  src/project_patch/panels.cpp  # TX.panel-faces: closed stock devices as their Device-view panels
  src/project_patch/lane_window.cpp  # T5.x-one-circuit: the MIDI / audio sources as windows onto their lanes
  src/project_patch/lane_edit.cpp  # TX.lane-edit: the windows as an edit surface of the arrangement
  src/project_patch/sketch.cpp)
