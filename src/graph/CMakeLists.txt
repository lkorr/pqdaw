pq_add_module(pq_graph
  SOURCES
    src/types.cpp
    src/atoms.cpp
    src/patch.cpp
    src/validate.cpp
    src/patch_equal.cpp
    src/patch_file.cpp
  DEPS pq_core
)
