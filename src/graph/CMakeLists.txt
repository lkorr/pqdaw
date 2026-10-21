pq_add_module(pq_graph
  SOURCES
    src/types.cpp
    src/atoms.cpp
    src/kernel_map.cpp
    src/patch.cpp
    src/validate.cpp
    src/patch_equal.cpp
    src/patch_file.cpp
    src/lexer.cpp
    src/text_common.cpp
    src/parse.cpp
    src/print.cpp
    src/infer.cpp
  DEPS pq_core pq_dsp
)
