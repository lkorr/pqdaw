# T0.9-fix1: differential test of pq::ui's line primitives against PQ Meter's own source (M0 review M6).
#
# PQ Meter is the same author's proprietary application ("All rights reserved", see its LICENSE.txt), so none
# of its source is copied into this public repository. The test compiles it from where it lives:
#   - src/ui/smooth_line.h is #included directly (header-only);
#   - the helper block of src/modules/spectrum.cpp (withAlpha … splineSmooth, inside an anonymous namespace
#     of a translation unit that needs PQ Meter's whole spectrum module) is cut out of the file at configure
#     time into the BUILD directory (never committed) and #included by the test;
# both against tests/ui/pqmeter_shim/imgui.h, a minimal ImGui surface.
#
# Where the PQ Meter tree is not found (any other machine, CI), the test is registered as a stub that exits
# with 77 and a message, which CTest reports as "Skipped", never as passed.
#
# Location: -DPQ_METER_SOURCE_DIR=<PQ Meter>/src, or the PQ_METER_SOURCE_DIR environment variable, else a
# directory named "pq meter" next to this repository or any of its parent directories (worktrees nest).

set(PQ_METER_SOURCE_DIR "" CACHE PATH "PQ Meter's src/ directory for the T0.9-fix1 differential test (empty: search)")

set(_pqm_src "${PQ_METER_SOURCE_DIR}")
if(NOT _pqm_src AND DEFINED ENV{PQ_METER_SOURCE_DIR})
  set(_pqm_src "$ENV{PQ_METER_SOURCE_DIR}")
endif()
if(NOT _pqm_src)
  set(_dir "${PQ_SOURCE_DIR}")
  foreach(_i RANGE 6)
    get_filename_component(_dir "${_dir}" DIRECTORY)
    if(EXISTS "${_dir}/pq meter/src/ui/smooth_line.h")
      set(_pqm_src "${_dir}/pq meter/src")
      break()
    endif()
  endforeach()
endif()

set(_pqm_skip "")
set(_pqm_gen "${CMAKE_CURRENT_BINARY_DIR}/pqmeter_extract")
if(NOT _pqm_src OR NOT EXISTS "${_pqm_src}/ui/smooth_line.h" OR NOT EXISTS "${_pqm_src}/modules/spectrum.cpp")
  set(_pqm_skip "PQ Meter source tree not found (set PQ_METER_SOURCE_DIR to its src/ directory)")
else()
  set(_spectrum "${_pqm_src}/modules/spectrum.cpp")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_spectrum}")
  file(READ "${_spectrum}" _text)
  # The block runs from withAlpha to the end of the anonymous namespace that closes after splineSmooth.
  string(FIND "${_text}" "inline ImU32 withAlpha(ImU32 c, float a)" _begin)
  if(_begin EQUAL -1)
    set(_pqm_skip "spectrum.cpp: start marker 'inline ImU32 withAlpha(ImU32 c, float a)' not found")
  else()
    string(SUBSTRING "${_text}" ${_begin} -1 _block)
    string(FIND "${_block}" "\n} // namespace" _end)
    if(_end EQUAL -1)
      set(_pqm_skip "spectrum.cpp: end of the helper namespace not found after withAlpha")
    else()
      string(SUBSTRING "${_block}" 0 ${_end} _block)
      foreach(_fn "inline ImU32 setAlpha(" "inline ImU32 scaleRgb(" "inline ImU32 brighten(" "void gradientPolyline("
                  "void gradientBand(" "void splineSmooth(")
        string(FIND "${_block}" "${_fn}" _at)
        if(_at EQUAL -1)
          set(_pqm_skip "spectrum.cpp: '${_fn}' not found in the helper block")
        endif()
      endforeach()
      string(REGEX MATCH "const ImVec4 kCurveColorDefault\\([^)]*\\);" _curve "${_text}")
      if(NOT _curve)
        set(_pqm_skip "spectrum.cpp: kCurveColorDefault not found")
      endif()
    endif()
  endif()
  if(NOT _pqm_skip)
    file(WRITE "${_pqm_gen}/pqmeter_spectrum_helpers.inc.tmp"
      "// GENERATED at configure time by tests/ui/pqmeter_diff.cmake from PQ Meter's src/modules/spectrum.cpp\n"
      "// (helper block withAlpha ... splineSmooth, plus kCurveColorDefault). Build output; never commit.\n"
      "${_curve}\n${_block}\n")
    # Only touch the include when its content changed, so an unrelated reconfigure does not rebuild the test.
    file(COPY_FILE "${_pqm_gen}/pqmeter_spectrum_helpers.inc.tmp" "${_pqm_gen}/pqmeter_spectrum_helpers.inc"
         ONLY_IF_DIFFERENT)
  endif()
endif()

if(NOT _pqm_skip)
  message(STATUS "pqdaw: PQ Meter differential test enabled (${_pqm_src})")
  pq_add_test(pq_tests_pqmeter_diff SOURCES ui/test_pqmeter_diff.cpp DEPS pq_ui)
  target_include_directories(pq_tests_pqmeter_diff PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ui/pqmeter_shim")
  # PQ Meter's code is compiled as external headers: the test includes it with <...> from these SYSTEM
  # directories (/external:anglebrackets /external:W0, cmake/PqCompilerCache.cmake), so our /W4 /WX applies
  # only to the test and the shim.
  target_include_directories(pq_tests_pqmeter_diff SYSTEM PRIVATE "${_pqm_src}" "${_pqm_gen}")
else()
  message(STATUS "pqdaw: PQ Meter differential test SKIPPED: ${_pqm_skip}")
  add_executable(pq_test_skipped skipped_test.cpp)
  pq_target_defaults(pq_test_skipped)
  add_test(NAME pq_tests_pqmeter_diff COMMAND pq_test_skipped "pq_tests_pqmeter_diff: ${_pqm_skip}")
  set_tests_properties(pq_tests_pqmeter_diff PROPERTIES SKIP_RETURN_CODE 77)
endif()
