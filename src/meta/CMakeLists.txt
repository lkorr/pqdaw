# pq_meta (TMM.3): meta modules — the type registry, state data, deterministic expansion to graph patches, the prelude
# loaders give `meta_<type>` calls, and the built-in types (the Phase accumulator). docs/plans/MM.md "Architecture".
pq_add_module(pq_meta
  SOURCES
    src/data.cpp
    src/expand.cpp
    src/registry.cpp
    src/phase_acc.cpp
  DEPS pq_core pq_graph pq_dsp)
