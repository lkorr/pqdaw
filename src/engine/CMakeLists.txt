pq_add_module(pq_engine
  SOURCES
    src/audio_device.cpp
    src/block_adapter.cpp
    src/callback_stats.cpp
    src/null_device.cpp
    src/rtaudio_device.cpp
  DEPS pq_core
  PRIVATE_DEPS rtaudio
)
