pq_add_module(pq_engine
  SOURCES
    src/asio_convert.cpp
    src/audio_sequence.cpp
    src/clip_player.cpp
    src/stretch_pool.cpp
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
  PRIVATE_DEPS rtaudio ole32 avrt advapi32 user32 pq_signalsmith
)
# T6.5: signalsmith-stretch is third-party code instantiated in this TU alone: its warnings off there (/bigobj: the
# library's templates exceed MSVC's default section count).
set_source_files_properties(src/stretch_pool.cpp PROPERTIES COMPILE_OPTIONS "/W0;/bigobj")
