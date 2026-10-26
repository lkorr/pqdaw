# --- shaders: GLSL → SPIR-V at build time, embedded as C initialiser lists (no runtime files) ---------
set(PQ_UI_SHADERS ui.vert ui.frag)
set(_pq_ui_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(_pq_ui_spv)
foreach(_s ${PQ_UI_SHADERS})
  set(_out ${_pq_ui_gen}/${_s}.spv.inc)
  add_custom_command(
    OUTPUT ${_out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_pq_ui_gen}
    COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -Werror -mfmt=c -o ${_out} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_s}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_s}
    COMMENT "glslc ${_s}"
    VERBATIM)
  list(APPEND _pq_ui_spv ${_out})
endforeach()

pq_add_module(pq_ui
  SOURCES
    src/actions.cpp
    src/atlas.cpp
    src/colormap.cpp
    src/controls.cpp
    src/draw_list.cpp
    src/frame_pacer.cpp
    src/gpu.cpp
    src/image.cpp
    src/input.cpp
    src/keymap_file.cpp
    src/lines.cpp
    src/offscreen.cpp
    src/overlay.cpp
    src/raster.cpp
    src/renderer.cpp
    src/scene_textures.cpp
    src/test_driver.cpp
    src/text.cpp
    src/text_edit.cpp
    src/code_editor.cpp
    src/theme.cpp
    src/tile_atlas.cpp
    src/ui_tree.cpp
    src/vma_impl.cpp
    src/widget.cpp
    src/window.cpp
    ${_pq_ui_spv}
  DEPS pq_core Vulkan::Vulkan
  PRIVATE_DEPS pq_vma SDL3::SDL3-static ZLIB::ZLIB dwmapi pq_harfbuzz freetype nlohmann_json::nlohmann_json)
target_include_directories(pq_ui PRIVATE ${_pq_ui_gen})
# Fallback location of the bundled fonts for dev builds and tests (see pq::ui::assetsDir()).
pq_use_build_paths(pq_ui)
target_compile_definitions(pq_ui PRIVATE PQ_SOURCE_ASSETS_DIR=pq_build_source_assets_dir)
# Third-party headers (VMA, SDL, zlib, Vulkan, FreeType, HarfBuzz) are SYSTEM includes: keep their warnings out of /W4 /WX.
target_compile_options(pq_ui PRIVATE /external:W0)

# Test scenes (--screenshot, goldens, benchmarks): a separate library so they are not part of pq_ui.
add_subdirectory(scenes)
