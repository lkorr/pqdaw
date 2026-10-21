pq_add_module(pq_engine
  SOURCES
    src/asio_convert.cpp
    src/asio_device.cpp
    src/audio_device.cpp
    src/block_adapter.cpp
    src/callback_stats.cpp
    src/image_runner.cpp
    src/interpreter.cpp
    src/null_device.cpp
    src/program_host.cpp
    src/rtaudio_device.cpp
    src/sample_convert.cpp
    src/state_transfer.cpp
    src/voices.cpp
    src/wasapi_device.cpp
  DEPS pq_core pq_dsp pq_graph
  PRIVATE_DEPS rtaudio ole32 avrt advapi32 user32
)
