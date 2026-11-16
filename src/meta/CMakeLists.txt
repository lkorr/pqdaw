# pq_meta (TMM.3): meta modules — the type registry, state data, deterministic expansion to graph patches, the prelude
# loaders give `meta_<type>` calls, and the built-in types (the Wavetable, the Envelope, the analysers), the off-thread
# frame-table builder (TMM.5). docs/plans/MM.md "Architecture".
pq_add_module(pq_meta
  SOURCES
    src/data.cpp
    src/envelope.cpp
    src/expand.cpp
    src/noise.cpp
    src/registry.cpp
    src/table_builder.cpp
    src/spectrum.cpp
    src/oscilloscope.cpp
    src/spectrogram.cpp
    src/analysers.cpp       # VZ.4 / VZ.5: the L / R pass-through analysers (loudness, stereo)
    src/user_defaults.cpp   # VZ.1: saved defaults for new instances
    src/wavetable.cpp
  DEPS pq_core pq_graph pq_dsp)
