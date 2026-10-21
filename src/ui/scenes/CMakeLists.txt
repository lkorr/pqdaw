# pq_ui_scenes: the named test scenes (pq/ui/scenes.h) for `pqdaw --screenshot`, golden-image tests and the
# ui benchmarks. Kept out of pq_ui so the toolkit carries no test content (T0.1-fix1, review M0 L10).
pq_add_module(pq_ui_scenes
  SOURCES
    src/scene_controls.cpp
    src/scene_lines.cpp
    src/scene_primitives.cpp
    src/scene_text.cpp
    src/scene_widgets.cpp
    src/scenes.cpp
  DEPS pq_ui)
# The control gallery paints with pq_ui's private control-painting helpers (src/ui/src/controls_paint.h).
target_include_directories(pq_ui_scenes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(pq_ui_scenes PRIVATE /external:W0)
