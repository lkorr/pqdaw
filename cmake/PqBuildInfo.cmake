# Script mode (cmake -P), run at every build by the pq_build_info target (src/app/CMakeLists.txt; T5.x-bugreport):
# writes the commit the source tree is at into ${OUT}, rewriting it only when the content changed, so a new commit or a
# change of the dirty state recompiles the one file that includes it (src/app/src/build_info.cpp) and nothing else.
# No absolute path is written (the compiler cache shares objects between checkouts).
#   -DSRC=<source dir> -DOUT=<header> -DPRESET=<check|release|ship|dev>
set(commit "unknown")
set(branch "unknown")
set(date "unknown")
set(dirty 0)
find_program(PQ_GIT git)
if(PQ_GIT)
  execute_process(COMMAND "${PQ_GIT}" -C "${SRC}" rev-parse --short=12 HEAD
                  OUTPUT_VARIABLE out RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(rc EQUAL 0 AND out)
    set(commit "${out}")
    execute_process(COMMAND "${PQ_GIT}" -C "${SRC}" rev-parse --abbrev-ref HEAD
                    OUTPUT_VARIABLE out RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(rc EQUAL 0 AND out)
      set(branch "${out}")
    endif()
    execute_process(COMMAND "${PQ_GIT}" -C "${SRC}" log -1 --format=%ci
                    OUTPUT_VARIABLE out RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(rc EQUAL 0 AND out)
      set(date "${out}")
    endif()
    # Tracked files only (untracked build outputs and bug_reports/ do not make a build "dirty").
    execute_process(COMMAND "${PQ_GIT}" -C "${SRC}" status --porcelain --untracked-files=no
                    OUTPUT_VARIABLE out RESULT_VARIABLE rc ERROR_QUIET)
    if(rc EQUAL 0 AND NOT out STREQUAL "")
      set(dirty 1)
    endif()
  endif()
endif()
set(text "#pragma once
// GENERATED at build time by cmake/PqBuildInfo.cmake: the commit this build was made from (the bug report's build line).
#define PQ_BUILD_COMMIT \"${commit}\"
#define PQ_BUILD_BRANCH \"${branch}\"
#define PQ_BUILD_COMMIT_DATE \"${date}\"
#define PQ_BUILD_DIRTY ${dirty}
#define PQ_BUILD_PRESET \"${PRESET}\"
")
set(old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
