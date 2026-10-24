pq_add_module(pq_engine
  SOURCES
    src/asio_convert.cpp
    src/asio_device.cpp
    src/audio_device.cpp
    src/automation.cpp
    src/block_adapter.cpp
    src/callback_stats.cpp
    src/image_runner.cpp
    src/interpreter.cpp
    src/note_list.cpp
    src/null_device.cpp
    src/offline_render.cpp
    src/program_host.cpp
    src/project_program.cpp
    src/project_compile.cpp
    src/rtaudio_device.cpp
    src/sample_convert.cpp
    src/sequence.cpp
    src/state_transfer.cpp
    src/timeline.cpp
    src/voices.cpp
    src/wasapi_device.cpp
    src/wav_file.cpp
  DEPS pq_core pq_dsp pq_graph
  PRIVATE_DEPS rtaudio ole32 avrt advapi32 user32
)
