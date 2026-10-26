pq_add_module(pq_dsp
  SOURCES
    src/atoms.cpp
    src/kernels_math.cpp
    src/kernels_logic.cpp
    src/kernels_memory.cpp
    src/kernels_gen.cpp
    src/kernels_tuning.cpp
    src/kernels_gain.cpp
    src/tables.cpp
    src/fft.cpp
    src/spectrum.cpp
  DEPS pq_core
)
