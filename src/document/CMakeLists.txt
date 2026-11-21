pq_add_module(pq_doc
  SOURCES
    src/note_seq.cpp
    src/model.cpp
    src/knob_positions.cpp
    src/audio_clip.cpp
    src/audio_sequence.cpp
    src/change_set.cpp
    src/checks.cpp
    src/apply.cpp
    src/store.cpp
    src/json_io.cpp
    src/serialize.cpp
    src/project_io.cpp
    src/library_io.cpp
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
    src/edit_takes.cpp
    src/edit_render.cpp  # FZ.4: flatten, bounce to a new track, consolidate audio
    src/edit_devices.cpp
    src/edit_cables.cpp  # DY.2: cable sources (the sidechain picker)
    src/edit_racks.cpp
    src/edit_eq.cpp
    src/edit_math.cpp
    src/racks.cpp
    src/edit_groups.cpp
    src/edit_blocks.cpp
    src/modules.cpp
    src/edit_panels.cpp
    src/panel.cpp
    src/edit_paths.cpp
    src/edit_library.cpp
    src/edit_meta.cpp
    src/devices.cpp
    src/meta.cpp
    src/main_path.cpp
    src/note_flatten.cpp  # AP.7: Flatten to clip (the generated notes of a track's note devices)
    src/note_devices.cpp  # AP.2: note devices (the arpeggiator): its params from pq/engine/arp.h's table
    src/note_pattern.cpp  # AP.10: the resolved pattern of a note device (engine sketchArp over its values)
    src/wire_types.cpp  # WI.3: declared-type clashes a wiring gesture refuses
    src/instrument_groups.cpp
    src/audibility.cpp
    src/lower.cpp
    src/group_response.cpp
    src/pitch_resolve.cpp
    src/note_bend.cpp  # T7.11a: derived bends between notes (the effective note curve)
    src/edit_ji.cpp    # T7.13: JI group, warp and note-curve edit operations
    src/input_map.cpp
    src/midi_file.cpp  # MF.1: Standard MIDI File reader / writer, note resolution
    src/midi_import.cpp  # MF.2: MIDI file import (resolved notes → clips, tempo, signatures)
    src/midi_export.cpp  # MF.3: MIDI file export (clips → SMF: 12edo, MPE, Degrees)
    src/midi_key.cpp  # SM.1: a note's MIDI key (voice.key, MIDI export's Degrees / 12edo keys)
  # nlohmann/json: schema migrations (generic DOM transforms of old project files); the public header names
  # nlohmann::json in MigrationFn.
  # pq_graph: project → graph lowering (lower.h, T3.17).
  # pq_meta (TMM.3): meta-module state in the model, their expansions in the lowering.
  # pq_engine (AP.2, private): the note devices' param table (pq/engine/arp.h kArpParamInfo, header data only).
  DEPS pq_tuning pq_core pq_graph pq_meta nlohmann_json::nlohmann_json
  PRIVATE_DEPS pq_engine
)
