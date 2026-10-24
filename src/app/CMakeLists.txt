# pq_app: everything in the executable except main(), so tests can link the CLI and commands.
pq_add_module(pq_app
  SOURCES
    src/cli.cpp
    src/commands.cpp
    src/audio_commands.cpp
    src/play_command.cpp
    src/shell_app.cpp
    src/sim_transport.cpp
    src/startup.cpp
    src/pipeline_cache.cpp
    src/file_watch.cpp
    src/project_link.cpp
    src/audio_output.cpp
    src/audio_manager.cpp
    src/user_settings.cpp
    src/render_command.cpp
  DEPS pq_core pq_ui pq_ui_scenes pq_views pq_doc pq_engine nlohmann_json::nlohmann_json
  PRIVATE_DEPS SDL3::SDL3-static)
target_compile_options(pq_app PRIVATE /external:W0)
# T3.17: the stdlib fallback location (pq::app::stdlibDir) for dev builds and tests.
pq_use_build_paths(pq_app)
target_compile_definitions(pq_app PRIVATE PQ_SOURCE_DIR=pq_build_source_dir)

pq_add_executable(pqdaw
  SOURCES src/main.cpp
  DEPS pq_app)
