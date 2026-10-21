pq_add_module(pq_tuning
  SOURCES
    src/rational.cpp
    src/monzo.cpp
    src/edo.cpp
    src/tuning.cpp
    src/js_number.cpp
    src/mos.cpp
    src/mos_regions.cpp
    src/mos_data.cpp
    src/mos_data_tables.cpp
    src/mos_tuning.cpp
    src/scala.cpp
    src/scale_tuning.cpp
  DEPS pq_core
)
