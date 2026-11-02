// The Spectrum meta module (TX.spectrum): pq/meta/spectrum.h.
#include "pq/meta/spectrum.h"

#include <array>

namespace pq::meta {

namespace {

constexpr std::array<InputSpec, 1> kInputs = {{
    {"in", "Signal in", InputDefault::Zero, graph::Kind::Real, Reads::Signal},
}};
constexpr std::array<OutputSpec, 1> kOutputs = {{{"out", graph::kWidthInfer}}};

enum : uint32_t { kTap, kLeft, kRight, kIndex0, kIndex1 };

Status body(Builder& bld) {
  using P = Builder::PortRef;
  const P in = bld.input(0);
  bld.output(0, in);  // pass-through: the input port drives the output port, nothing in between
  // channel(x, 1) of a mono x is x (the scheduler's channel view): mono fills both tap channels.
  const P l = bld.atom("channel", {in, bld.konst(0.0, kIndex0)}, kLeft);
  const P r = bld.atom("channel", {in, bld.konst(1.0, kIndex1)}, kRight);
  bld.bind(l, "l");
  bld.bind(r, "r");
  bld.bind(bld.atom("signal_tap", {l, r, l, r, bld.slot(0)}, kTap), "tap");
  return Ok{};
}

std::vector<ResourceRequest> resources(std::span<const double>) { return {ResourceRequest{ResourceKind::SignalTap, 0}}; }

Type makeType() {
  Type t;
  t.id = "spectrum";
  t.name = "Spectrum";
  t.version = 1;
  t.inputs = kInputs;
  t.outputs = kOutputs;
  t.chainInput = 0;
  t.resources = &resources;
  t.body = &body;
  return t;
}

}  // namespace

const Type& spectrumType() noexcept {
  static const Type t = makeType();
  return t;
}

}  // namespace pq::meta
