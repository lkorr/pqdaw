# pq_app: everything in the executable except main(), so tests can link the CLI and commands.
pq_add_module(pq_app
  SOURCES
    src/cli.cpp
    src/commands.cpp
  DEPS pq_core pq_ui
  PRIVATE_DEPS SDL3::SDL3-static)
target_compile_options(pq_app PRIVATE /external:W0)

pq_add_executable(pqdaw
  SOURCES src/main.cpp
  DEPS pq_app)
