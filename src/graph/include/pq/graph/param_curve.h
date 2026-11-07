#pragma once

// GU.1 (docs/plans/GU.md "Curves"; pq/dsp/curve.h): the curve a `param` declaration gives — what every param of the
// patch language plays unless its device stores another (pq/document/model.h ControlCurve). One definition for the
// document (doc::declaredCurve) and the meta modules (meta::paramCurve, from the expansion's own declarations), so a
// face and the lowering read the same curve.
//
// The curve: its range ([lo..hi]; none: [min(0, default), max(1, default)]); its `@curve` preset — `linear`, `octave`
// (its old name `exp`; lo > 0), `exponential` / `logarithmic` [, k] (bend ±k, default ±4.8), `exp0`, k (exponential
// from exactly lo: bend k·ln 2, default 14) —; its unit (@unit, else the literals'): a dB gain with a positive floor is a
// curve in dB (linear, or bent with exponential / logarithmic) with the DbToGain output; Hz, s, %, cents set the
// readout; `@steps(n)` (n > 1) and a bool (0 … 1) step 1; `@wrap` CurveMode::Wrap; `@signed` CurveMode::Signed.
// Not RT code.

#include "pq/dsp/curve.h"
#include "pq/graph/patch.h"

namespace pq::graph {

dsp::Curve declaredCurve(const ParamDecl& decl) noexcept;

}  // namespace pq::graph
