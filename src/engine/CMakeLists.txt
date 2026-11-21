pq_add_module(pq_engine
  SOURCES
    src/arp.cpp
    src/arp_sketch.cpp  # AP.10: the resolved cycle (the panel's pattern / bend views)
    src/asio_convert.cpp
    src/audio_sequence.cpp
    src/clip_player.cpp
    src/asio_device.cpp
    src/audio_device.cpp
    src/automation.cpp
    src/background_compile.cpp
    src/block_adapter.cpp
    src/engine.cpp
    src/callback_stats.cpp
    src/capture_fifo.cpp
    src/image_host.cpp
    src/image_runner.cpp
    src/inspect.cpp
    src/interpreter.cpp
    src/newton.cpp
    src/note_list.cpp
    src/note_stage.cpp
    src/note_stage_offline.cpp  # AP.7: note stages offline (the roll preview, Flatten)
    src/null_device.cpp
    src/offline_render.cpp
    src/preview_player.cpp
    src/program_host.cpp
    src/project_program.cpp
    src/project_compile.cpp
    src/recorder.cpp
    src/latency_plan.cpp
    src/live_input.cpp
    src/metronome.cpp
    src/rtaudio_device.cpp
    src/sample_convert.cpp
    src/sequence.cpp
    src/state_transfer.cpp
    src/stream_trace.cpp
    src/timeline.cpp
    src/tuning_signal.cpp
    src/voices.cpp
    src/wasapi_device.cpp
  DEPS pq_core pq_dsp pq_media pq_graph pq_tuning
  PRIVATE_DEPS rtaudio ole32 avrt advapi32 user32
)
