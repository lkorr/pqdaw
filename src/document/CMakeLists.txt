pq_add_module(pq_doc
  SOURCES
    src/note_seq.cpp
    src/model.cpp
    src/audio_clip.cpp
    src/audio_sequence.cpp
    src/change_set.cpp
    src/checks.cpp
    src/apply.cpp
    src/store.cpp
    src/json_io.cpp
    src/serialize.cpp
    src/project_io.cpp
    src/samples.cpp
    src/edit_common.cpp
    src/edit_tracks.cpp
    src/edit_clips.cpp
    src/edit_notes.cpp
    src/edit_time.cpp
    src/edit_automation.cpp
    src/edit_mixer.cpp
    src/edit_tunings.cpp
    src/edit_audio.cpp
    src/edit_devices.cpp
    src/edit_racks.cpp
    src/devices.cpp
    src/audibility.cpp
    src/lower.cpp
    src/pitch_resolve.cpp
  # nlohmann/json: schema migrations (generic DOM transforms of old project files); the public header names
  # nlohmann::json in MigrationFn.
  # pq_graph: project → graph lowering (lower.h, T3.17).
  DEPS pq_tuning pq_core pq_graph nlohmann_json::nlohmann_json
)
