// pqc (T1.10): the CLI in-process (pq::pqc::run: arguments, commands, exit codes, outputs), the --notes syntax, and
// the real pqc.exe once end to end (render → WAV → golden comparison).
#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "../golden/golden_audio.h"
#include "pq/engine/wav_file.h"
#include "pq/pqc/pqc.h"

namespace {

namespace fs = std::filesystem;

struct RunResult {
  int code;
  std::string out, err;
};

RunResult pqc(std::vector<std::string> args) {
  std::ostringstream out, err;
  const int code = pq::pqc::run(args, out, err);
  return {code, out.str(), err.str()};
}

std::string goldenPatch(const char* name) { return (golden::goldenDir() / "patches" / name).string(); }

fs::path tempFile(const std::string& name) { return fs::temp_directory_path() / ("pq_test_pqc_" + name); }

fs::path writeTemp(const std::string& name, const std::string& text) {
  const fs::path p = tempFile(name);
  std::ofstream(p, std::ios::binary) << text;
  return p;
}

}  // namespace

TEST_CASE("pqc notes: plain lists are the pqdaw --play arpeggio") {
  auto s = pq::pqc::parseNotes("60 64, 67\t440hz");
  REQUIRE(s);
  CHECK(s->arpeggio);
  REQUIRE(s->hz.size() == 4);
  CHECK(s->hz[0] == doctest::Approx(261.6256f));
  CHECK(s->hz[3] == 440.0f);
  // Unrolled over 1 s at 48 kHz: one note per 12000 frames, 6000 long, pitches cycling.
  const auto notes = pq::pqc::timedNotes(*s, 48000.0, 48000);
  REQUIRE(notes.size() == 4);
  CHECK(notes[1].start == 12000);
  CHECK(notes[1].length == 6000);
  CHECK(notes[3].hz == 440.0f);
  CHECK(notes[0].velocity == doctest::Approx(0.7f));
  CHECK(pq::pqc::timedNotes(*s, 48000.0, 48001).size() == 5);
  CHECK(pq::pqc::timedNotes(*s, 48000.0, 48001)[4].hz == notes[0].hz);
}

TEST_CASE("pqc notes: timed tokens") {
  auto s = pq::pqc::parseNotes("60@0:1 64@500ms:0.25*0.9 67 69.5@1.5s*0.2");
  REQUIRE(s);
  CHECK_FALSE(s->arpeggio);
  REQUIRE(s->hz.size() == 4);
  CHECK(s->start[1] == doctest::Approx(0.5));
  CHECK(s->length[1] == doctest::Approx(0.25));
  CHECK(s->velocity[1] == doctest::Approx(0.9));
  CHECK(s->start[2] == doctest::Approx(0.75));  // previous start + 0.25 s
  CHECK(s->length[2] == doctest::Approx(0.125));
  CHECK(s->hz[3] == doctest::Approx(440.0 * std::exp2(0.5 / 12.0)));
  CHECK(s->velocity[3] == doctest::Approx(0.2));
  const auto notes = pq::pqc::timedNotes(*s, 48000.0, 10);  // timed lists are not cut to the length
  REQUIRE(notes.size() == 4);
  CHECK(notes[0].length == 48000);
  CHECK(notes[1].start == 24000);
  for (const char* bad : {"", "  ", "x", "60@", "60:-1", "60*2", "60@1@2", "60:1@2", "60*0.5:1", "0.1hz", "200",
                          "60@-1", "60:0"}) {
    INFO(std::string(bad));
    CHECK_FALSE(pq::pqc::parseNotes(bad));
  }
}

TEST_CASE("pqc: usage errors exit 2") {
  CHECK(pqc({}).code == pq::pqc::kExitUsage);
  CHECK(pqc({"frobnicate", "x.pqp"}).code == pq::pqc::kExitUsage);
  CHECK(pqc({"check"}).code == pq::pqc::kExitUsage);
  CHECK(pqc({"check", goldenPatch("pluck.pqp"), "--bogus"}).code == pq::pqc::kExitUsage);
  CHECK(pqc({"render", goldenPatch("pluck.pqp")}).code == pq::pqc::kExitUsage);  // no -o
  CHECK(pqc({"render", goldenPatch("pluck.pqp"), "-o", "x.wav", "--backend", "gpu"}).code == pq::pqc::kExitUsage);
  CHECK(pqc({"render", goldenPatch("pluck.pqp"), "-o", "x.wav", "--notes", "60@"}).code == pq::pqc::kExitUsage);
  CHECK(pqc({"render", goldenPatch("pluck.pqp"), "-o", "x.wav", "--seconds", "0"}).code == pq::pqc::kExitUsage);
  CHECK(pqc({"dump-ir", goldenPatch("pluck.pqp"), "--stage", "late"}).code == pq::pqc::kExitUsage);
  CHECK(pqc({"check", goldenPatch("pluck.pqp"), "--patch", "Nope"}).code == pq::pqc::kExitUsage);
  const fs::path out = tempFile("never.wav");
  const auto r = pqc({"render", goldenPatch("pluck.pqp"), "-o", out.string(), "--param", "nope=1"});
  CHECK(r.code == pq::pqc::kExitUsage);
  CHECK(r.err.find("damp") != std::string::npos);  // lists the params there are
  const auto help = pqc({"help"});
  CHECK(help.code == 0);
  CHECK(help.out.find("exit codes") != std::string::npos);
}

TEST_CASE("pqc check: ok patches, every diagnostic of a broken one, missing files") {
  auto ok = pqc({"check", goldenPatch("fm_voice.pqp")});
  CHECK(ok.code == 0);
  CHECK(ok.out.find("ok   FmVoice:") != std::string::npos);
  CHECK(ok.out.find("ok   adsr:") != std::string::npos);  // every patch of the file
  CHECK(ok.err.empty());

  const fs::path syntax = writeTemp("bad_syntax.pqp", "patch A {\n  out y : audio\n  y = 1 +\n  z = foo(\n}\n");
  auto r = pqc({"check", syntax.string()});
  CHECK(r.code == pq::pqc::kExitCompileError);
  CHECK(r.err.find("bad_syntax.pqp:4:5: error:") != std::string::npos);  // file:line:col
  CHECK(r.err.find("    ^") != std::string::npos);                       // caret under the column
  // Every diagnostic of the failing phase, not just the first.
  const fs::path bad = writeTemp("bad_names.pqp", "patch A {\n  out y : audio\n  y = nosuch1(1) + b\n  b = nosuch2(2)\n}\n");
  r = pqc({"check", bad.string()});
  CHECK(r.code == pq::pqc::kExitCompileError);
  CHECK(r.err.find("bad_names.pqp:3:") != std::string::npos);
  CHECK(r.err.find("bad_names.pqp:4:") != std::string::npos);
  fs::remove(syntax);

  const fs::path typeErr = writeTemp("bad_type.pqp", "patch A {\n  out y : audio\n  y = voice.gate\n}\n");
  r = pqc({"check", typeErr.string()});
  CHECK(r.code == pq::pqc::kExitCompileError);
  CHECK_FALSE(r.err.empty());

  const fs::path cycle = writeTemp("cycle.pqp", "patch A {\n  in x : audio\n  out y : audio\n  a = x + a * 0.5\n  y = a\n}\n");
  r = pqc({"check", cycle.string()});
  CHECK(r.code == pq::pqc::kExitCompileError);
  r = pqc({"check", cycle.string(), "--visual"});  // the visual editor's z1 insertion: ok with a warning
  CHECK(r.code == 0);
  CHECK(r.err.find("warning:") != std::string::npos);

  r = pqc({"check", tempFile("does_not_exist.pqp").string()});
  CHECK(r.code == pq::pqc::kExitIo);
  for (const fs::path& p : {bad, typeErr, cycle}) fs::remove(p);
}

TEST_CASE("pqc dump-ir and dump-schedule print the existing printers' text") {
  auto parsed = pqc({"dump-ir", goldenPatch("fm_voice.pqp"), "--stage", "parsed"});
  REQUIRE(parsed.code == 0);
  CHECK(parsed.out.find("patch FmVoice") != std::string::npos);
  CHECK(parsed.out.find("adsr(") != std::string::npos);  // not flattened yet
  auto opt = pqc({"dump-ir", goldenPatch("fm_voice.pqp")});
  REQUIRE(opt.code == 0);
  CHECK(opt.out.find("adsr(") == std::string::npos);  // flattened
  CHECK(opt.out.find("z1(") != std::string::npos);
  auto inferred = pqc({"dump-ir", goldenPatch("fm_voice.pqp"), "--stage", "inferred"});
  REQUIRE(inferred.code == 0);
  CHECK(inferred.out != opt.out);
  // The optimised IR is valid patch-language text.
  CHECK(pq::graph::parse(opt.out, "dump.pqp"));

  auto sched = pqc({"dump-schedule", goldenPatch("drive_zdf.pqp")});
  REQUIRE(sched.code == 0);
  CHECK(sched.out.find("stage") != std::string::npos);
  CHECK(sched.out.find("solve") != std::string::npos);
  auto keys = pqc({"dump-schedule", goldenPatch("drive_zdf.pqp"), "--keys"});
  CHECK(keys.out.size() > sched.out.size());
  // -o writes the same text to a file.
  const fs::path f = tempFile("sched.txt");
  REQUIRE(pqc({"dump-schedule", goldenPatch("drive_zdf.pqp"), "-o", f.string()}).code == 0);
  std::ifstream in(f, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  CHECK(ss.str() == sched.out);
  in.close();
  fs::remove(f);
}

TEST_CASE("pqc render: writes a 32-bit float WAV; options take effect") {
  const fs::path out = tempFile("render.wav");
  auto r = pqc({"render", goldenPatch("stereo_echo.pqp"), "--seconds", "0.25", "-o", out.string()});
  REQUIRE(r.code == 0);
  CHECK(r.out.find("realtime") != std::string::npos);
  auto w = pq::engine::readWav(out);
  REQUIRE(w);
  CHECK(w->numChannels() == 2);
  CHECK(w->frames() == 12000);
  CHECK(w->sampleRate == 48000);
  CHECK(w->isFloat);
  // --rate, --gain, --param.
  REQUIRE(pqc({"render", goldenPatch("stereo_echo.pqp"), "--seconds", "0.25", "--rate", "44100", "--gain", "-6",
               "--param", "fb=0", "-o", out.string()})
              .code == 0);
  auto w2 = pq::engine::readWav(out);
  REQUIRE(w2);
  CHECK(w2->sampleRate == 44100);
  CHECK(w2->frames() == 11025);
  // Notes on a patch without voices is a usage error; a patch with nothing to play is a backend error.
  CHECK(pqc({"render", goldenPatch("stereo_echo.pqp"), "--notes", "60", "-o", out.string()}).code ==
        pq::pqc::kExitUsage);
  const fs::path silent = writeTemp("silent.pqp", "patch S {\n  in x : audio\n  export e = x\n}\n");
  CHECK(pqc({"render", silent.string(), "-o", out.string()}).code == pq::pqc::kExitBackend);
  // Unwritable output.
  CHECK(pqc({"render", goldenPatch("drive_zdf.pqp"), "--seconds", "0.1", "-o",
             (tempFile("no_such_dir") / "x.wav").string()})
            .code == pq::pqc::kExitIo);
  // Voices: the summary reports them.
  r = pqc({"render", goldenPatch("fm_voice.pqp"), "--seconds", "0.5", "--backend", "jit", "-o", out.string()});
  REQUIRE(r.code == 0);
  CHECK(r.out.find("jit") != std::string::npos);
  CHECK(r.out.find("2 note-ons") != std::string::npos);  // the default arpeggio: notes at 0 and 0.25 s
  fs::remove(out);
  fs::remove(silent);
}

#ifdef PQC_EXE
TEST_CASE("pqc.exe renders a golden from the command line") {
  // The real executable (wmain → UTF-8 arguments → run), checked against the committed golden.
  const fs::path out = tempFile("exe_render.wav");
  const std::string cmd = std::string("\"\"") + PQC_EXE + "\" render \"" + goldenPatch("fm_voice.pqp") +
                          "\" --seconds 1 --notes \"60@0:0.4 64@0.1:0.4 67@0.2:0.5*0.9 72@0.45:0.35*0.6 55@0.6:0.3\" "
                          "--backend jit -o \"" + out.string() + "\" > NUL\"";
  REQUIRE(std::system(cmd.c_str()) == 0);
  auto w = pq::engine::readWav(out);
  REQUIRE(w);
  auto gold = pq::engine::readWav(golden::goldenDir() / "audio" / "fm_voice.wav");
  REQUIRE(gold);
  const auto cmp = golden::compare(*gold, w->channels, w->sampleRate);
  INFO(cmp.message);
  CHECK(cmp.ok);
  fs::remove(out);
  // Exit codes reach the shell.
  const std::string bad = std::string("\"\"") + PQC_EXE + "\" check \"" + tempFile("missing.pqp").string() + "\" 2> NUL\"";
  CHECK(std::system(bad.c_str()) == pq::pqc::kExitIo);
}
#endif
