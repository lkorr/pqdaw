pq_add_module(pq_doc
  SOURCES
    src/note_seq.cpp
    src/model.cpp
    src/change_set.cpp
    src/apply.cpp
    src/store.cpp
  DEPS pq_tuning pq_core
)
