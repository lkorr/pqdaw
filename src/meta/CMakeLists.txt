# pq_meta (TMM.3): meta modules — the type registry, state data, deterministic expansion to graph patches, the prelude
# loaders give `meta_<type>` calls, and the built-in types (the Wavetable, the Envelope, the analysers; GU.4: the retired types' state descriptions), the off-thread
# frame-table builder (TMM.5). docs/plans/MM.md "Architecture".
pq_add_module(pq_meta
  SOURCES
    src/data.cpp
    src/envelope.cpp
    src/expand.cpp
    src/registry.cpp
    src/retired.cpp
    src/table_builder.cpp
    src/spectrum.cpp
    src/oscilloscope.cpp
    src/spectrogram.cpp
    src/wavetable.cpp
  DEPS pq_core pq_graph pq_dsp)
