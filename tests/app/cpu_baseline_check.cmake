# Runs EXE with PQ_TEST_CPU_DISABLE=DISABLE (an emulated CPU without those features) and checks that the startup
# CPU baseline check (pq/core/cpu_baseline.h) rejects it: exit code 1 and the message naming EXPECT on stderr,
# and nothing else ran (no output on stdout, e.g. --help's usage text).
#   cmake -DEXE=<pqdaw> -DDISABLE=avx2 -DEXPECT=AVX2 -P cpu_baseline_check.cmake
foreach(v EXE DISABLE EXPECT)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "cpu_baseline_check.cmake: -D${v}=... is required")
  endif()
endforeach()

set(ENV{PQ_TEST_CPU_DISABLE} "${DISABLE}")
execute_process(COMMAND "${EXE}" --help
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "exit code: ${rc}\nstderr: ${err}")
if(NOT rc STREQUAL "1")
  message(FATAL_ERROR "expected exit code 1, got '${rc}'")
endif()
if(NOT err MATCHES "this CPU is not supported.*AVX2 and FMA.*Missing: ${EXPECT}[.]")
  message(FATAL_ERROR "stderr does not name the missing feature(s) '${EXPECT}'")
endif()
if(NOT out STREQUAL "")
  message(FATAL_ERROR "the program ran past the check (stdout: ${out})")
endif()
