#pragma once

// Hand-written C++ reference of the Meta Operator (stdlib/devices/meta_operator.pqp, TMM.12); tests only. One voice at
// a time, float32 like the patch: per operator a Phase accumulator (the uint32 phasor at voice.hz · n / d, + phase,
// + pm, frac, · amp), a Wavetable reading a sine (the engine's default table: the reference reads sin(2πp) in double,
// the table's interpolation error is ≈ 3e-7), an Envelope used as a VCA (stdlib msenv's arithmetic, mode None, soft
// start, Initial 0, Peak 1, End 0: TMM.11 made it bit-identical with every curve 0, so opref::MsEnv is its reference)
// · amount. The algorithm routing is written out per algorithm from operator.pqp's table (the same topologies), not
// from the patch's mux arms, so a routing test against it checks the arms independently. D's phase gets
// feedback · D's output of the previous sample.

#include <array>
#include <cmath>

#include "operator_reference.h"

namespace moref {

struct OpState {
  float n = 1.0f, d = 1.0f;                       // the device's ratio params
  float freq = 0.0f, phase = 0.0f, amp = 1.0f;     // the Phase accumulator's params
  float wtPhase = 0.0f;                            // the Wavetable's phase param
  float attack = 0.005f, decay = 0.4f, sustain = 0.6f, release = 0.25f, amount = 1.0f;  // the Envelope's
};

struct Voice {
  int algorithm = 1;
  float feedback = 0.0f;
  std::array<OpState, 4> op{};
  std::array<stdref::Phasor, 4> ph{};
  std::array<opref::MsEnv, 4> env{};
  float prevD = 0.0f;  // z1(envD)

  static float sine(float p) { return static_cast<float>(std::sin(6.283185307179586 * static_cast<double>(p))); }

  // One sample: the voice's note hz, velocity and gate. `done` as the patch's output.
  float tick(float hz, float vel, bool gate, bool& done) {
    std::array<float, 4> out{};
    std::array<bool, 4> fin{};
    auto oper = [&](int i, float pm) {
      const OpState& o = op[static_cast<size_t>(i)];
      const float h = hz * (o.n / std::max(o.d, 1.0f));
      const float p = ph[static_cast<size_t>(i)].tick(h + o.freq, false, opref::kSr);
      const float acc = o.amp * opref::frac((p + o.phase) + pm);
      const float w = sine(opref::frac(acc + o.wtPhase));
      opref::EnvParams e;
      e.a = o.attack;
      e.d = o.decay;
      e.r = o.release;
      e.init = 0.0f;
      e.peak = 1.0f;
      e.sus = o.sustain;
      e.end = 0.0f;
      bool dn = false;
      const float lvl = env[static_cast<size_t>(i)].tick(gate, opref::MsEnv::coef(e, 1.0f, 120.0f), 0.0f, dn);
      fin[static_cast<size_t>(i)] = dn;
      out[static_cast<size_t>(i)] = lvl * o.amount * w;
    };
    const int a = algorithm;
    oper(3, prevD * feedback);
    prevD = out[3];
    const float D = out[3];
    oper(2, (a == 1 || a == 3 || a == 6 || a == 7 || a == 9 || a == 10) ? D : 0.0f);
    const float C = out[2];
    float pmB = 0.0f;
    switch (a) {
      case 1: case 4: case 6: pmB = C; break;
      case 2: case 8: pmB = C + D; break;
      case 10: pmB = D; break;
      default: break;
    }
    oper(1, pmB);
    const float B = out[1];
    float pmA = 0.0f;
    switch (a) {
      case 1: case 2: case 7: pmA = B; break;
      case 3: pmA = B + C; break;
      case 4: pmA = B + D; break;
      case 5: pmA = (B + C) + D; break;
      case 10: pmA = D; break;
      default: break;
    }
    oper(0, pmA);
    const float A = out[0];
    float mix = A;
    bool dn = fin[0];
    switch (a) {
      case 6: case 8: mix = A + B, dn = fin[0] && fin[1]; break;
      case 7: mix = A + C, dn = fin[0] && fin[2]; break;
      case 9: case 10: mix = (A + B) + C, dn = fin[0] && fin[1] && fin[2]; break;
      case 11: mix = ((A + B) + C) + D, dn = fin[0] && fin[1] && fin[2] && fin[3]; break;
      default: break;
    }
    done = dn;
    return mix * vel;
  }
};

}  // namespace moref
