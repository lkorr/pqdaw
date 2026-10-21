pq_add_module(pq_doc
  SOURCES
    src/note_seq.cpp
    src/model.cpp
    src/change_set.cpp
    src/checks.cpp
    src/apply.cpp
    src/store.cpp
    src/json_io.cpp
    src/serialize.cpp
    src/project_io.cpp
    src/edit_common.cpp
    src/edit_tracks.cpp
    src/edit_clips.cpp
    src/edit_notes.cpp
    src/edit_time.cpp
    src/edit_automation.cpp
  # nlohmann/json: schema migrations (generic DOM transforms of old project files); the public header names
  # nlohmann::json in MigrationFn.
  DEPS pq_tuning pq_core nlohmann_json::nlohmann_json
)
