# library (T5.9): the browser index (places, indexer thread, change watcher, index file + journal), search and tags
# (ARCHITECTURE §1). Depends on media (probes) and document (the stock device catalog for categories).
pq_add_module(pq_library
  SOURCES
    src/win_util.cpp
    src/index.cpp
    src/scan.cpp
    src/index_file.cpp
    src/indexer.cpp
    src/search.cpp
    src/tags.cpp
  DEPS pq_media pq_doc
  PRIVATE_DEPS shell32 ole32
)
