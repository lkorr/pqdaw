#pragma once

// BR.A: test access to the TRACE SCOPE (trace_element.cpp: a `scope` element bound to several signals).

#include <optional>
#include <utility>

#include "pq/views/panel/element.h"

namespace pq::views::panel::test {

// The value range its auto scale shows now (lo at the bottom, hi at the top: −1 … 1 by default, widened to what the
// traces spanned over the last ~5 s, eased); nullopt: not a trace scope.
std::optional<std::pair<float, float>> traceRange(const Element& e);

}  // namespace pq::views::panel::test
