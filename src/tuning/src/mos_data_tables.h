#pragma once

// Arrays defined in the generated mos_data_tables.cpp.

#include <cstddef>

#include "pq/tuning/mos_data.h"

namespace pq::tuning::detail {

extern const Temperament kTemperaments[];
extern const size_t kTemperamentCount;
extern const NamedColor kTemperamentColors[];
extern const size_t kTemperamentColorCount;
extern const MosNameEntry kMosNames[];
extern const size_t kMosNameCount;
extern const MosNameEntry kMultiPeriodMosNames[];
extern const size_t kMultiPeriodMosNameCount;
extern const ModeNameEntry kModeNames[];
extern const size_t kModeNameCount;

}  // namespace pq::tuning::detail
