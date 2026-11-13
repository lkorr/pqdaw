#pragma once

// EQ.2 (docs/plans/EQ.md "Analytic responses"): the analytic frequency response of a GROUP, from its In terminal to its
// Out terminal (model.h "Groups"), for the response displays (EQ.4): a band, a whole EQ with its M/S matrix, any group.
//
// Prepare once per structural revision of the project (what lower() compiles): it takes a lowering of the project (the
// app's current LoweredProject, or one made for this), infers the patch of the track the group is lowered in (the same
// flattening the compiler does, graph::infer) with probes on the group call's `input` and `y` (the In terminal's value
// and the Out terminal's sum), and prepares graph::LinearResponse (pq/graph/response.h) between them. Then update()
// re-reads only the controls: every runtime param the response depends on is a device param `d<id>__<name>`
// (lower.h "Rack chain params"): an undriven control's value is its curve at its knob position (paramValue: what
// lowerParams writes), a driven one's param is its knob position (paramPosition) and the `curve` atom after it is
// evaluated through the control's curve (controlCurve) — so a knob drag, a curve edit or a typed value never relowers.
// A structural edit (a literal value block, a band added, Stages, Mode: lower.h "GU.f4") changes the lowering: call
// prepare() again. Params that are not a device's (a mixer's) keep the value the lowering had.
//
// TX.mod-bands: a DRIVEN control (a wire, an automation lane or a macro mapping on it) is a set port of the response
// (graph::LinearResponse "SET PORTS"; its final value, doc::CurveSite::value), held at the control's curve at a
// position: the LIVE MODULATED one a display gives (setLive: the ModulationRing's current), else the knob's. So a band
// an LFO sweeps keeps its curve, drawn where the modulation has it, whatever the modulator is.
//
// DY.3: GroupTransfer is the same binding around graph::StaticTransfer (the static curve Out = f(In) of a memoryless
// group); both share GroupEvaluator (prepare, update, the driven controls).
//
// Not RT code (prepare allocates; update reads the project); evaluate() is allocation-free (the UI calls it on every
// drag).

#include <complex>
#include <span>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/devices.h"
#include "pq/document/lower.h"
#include "pq/document/model.h"
#include "pq/document/panel.h"
#include "pq/graph/infer.h"
#include "pq/graph/response.h"

namespace pq::doc {

// The document side shared by GroupResponse and GroupTransfer (DY.3): finding and inferring the group's track patch,
// the evaluator's ends (the group call's In and Out), and re-reading the controls it depends on (params, knob curves,
// driven controls held as set ports). A derived class owns its graph evaluator (a graph::ControlEval).
class GroupEvaluator {
public:
  virtual ~GroupEvaluator() = default;
  // Errors: the group is not lowered in `lowered` (deactivated, not a group), the track patch does not infer, the In
  // terminal carries nothing (a free group with no wire into it). `devices`: the library `lowered` was made with
  // (the callees' param declarations).
  Status prepare(const Project& project, const LoweredProject& lowered, const DeviceLibrary& devices, DeviceId group);
  // EQ.4: prepare() for several groups at once (out[k] for groups[k]; any mix of evaluators): one inference per track
  // patch, with probes on every group's In and Out there (a display's EQ, its chains and its 64 bands: one flattening,
  // not 67). Per group its status.
  static std::vector<Status> prepareMany(const Project& project, const LoweredProject& lowered, const DeviceLibrary& devices,
                                         std::span<const DeviceId> groups, std::span<GroupEvaluator* const> out);
  // The controls' current values from `project` (the same structural revision as prepared).
  void update(const Project& project) noexcept;
  // EQ.4: changes whenever prepare() ran or update() changed a value the evaluator reads (a display re-evaluates only
  // then: a drag on one band leaves the other bands' curves as they are).
  uint64_t stamp() const noexcept { return stamp_; }
  // TX.mod-bands: the driven controls the evaluator reads, as Control bindings (a display looks up their live rings).
  std::span<const Binding> driven() const noexcept { return drivenBindings_; }
  // The live modulated position of driven()[i] (in [0, 1]); < 0 or NaN: none (the knob's own). Allocation-free; true
  // when the evaluator changed (its stamp moved).
  bool setLive(uint32_t i, double position) noexcept;

protected:
  // Prepares the evaluator between the ends (a fresh one: it replaces what it held; invalid ends: Unavailable).
  virtual void prepareGraph(const graph::TypedPatch& typed, graph::ResponseEnd in, graph::ResponseEnd out,
                            std::span<const uint32_t> settable) = 0;
  virtual graph::ControlEval& controls() noexcept = 0;

private:
  struct Control {
    uint32_t index = 0;  // into controls().paramNodes() / curveNodes()
    DeviceId device;
    std::string param;
    graph::ParamDecl decl;  // the callee's declaration
    bool position = false;  // a driven control: its knob position
    dsp::Curve curve;       // curves_: the curve set last
    bool curveSet = false;
  };
  Status finish(const Project& project, const LoweredProject& lowered, const DeviceLibrary& devices, DeviceId group,
                const graph::InferResult& inf, uint32_t track, uint32_t inSource, uint32_t outSource);
  struct DrivenControl {
    uint32_t index = 0;  // into controls().setPorts()
    DeviceId device;
    std::string param;
    graph::ParamDecl decl;
    dsp::Curve curve;
    double knob = 0.0;   // its knob position (the document's)
    double live = -1.0;  // the live modulated position (< 0: none)
    bool held = false;   // a value was given
  };
  void hold(DrivenControl& c, bool& changed) noexcept;
  std::vector<Control> params_, curves_;
  std::vector<DrivenControl> driven_;
  std::vector<Binding> drivenBindings_;
  uint64_t stamp_ = 0;
};

// EQ.2: the analytic frequency response of a group (graph::LinearResponse).
class GroupResponse final : public GroupEvaluator {
public:
  // H from In channel i to Out channel j at each frequency (graph::LinearResponse::evaluate's layout).
  graph::ResponseKind evaluate(std::span<const double> hz, double sampleRate,
                               std::span<std::complex<double>> out) noexcept {
    return response_.evaluate(hz, sampleRate, out);
  }
  uint16_t inChannels() const noexcept { return response_.sourceChannels(); }
  uint16_t outChannels() const noexcept { return response_.sinkChannels(); }
  const graph::LinearResponse& response() const noexcept { return response_; }

private:
  void prepareGraph(const graph::TypedPatch& typed, graph::ResponseEnd in, graph::ResponseEnd out,
                    std::span<const uint32_t> settable) override {
    response_.prepare(typed, in, out, settable);
  }
  graph::ControlEval& controls() noexcept override { return response_; }
  graph::LinearResponse response_;
};

// DY.3 (docs/plans/DY.md "DY.3"): the static transfer curve Out = f(In) of a memoryless group (graph::StaticTransfer):
// a waveshaper's curve (a Saturator curve group), a compressor's gain computer (In = level dB, Out = reduction dB).
// Unavailable when a stateful atom (z1, a filter, `follow`) is between In and Out.
class GroupTransfer final : public GroupEvaluator {
public:
  // Out channel j at each In value (graph::StaticTransfer::evaluate's layout: ys[k * outChannels() + j]).
  graph::ResponseKind evaluate(std::span<const float> xs, double sampleRate, std::span<float> ys) noexcept {
    return transfer_.evaluate(xs, sampleRate, ys);
  }
  uint16_t outChannels() const noexcept { return transfer_.sinkChannels(); }
  graph::ResponseKind status() const noexcept { return transfer_.status(); }
  const graph::StaticTransfer& transfer() const noexcept { return transfer_; }

private:
  void prepareGraph(const graph::TypedPatch& typed, graph::ResponseEnd in, graph::ResponseEnd out,
                    std::span<const uint32_t> settable) override {
    transfer_.prepare(typed, in, out, settable);
  }
  graph::ControlEval& controls() noexcept override { return transfer_; }
  graph::StaticTransfer transfer_;
};

}  // namespace pq::doc
