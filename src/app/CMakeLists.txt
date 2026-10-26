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
    src/jit_cache.cpp
  DEPS pq_core pq_ui pq_ui_scenes pq_views pq_doc pq_engine pq_compile nlohmann_json::nlohmann_json
  PRIVATE_DEPS SDL3::SDL3-static)
target_compile_options(pq_app PRIVATE /external:W0)
# T3.17: the stdlib fallback location (pq::app::stdlibDir) for dev builds and tests.
pq_use_build_paths(pq_app)
target_compile_definitions(pq_app PRIVATE PQ_SOURCE_DIR=pq_build_source_dir)

pq_add_executable(pqdaw
  SOURCES src/main.cpp
  DEPS pq_app)

# T1b.4: the AOT seed (pq/app/jit_cache.h, QUESTIONS Q116). Right after every link, the new executable compiles
# the stock units (a new project, default-instrument tracks; 44.1/48/88.2/96 kHz) for the baseline CPU into
# <exe dir>/jit-aot, which it reads at run time: its entries are keyed by this very binary, so they are rebuilt with
# it (well under a second in release). Ship the directory next to pqdaw.exe.
option(PQ_JIT_AOT_SEED "Compile the JIT AOT seed (stock units) next to pqdaw.exe after each link" ON)
if(PQ_JIT_AOT_SEED)
  add_custom_command(TARGET pqdaw POST_BUILD
    COMMAND pqdaw --jit-aot "$<TARGET_FILE_DIR:pqdaw>/jit-aot"
    COMMENT "pqdaw: JIT AOT seed of the stock units (jit-aot/)"
    VERBATIM)
endif()
