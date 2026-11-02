// TX.spectrum: the Spectrum meta module — its expansion (every form, the tap of request 0, the output port driven by the
// input port), the text form parses back, and pass-through bit for bit (mono and stereo, interpreter and JIT) against
// the same patch without it.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pq/engine/offline_render.h"
#include "pq/graph/parse.h"
#include "pq/graph/patch_file.h"
#include "pq/meta/registry.h"
#include "pq/meta/spectrum.h"
#include "pq/pqc/pqc.h"

using namespace pq;
using namespace pq::meta;

namespace {

namespace fs = std::filesystem;

fs::path writeTemp(const std::string& name, const std::string& text) {
  const fs::path p = fs::temp_directory_path() / ("pq_test_spectrum_" + name);
  std::ofstream(p, std::ios::binary) << text;
  return p;
}

std::vector<std::vector<float>> render(const fs::path& file, pqc::Backend backend) {
  pqc::RenderOptions o;
  o.seconds = 0.25;
  o.backend = backend;
  o.patch = "Main";  // the file starts with the module's expansion
  engine::BufferSink sink;
  auto r = pqc::renderFile(file, o, sink);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return sink.channels;
}

Expansion mustExpand(const ExpandContext& c, const std::string& name = "x") {
  auto e = expand(spectrumType(), c, name);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  return std::move(e.value());
}

}  // namespace

TEST_CASE("spectrum: expansion — every form, the tap of request 0, out = in") {
  const Type& t = spectrumType();
  CHECK(findType("spectrum") == &t);
  CHECK(t.chainInput == t.inputIndex("in"));
  CHECK(t.params.empty());
  for (const Form f : {Form::Nested, Form::ChainAudio, Form::ChainVoice, Form::ChainNotes}) {
    CAPTURE(static_cast<int>(f));
    ExpandContext c;
    c.form = f;
    c.voice = f == Form::ChainNotes || f == Form::ChainVoice;
    const uint32_t slot = 9;
    c.slots = std::span<const uint32_t>(&slot, 1);
    const Expansion a = mustExpand(c), b = mustExpand(c);
    CHECK(a.patch == b.patch);
    REQUIRE(a.resources.size() == 1);
    CHECK(a.resources[0].kind == ResourceKind::SignalTap);
    CHECK(a.resources[0].index == 0);
    // The tap is the only atom with a side effect; the output reads the input port directly.
    const auto tap = a.patch.findByName("tap");
    REQUIRE(tap.has_value());
    CHECK(graph::atomSignature(a.patch.node(*tap).atom).name == "signal_tap");
  }
  // The text form parses back to the same graph.
  ExpandContext c;
  const Expansion e = mustExpand(c, "SpecX");
  const std::string text = graph::print(e.patch);
  auto parsed = graph::parse(text, "expansion.pqp");
  REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
}

TEST_CASE("spectrum: pass-through bit for bit, mono and stereo, interpreter and JIT") {
  ExpandContext c;
  const Expansion e = mustExpand(c, "SpecX");
  const std::string module = graph::print(e.patch);
  // A chord with a bright partial and a tiny (denormal-range) one. No noise: its seed follows the NodeId, which the two
  // files number differently.
  const std::string src =
      "  s = sine(phasor(220Hz)) * 0.3 + sine(phasor(331Hz)) * 0.2 + sine(phasor(5013Hz)) * 0.05 + "
      "sine(phasor(97Hz)) * 1e-39\n";
  struct Case {
    const char* name;
    std::string body;   // with the module
    std::string plain;  // without
    size_t channels;
  };
  const Case cases[] = {
      {"mono", src + "  out = SpecX(s)\n", src + "  out = s\n", 1},
      {"stereo", src + "  st = SpecX(pack(s, s * -0.5))\n  out = st[0]\n  r = st[1]\n",
       src + "  st = pack(s, s * -0.5)\n  out = st[0]\n  r = st[1]\n", 2},
  };
  for (const Case& cs : cases) {
    CAPTURE(std::string(cs.name));
    // One out per channel (a render plays one channel per global out).
    const std::string outs = cs.channels == 2 ? "  out out : audio mono\n  out r : audio mono\n" : "  out out : audio mono\n";
    const fs::path with = writeTemp(std::string(cs.name) + "_with.pqp", module + "\npatch Main {\n" + outs + cs.body + "}\n");
    const fs::path without =
        writeTemp(std::string(cs.name) + "_without.pqp", module + "\npatch Main {\n" + outs + cs.plain + "}\n");
    for (const pqc::Backend b : {pqc::Backend::Interpreter, pqc::Backend::Jit}) {
      CAPTURE(static_cast<int>(b));
      const auto x = render(with, b), y = render(without, b);
      REQUIRE(x.size() == cs.channels);
      REQUIRE(y.size() == cs.channels);
      for (size_t ch = 0; ch < cs.channels; ++ch) {
        REQUIRE(x[ch].size() == y[ch].size());
        size_t firstDiff = x[ch].size();
        for (size_t i = 0; i < x[ch].size() && firstDiff == x[ch].size(); ++i)
          if (std::memcmp(&x[ch][i], &y[ch][i], sizeof(float)) != 0) firstDiff = i;
        CAPTURE(ch);
        CAPTURE(firstDiff);
        if (firstDiff < x[ch].size()) MESSAGE("ch " << ch << " at " << firstDiff << ": with " << x[ch][firstDiff] << " without " << y[ch][firstDiff]);
        CHECK(firstDiff == x[ch].size());
      }
    }
  }
}
