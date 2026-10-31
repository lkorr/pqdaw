// Project file encoding and decoding (T3.5). File format: see serialize.h; the layout written here is the schema
// (version kSchemaVersion). File I/O and the atomic save live in project_io.cpp.

#include "pq/document/serialize.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cmath>
#include <deque>
#include <iterator>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "checks.h"
#include "pq/document/audio_clip.h"
#include "pq/document/library_files.h"
#include "json_io.h"

namespace pq::doc {

namespace {

using json::PathSeg;
using json::Reader;
using json::Writer;

// ---- Names -------------------------------------------------------------------------------------------------------

constexpr std::array<std::string_view, 5> kTrackKindNames = {"midi", "group", "return", "master", "audio"};
constexpr std::array<std::string_view, 12> kParamKindNames = {
    "trackVolume", "trackPan", "trackMute", "trackSolo", "trackSend",
    "tuningReference", "tuningEquave", "mosGenerator", "mosPeriod", "mosStretch", "deviceParam", "wireAmount"};
constexpr std::array<std::string_view, 5> kWireReducerNames = {"sum", "last", "highest", "lowest", "loudest"};
constexpr std::array<std::string_view, 2> kDeviceKindNames = {"instrument", "audioEffect"};
constexpr std::array<std::string_view, 2> kTempoCurveNames = {"step", "linear"};
constexpr std::array<std::string_view, 3> kAnchorNames = {"reference", "note", "jiFundamental"};
constexpr std::array<std::string_view, 2> kWarpNames = {"off", "rePitch"};  // T5.3

// T5.3: a 64-bit content hash as 16 lower-case hex digits (JSON integers stop at int64 in many readers).
std::string hashText(uint64_t h) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s(16, '0');
  for (int i = 15; i >= 0; --i, h >>= 4) s[static_cast<size_t>(i)] = kHex[h & 15];
  return s;
}
std::optional<uint64_t> parseHashText(std::string_view t) {
  if (t.size() != 16) return std::nullopt;
  uint64_t v = 0;
  for (const char c : t) {
    const int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
    if (h < 0) return std::nullopt;
    v = (v << 4) | static_cast<uint64_t>(h);
  }
  return v;
}

template <size_t N>
int indexOf(const std::array<std::string_view, N>& names, std::string_view v) {
  for (size_t i = 0; i < N; ++i)
    if (names[i] == v) return static_cast<int>(i);
  return -1;
}

template <size_t N>
std::string choices(const std::array<std::string_view, N>& names) {
  std::string out;
  for (size_t i = 0; i < N; ++i) {
    if (i) out += i + 1 == N ? " or " : ", ";
    out += '"';
    out += names[i];
    out += '"';
  }
  return out;
}

std::string colourText(Colour c) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string s = "#";
  for (int shift = 20; shift >= 0; shift -= 4) s += kHex[(c.rgb >> shift) & 0xF];
  return s;
}

std::string ratioText(int64_t n, int64_t d) {
  char buf[48];
  auto r = std::to_chars(buf, buf + sizeof buf, n);
  *r.ptr++ = '/';
  r = std::to_chars(r.ptr, buf + sizeof buf, d);
  return std::string(buf, r.ptr);
}

std::string scalaPitchText(const tuning::ScalaPitch& p) {
  if (p.kind == tuning::ScalaPitch::Kind::Ratio) return ratioText(p.ratio.num(), p.ratio.den());
  return p.largeNum + "/" + p.largeDen;
}

// "n/d": two runs of decimal digits; reduced on read. nullopt if malformed, out of int64, or d == 0.
std::optional<tuning::Rational> parseRatioText(std::string_view t) {
  const size_t slash = t.find('/');
  if (slash == std::string_view::npos || slash == 0 || slash + 1 == t.size()) return std::nullopt;
  int64_t n = 0, d = 0;
  for (char c : t.substr(0, slash))
    if (c < '0' || c > '9') return std::nullopt;
  for (char c : t.substr(slash + 1))
    if (c < '0' || c > '9') return std::nullopt;
  if (std::from_chars(t.data(), t.data() + slash, n).ec != std::errc{}) return std::nullopt;
  if (std::from_chars(t.data() + slash + 1, t.data() + t.size(), d).ec != std::errc{}) return std::nullopt;
  return tuning::Rational::tryMake(n, d);
}

// ==== Encoder =====================================================================================================

class Encoder {
public:
  explicit Encoder(std::string& out) : out_(out), w_(out) {}

  void project(const Project& p, const ViewState& view) {
    const PathSeg root;
    w_.beginObject(true);
    w_.key("schemaVersion");
    w_.integer(kSchemaVersion);
    timeline(PathSeg(&root, "timeline"), *p.timeline);
    w_.key("loop");
    w_.beginObject(false);
    w_.key("start");
    w_.integer(p.loop.start);
    w_.key("end");
    w_.integer(p.loop.end);
    w_.key("enabled");
    w_.boolean(p.loop.enabled);
    w_.endObject();

    const PathSeg locs(&root, "locators");
    w_.key("locators");
    w_.beginArray(true);
    for (size_t i = 0; i < p.locators->size(); ++i) {
      const Locator& l = (*p.locators)[i];
      const PathSeg at(&locs, i);
      w_.beginObject(false);
      w_.key("id");
      w_.integer(l.id.value);
      w_.key("tick");
      w_.integer(l.tick);
      w_.key("name");
      w_.string(PathSeg(&at, "name"), l.name);
      w_.endObject();
    }
    w_.endArray();

    const PathSeg tuns(&root, "tunings");
    w_.key("tunings");
    w_.beginArray(true);
    for (size_t i = 0; i < p.tunings->size(); ++i) {
      const TuningSlot& s = *(*p.tunings)[i];
      const PathSeg at(&tuns, i);
      w_.beginObject(true);
      w_.key("id");
      w_.integer(s.id.value);
      w_.key("name");
      w_.string(PathSeg(&at, "name"), s.name);
      w_.key("colour");
      w_.string(at, colourText(s.colour));
      w_.key("def");
      tuningDef(PathSeg(&at, "def"), s.def);
      w_.endObject();
    }
    w_.endArray();

    trackList(PathSeg(&root, "tracks"), "tracks", p.tracks);
    trackList(PathSeg(&root, "returns"), "returns", p.returns);
    w_.key("master");
    track(PathSeg(&root, "master"), *p.master);
    wires(PathSeg(&root, "wires"), wiresOf(p));
    samples(PathSeg(&root, "samples"), samplesOf(p));
    viewState(p, view);
    w_.endObject();
    out_ += "\n";
  }

  // ---- T5.11: user-library files (library_files.h) ----
  void libraryHead(const PathSeg& root, std::string_view format, std::string_view name) {
    w_.beginObject(true);
    w_.key("format");
    w_.string(root, format);
    w_.key("schemaVersion");
    w_.integer(kSchemaVersion);
    w_.key("name");
    w_.string(PathSeg(&root, "name"), name);
  }
  void presetFile(const Device& d, std::string_view name) {
    const PathSeg root;
    libraryHead(root, "pqpreset", name);
    const std::vector<DevicePtr> one{std::shared_ptr<const Device>(std::shared_ptr<const Device>(), &d)};
    const PathSeg devs(&root, "devices");
    w_.key("devices");
    w_.beginArray(true);
    deviceList(devs, one);
    w_.endArray();
    w_.endObject();
    out_ += "\n";
  }
  void userDeviceFile(const UserDeviceFile& f) {
    const PathSeg root;
    libraryHead(root, "pqdevice", f.name);
    w_.key("kind");
    w_.string(root, kDeviceKindNames[static_cast<size_t>(f.kind)]);
    w_.key("patch");
    w_.string(PathSeg(&root, "patch"), f.patch);
    w_.key("origin");
    w_.string(PathSeg(&root, "origin"), f.origin);
    lines(root, "source", f.text, true);
    if (!f.panel.empty()) lines(root, "panel", f.panel, false);
    w_.endObject();
    out_ += "\n";
  }
  void tuningFile(const TuningFile& f) {
    const PathSeg root;
    libraryHead(root, "pqtuning", f.name);
    w_.key("colour");
    w_.string(root, colourText(f.colour));
    w_.key("def");
    tuningDef(PathSeg(&root, "def"), f.def);
    w_.endObject();
    out_ += "\n";
  }
  void clipFile(const ClipFile& f) {
    const PathSeg root;
    libraryHead(root, "pqclip", f.name);
    w_.key("clip");
    const PathSeg at(&root, "clip");
    if (f.clip.audio) audioClip(at, *f.clip.audio);
    else clip(at, *f.clip.clip);
    if (f.sample) samples(PathSeg(&root, "samples"), SampleList{*f.sample});
    w_.endObject();
    out_ += "\n";
  }

private:
  // A text as one string per line ("crlf": true first when every line ends in "\r\n"; `crlfKey` false: the caller's
  // text must not use "\r\n" lines then, they are written with their '\r').
  void lines(const PathSeg& root, const char* key, std::string_view text, bool crlfKey) {
    bool crlf = crlfKey && text.find('\n') != std::string_view::npos;
    for (size_t b = text.find('\n'); crlf && b != std::string_view::npos; b = text.find('\n', b + 1))
      crlf = b > 0 && text[b - 1] == '\r';
    if (crlf) {
      w_.key("crlf");
      w_.boolean(true);
    }
    const PathSeg at(&root, key);
    w_.key(key);
    w_.beginArray(true);
    size_t line = 0;
    for (size_t a = 0;; ++line) {
      const size_t b = text.find('\n', a);
      std::string_view l = text.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a);
      if (crlf && b != std::string_view::npos) l.remove_suffix(1);
      w_.string(PathSeg(&at, line), l);
      if (b == std::string_view::npos) break;
      a = b + 1;
    }
    w_.endArray();
  }

  void timeline(const PathSeg& at, const Timeline& t) {
    w_.key("timeline");
    w_.beginObject(true);
    const PathSeg tempo(&at, "tempo");
    w_.key("tempo");
    w_.beginArray(true);
    for (size_t i = 0; i < t.tempo.size(); ++i) {
      const TempoPoint& tp = t.tempo[i];
      const PathSeg pt(&tempo, i);
      w_.beginObject(false);
      w_.key("tick");
      w_.integer(tp.tick);
      w_.key("bpm");
      w_.number(PathSeg(&pt, "bpm"), tp.bpm);
      w_.key("curve");
      w_.string(pt, kTempoCurveNames[static_cast<size_t>(tp.curve)]);
      w_.endObject();
    }
    w_.endArray();
    w_.key("signatures");
    w_.beginArray(true);
    for (const TimeSigChange& c : t.signatures) {
      w_.beginObject(false);
      w_.key("bar");
      w_.integer(c.bar);
      w_.key("numerator");
      w_.integer(c.sig.numerator);
      w_.key("denominator");
      w_.integer(c.sig.denominator);
      w_.endObject();
    }
    w_.endArray();
    w_.endObject();
  }

  void doubles(const PathSeg& at, const std::vector<double>& v) {
    w_.beginArray(false);
    for (size_t i = 0; i < v.size(); ++i) w_.number(PathSeg(&at, i), v[i]);
    w_.endArray();
  }

  void tuningDef(const PathSeg& at, const TuningDef& d) {
    w_.beginObject(true);
    if (const tuning::Edo* e = d.edo()) {
      w_.key("kind");
      w_.string(at, "edo");
      w_.key("n");
      w_.integer(e->n());
      if (const auto r = e->equaveRatio()) {
        w_.key("equave");
        w_.string(at, ratioText(r->num(), r->den()));
      } else {
        w_.key("equaveCents");
        w_.number(PathSeg(&at, "equaveCents"), e->equaveCents());
      }
    } else if (const tuning::MosParams* m = d.mos()) {
      w_.key("kind");
      w_.string(at, "mos");
      w_.key("generator");
      w_.number(PathSeg(&at, "generator"), m->generator);
      w_.key("n");
      w_.integer(m->n);
      w_.key("period");
      w_.number(PathSeg(&at, "period"), m->period);
      w_.key("periodsPerEquave");
      w_.integer(m->periodsPerEquave);
      w_.key("rotation");
      w_.integer(m->rotation);
      w_.key("modmos");
      doubles(PathSeg(&at, "modmos"), m->modmos);
      w_.key("genSequence");
      doubles(PathSeg(&at, "genSequence"), m->genSequence);
      w_.key("octaveStretch");
      w_.number(PathSeg(&at, "octaveStretch"), m->octaveStretch);
      if (m->region) {
        const PathSeg rg(&at, "region");
        w_.key("region");
        w_.beginObject(false);
        w_.key("perPeriodSize");
        w_.integer(m->region->perPeriodSize);
        w_.key("lowCents");
        w_.number(PathSeg(&rg, "lowCents"), m->region->lowCents);
        w_.key("highCents");
        w_.number(PathSeg(&rg, "highCents"), m->region->highCents);
        w_.endObject();
      }
    } else {
      const ScaleDef& s = *d.scale();
      w_.key("kind");
      w_.string(at, "scale");
      w_.key("description");
      w_.string(PathSeg(&at, "description"), s.scale.description);
      const PathSeg ps(&at, "pitches");
      w_.key("pitches");
      w_.beginArray(false);
      for (size_t i = 0; i < s.scale.pitches.size(); ++i) {
        const tuning::ScalaPitch& p = s.scale.pitches[i];
        if (p.kind == tuning::ScalaPitch::Kind::Cents)
          w_.number(PathSeg(&ps, i), p.cents);
        else
          w_.string(PathSeg(&ps, i), scalaPitchText(p));
      }
      w_.endArray();
      if (s.mapping) {
        const tuning::KeyboardMapping& k = *s.mapping;
        const PathSeg mp(&at, "mapping");
        w_.key("mapping");
        w_.beginObject(true);
        w_.key("mapSize");
        w_.integer(k.mapSize);
        w_.key("firstKey");
        w_.integer(k.firstKey);
        w_.key("lastKey");
        w_.integer(k.lastKey);
        w_.key("middleKey");
        w_.integer(k.middleKey);
        w_.key("referenceKey");
        w_.integer(k.referenceKey);
        w_.key("referenceHz");
        w_.number(PathSeg(&mp, "referenceHz"), k.referenceHz);
        w_.key("formalOctaveDegree");
        w_.integer(k.formalOctaveDegree);
        w_.key("mapping");
        w_.beginArray(false);
        for (int32_t entry : k.mapping) {
          if (entry == tuning::kUnmappedKey)
            w_.null();
          else
            w_.integer(entry);
        }
        w_.endArray();
        w_.endObject();
      }
    }
    w_.key("referenceHz");
    w_.number(PathSeg(&at, "referenceHz"), d.referenceHz);
    w_.endObject();
  }

  void assignment(const PathSeg& at, const TuningAssignment& a) {
    switch (a.mode) {
      case TuningMode::Inherit:
        w_.string(at, "inherit");
        return;
      case TuningMode::Main:
        w_.beginObject(false);
        w_.key("main");
        w_.integer(a.main.value);
        w_.endObject();
        return;
      case TuningMode::Private:
        if (!a.privateDef) json::fail(at, "private tuning without a definition");
        w_.beginObject(true);
        w_.key("private");
        tuningDef(PathSeg(&at, "private"), *a.privateDef);
        w_.endObject();
        return;
    }
  }

  void trackList(const PathSeg& at, std::string_view key, const std::vector<TrackPtr>& list) {
    w_.key(key);
    w_.beginArray(true);
    for (size_t i = 0; i < list.size(); ++i) track(PathSeg(&at, i), *list[i]);
    w_.endArray();
  }

  void track(const PathSeg& at, const Track& t) {
    w_.beginObject(true);
    w_.key("id");
    w_.integer(t.id.value);
    w_.key("kind");
    w_.string(at, kTrackKindNames[static_cast<size_t>(t.kind)]);
    w_.key("name");
    w_.string(PathSeg(&at, "name"), t.name);
    w_.key("colour");
    w_.string(at, colourText(t.colour));
    w_.key("active");
    w_.boolean(t.active);
    w_.key("tuning");
    assignment(PathSeg(&at, "tuning"), t.tuning);

    const PathSeg mx(&at, "mixer");
    const Mixer& m = t.mixer;
    w_.key("mixer");
    w_.beginObject(false);
    w_.key("volumeDb");
    w_.number(PathSeg(&mx, "volumeDb"), m.volumeDb);
    w_.key("pan");
    w_.number(PathSeg(&mx, "pan"), m.pan);
    w_.key("mute");
    w_.boolean(m.mute);
    w_.key("solo");
    w_.boolean(m.solo);
    w_.key("arm");
    w_.boolean(m.arm);
    const PathSeg sends(&mx, "sends");
    w_.key("sends");
    w_.beginArray(false);
    for (size_t i = 0; i < m.sends.size(); ++i) {
      w_.beginObject(false);
      w_.key("target");
      w_.integer(m.sends[i].target.value);
      w_.key("levelDb");
      w_.number(PathSeg(&sends, i), m.sends[i].levelDb);
      w_.key("preFader");
      w_.boolean(m.sends[i].preFader);
      w_.endObject();
    }
    w_.endArray();
    w_.key("output");
    switch (m.output.kind) {
      case OutputKind::Default: w_.string(mx, "default"); break;
      case OutputKind::Master: w_.string(mx, "master"); break;
      case OutputKind::None: w_.string(mx, "none"); break;
      case OutputKind::Track:
        w_.beginObject(false);
        w_.key("track");
        w_.integer(m.output.track.value);
        w_.endObject();
        break;
    }
    w_.endObject();

    const PathSeg lanes(&at, "lanes");
    w_.key("lanes");
    w_.beginArray(true);
    for (size_t i = 0; i < t.lanes.size(); ++i) lane(PathSeg(&lanes, i), *t.lanes[i]);
    w_.endArray();

    const PathSeg devs(&at, "devices");
    w_.key("devices");
    w_.beginArray(true);
    deviceList(devs, t.devices);
    w_.endArray();

    if (t.kind == TrackKind::Midi || t.kind == TrackKind::Audio || !t.clips.empty()) {
      const PathSeg clips(&at, "clips");
      w_.key("clips");
      w_.beginArray(true);
      for (size_t i = 0; i < t.clips.size(); ++i) {
        if (t.clips[i].audio) audioClip(PathSeg(&clips, i), *t.clips[i].audio);  // T5.3
        else clip(PathSeg(&clips, i), *t.clips[i].clip);
      }
      w_.endArray();
    }
    if (t.kind == TrackKind::Group || !t.children.empty()) trackList(PathSeg(&at, "children"), "children", t.children);
    w_.endObject();
  }

  // T5.3: the sample table, one line each (the key only when there are samples).
  void samples(const PathSeg& at, const SampleList& list) {
    if (list.empty()) return;
    w_.key("samples");
    w_.beginArray(true);
    for (size_t i = 0; i < list.size(); ++i) {
      const SampleRef& r = list[i];
      const PathSeg el(&at, i);
      w_.beginObject(false);
      w_.key("id");
      w_.integer(r.id.value);
      w_.key("path");
      w_.string(PathSeg(&el, "path"), r.path);
      w_.key("name");
      w_.string(PathSeg(&el, "name"), r.displayName);
      w_.key("hash");
      w_.string(el, hashText(r.contentHash));
      w_.key("size");
      w_.integer(static_cast<int64_t>(r.size));
      w_.key("frames");
      w_.integer(r.frames);
      w_.key("rate");
      w_.number(PathSeg(&el, "rate"), r.rate);
      w_.key("channels");
      w_.integer(r.channels);
      w_.endObject();
    }
    w_.endArray();
  }

  // T4.7: the modulation wires, one line each (the key only when there are wires); `reducer` only when not "sum".
  void wires(const PathSeg& at, const WireList& list) {
    if (list.empty()) return;
    w_.key("wires");
    w_.beginArray(true);
    for (size_t i = 0; i < list.size(); ++i) {
      const Wire& wr = list[i];
      const PathSeg el(&at, i);
      w_.beginObject(false);
      w_.key("id");
      w_.integer(wr.id.value);
      w_.key("from");
      w_.integer(wr.from.value);
      w_.key("port");
      w_.string(PathSeg(&el, "port"), wr.port);
      w_.key("to");
      w_.integer(wr.to.value);
      w_.key("param");
      w_.string(PathSeg(&el, "param"), wr.param);
      w_.key("amount");
      w_.number(PathSeg(&el, "amount"), wr.amount);
      if (wr.reducer != WireReducer::Sum) {
        w_.key("reducer");
        w_.string(el, kWireReducerNames[static_cast<size_t>(wr.reducer)]);
      }
      w_.endObject();
    }
    w_.endArray();
  }

  // T4.1: one line per device; params / settings as {name: value} (only those set), `active` only when false.
  // T4.4-fix1 (schema 5): a user device's own patch text as "source": one string per line (diffable; the text is
  // the lines joined by "\n"), the device then written over several lines. T4.8 (schema 6): a rack spans several lines
  // too: its "rack" object holds the chains (each with its own "devices"), the mappings and the options that differ
  // from the defaults.
  // M4 review M9: a device list with its racks written iteratively (explicit frames; the path segments of the open
  // levels live in a deque, so their addresses stay put), so racks nest as deep as memory allows. The elements of
  // `list` (the caller writes the array's brackets).
  void deviceList(const PathSeg& listAt, const std::vector<DevicePtr>& list) {
    struct Frame {
      const std::vector<DevicePtr>* list = nullptr;  // a device list (null: a rack, iterating its chains)
      const Rack* rack = nullptr;
      size_t next = 0;
      const PathSeg* at = nullptr;   // the list's path, or the rack's ("…rack")
      const PathSeg* aux = nullptr;  // a rack: its "chains" path
      size_t paths = 0;              // the path segments to drop when the frame ends
    };
    std::deque<PathSeg> paths;
    std::vector<Frame> stack;
    stack.push_back(Frame{&list, nullptr, 0, &listAt, nullptr, 0});
    while (!stack.empty()) {
      Frame& f = stack.back();
      if (f.list != nullptr) {
        if (f.next < f.list->size()) {
          const Device& d = *(*f.list)[f.next];
          const size_t mark = paths.size();
          const PathSeg& el = paths.emplace_back(f.at, f.next++);
          deviceHead(el, d);
          if (!d.rack) {
            w_.endObject();
            paths.resize(mark);
            continue;
          }
          const PathSeg& ra = paths.emplace_back(&el, "rack");
          const PathSeg& ca = paths.emplace_back(&ra, "chains");
          w_.key("rack");
          w_.beginObject(true);
          w_.key("chains");
          w_.beginArray(true);
          stack.push_back(Frame{nullptr, d.rack.get(), 0, &ra, &ca, mark});  // invalidates f
          continue;
        }
        const size_t mark = f.paths;
        const bool chain = stack.size() > 1;
        stack.pop_back();
        if (chain) {  // a chain's devices: close them and the chain
          w_.endArray();
          w_.endObject();
          paths.resize(mark);
        }
        continue;
      }
      const Rack& r = *f.rack;
      if (f.next < r.chains.size()) {
        const size_t mark = paths.size();
        const PathSeg& el = paths.emplace_back(f.aux, f.next);
        chainHead(el, *r.chains[f.next], r.keyUnit);
        const PathSeg& devs = paths.emplace_back(&el, "devices");
        w_.key("devices");
        w_.beginArray(true);
        const std::vector<DevicePtr>* devices = &r.chains[f.next++]->devices;
        stack.push_back(Frame{devices, nullptr, 0, &devs, nullptr, mark});  // invalidates f
        continue;
      }
      w_.endArray();  // the chains
      rackTail(*f.at, r);
      w_.endObject();  // the rack
      w_.endObject();  // its device
      paths.resize(f.paths);
      stack.pop_back();
    }
  }

  void deviceHead(const PathSeg& at, const Device& d) {
    w_.beginObject(d.source != nullptr || d.rack != nullptr);
    w_.key("id");
    w_.integer(d.id.value);
    w_.key("patch");
    w_.string(PathSeg(&at, "patch"), d.patch);
    w_.key("kind");
    w_.string(at, kDeviceKindNames[static_cast<size_t>(d.kind)]);
    if (!d.active) {
      w_.key("active");
      w_.boolean(false);
    }
    auto values = [&](const char* key, const std::vector<DeviceValue>& v) {
      if (v.empty()) return;
      const PathSeg vat(&at, key);
      w_.key(key);
      w_.beginObject(false);
      for (const DeviceValue& x : v) {
        w_.key(x.name);
        w_.number(PathSeg(&vat, x.name), x.value);
      }
      w_.endObject();
    };
    values("params", d.params);
    values("settings", d.settings);
    if (d.source) {
      // Lines end in "\r\n" throughout (a Windows checkout of the stdlib): written without the '\r', "crlf": true.
      const std::string_view text = d.source->text;
      bool crlf = text.find('\n') != std::string_view::npos;
      for (size_t b = text.find('\n'); crlf && b != std::string_view::npos; b = text.find('\n', b + 1))
        crlf = b > 0 && text[b - 1] == '\r';
      if (crlf) {
        w_.key("crlf");
        w_.boolean(true);
      }
      const PathSeg sat(&at, "source");
      w_.key("source");
      w_.beginArray(true);
      size_t line = 0;
      for (size_t a = 0;; ++line) {
        const size_t b = text.find('\n', a);
        std::string_view l = text.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a);
        if (crlf && b != std::string_view::npos) l.remove_suffix(1);
        w_.string(PathSeg(&sat, line), l);
        if (b == std::string_view::npos) break;
        a = b + 1;
      }
      w_.endArray();
    }
  }

  // T4.8.
  void zone(const PathSeg& at, const char* key, const Zone& z) {
    w_.key(key);
    w_.beginArray(false);
    for (const double v : {z.lo, z.fadeLo, z.fadeHi, z.hi}) w_.number(at, v);
    w_.endArray();
  }

  // T4.8: one chain's fields up to its "devices" (the object stays open).
  void chainHead(const PathSeg& el, const Chain& c, KeyZoneUnit keyUnit) {
    w_.beginObject(true);
    w_.key("id");
    w_.integer(c.id.value);
    w_.key("name");
    w_.string(PathSeg(&el, "name"), c.name);
    if (c.volumeDb != 0.0f) {
      w_.key("volumeDb");
      w_.number(PathSeg(&el, "volumeDb"), c.volumeDb);
    }
    if (c.pan != 0.0f) {
      w_.key("pan");
      w_.number(PathSeg(&el, "pan"), c.pan);
    }
    if (!c.active) {
      w_.key("active");
      w_.boolean(false);
    }
    if (c.solo) {
      w_.key("solo");
      w_.boolean(true);
    }
    if (!zoneOff(c.key, ZoneKind::Key, keyUnit)) zone(PathSeg(&el, "key"), "key", c.key);
    if (!zoneOff(c.velocity, ZoneKind::Velocity, keyUnit)) zone(PathSeg(&el, "velocity"), "velocity", c.velocity);
    if (!zoneOff(c.select, ZoneKind::Select, keyUnit)) zone(PathSeg(&el, "select"), "select", c.select);
  }

  // T4.8: a rack's members after "chains" (the object stays open).
  void rackTail(const PathSeg& at, const Rack& r) {
    if (!r.mappings.empty()) {
      const PathSeg mapsAt(&at, "mappings");
      w_.key("mappings");
      w_.beginArray(true);
      for (size_t i = 0; i < r.mappings.size(); ++i) {
        const MacroMapping& m = r.mappings[i];
        const PathSeg el(&mapsAt, i);
        w_.beginObject(false);
        w_.key("id");
        w_.integer(m.id.value);
        w_.key("macro");
        w_.integer(m.macro + 1);
        w_.key("device");
        w_.integer(m.device.value);
        w_.key("param");
        w_.string(PathSeg(&el, "param"), m.param);
        w_.key("min");
        w_.number(PathSeg(&el, "min"), m.min);
        w_.key("max");
        w_.number(PathSeg(&el, "max"), m.max);
        if (m.curve != 0.0) {
          w_.key("curve");
          w_.number(PathSeg(&el, "curve"), m.curve);
        }
        w_.endObject();
      }
      w_.endArray();
    }
    if (std::any_of(r.macroNames.begin(), r.macroNames.end(), [](const std::string& s) { return !s.empty(); })) {
      const PathSeg namesAt(&at, "macroNames");
      w_.key("macroNames");
      w_.beginArray(false);
      for (size_t i = 0; i < kMacroCount; ++i) w_.string(PathSeg(&namesAt, i), r.macroNames[i]);
      w_.endArray();
    }
    if (r.visibleMacros != 8) {
      w_.key("visibleMacros");
      w_.integer(r.visibleMacros);
    }
    if (!r.variations.empty()) {
      const PathSeg varsAt(&at, "variations");
      w_.key("variations");
      w_.beginArray(true);
      for (size_t i = 0; i < r.variations.size(); ++i) {
        const PathSeg el(&varsAt, i);
        w_.beginObject(false);
        w_.key("name");
        w_.string(PathSeg(&el, "name"), r.variations[i].name);
        w_.key("values");
        w_.beginArray(false);
        for (const double v : r.variations[i].values) w_.number(PathSeg(&el, "values"), v);
        w_.endArray();
        w_.endObject();
      }
      w_.endArray();
    }
    if (r.randomizeExclude != 0) {
      w_.key("randomizeExclude");
      w_.integer(r.randomizeExclude);
    }
    if (r.keyUnit != KeyZoneUnit::Hz) {
      w_.key("keyUnit");
      w_.string(at, "degree");
    }
    if (r.voiceChains) {
      w_.key("voiceChains");
      w_.boolean(true);
    }
    if (!r.showChains) {
      w_.key("showChains");
      w_.boolean(false);
    }
    if (!r.showMacros) {
      w_.key("showMacros");
      w_.boolean(false);
    }
  }

  void lane(const PathSeg& at, const AutomationLane& l) {
    w_.beginObject(true);
    w_.key("id");
    w_.integer(l.id.value);
    w_.key("param");
    w_.beginObject(false);
    w_.key("kind");
    w_.string(at, kParamKindNames[static_cast<size_t>(l.param.kind)]);
    w_.key("owner");
    w_.integer(l.param.owner);
    if (l.param.kind == ParamKind::TrackSend || l.param.target != 0) {
      w_.key("target");
      w_.integer(l.param.target);
    }
    w_.endObject();
    const PathSeg pts(&at, "points");
    w_.key("points");
    w_.beginArray(true);
    for (size_t i = 0; i < l.points.size(); ++i) {
      const Breakpoint& b = l.points[i];
      const PathSeg pt(&pts, i);
      w_.beginObject(false);
      w_.key("id");
      w_.integer(b.id.value);
      w_.key("tick");
      w_.integer(b.tick);
      w_.key("value");
      w_.number(PathSeg(&pt, "value"), b.value);
      w_.key("curve");
      w_.number(PathSeg(&pt, "curve"), b.curve);
      w_.endObject();
    }
    w_.endArray();
    w_.endObject();
  }

  void clip(const PathSeg& at, const NoteClip& c) {
    w_.beginObject(true);
    w_.key("id");
    w_.integer(c.id.value);
    w_.key("name");
    w_.string(PathSeg(&at, "name"), c.name);
    w_.key("colour");
    w_.string(at, colourText(c.colour));
    w_.key("start");
    w_.integer(c.start);
    w_.key("length");
    w_.integer(c.length);
    w_.key("contentStart");
    w_.integer(c.contentStart);
    w_.key("loop");
    w_.beginObject(false);
    w_.key("enabled");
    w_.boolean(c.loop.enabled);
    w_.key("start");
    w_.integer(c.loop.start);
    w_.key("end");
    w_.integer(c.loop.end);
    w_.endObject();
    w_.key("signature");
    w_.beginObject(false);
    w_.key("numerator");
    w_.integer(c.signature.numerator);
    w_.key("denominator");
    w_.integer(c.signature.denominator);
    w_.endObject();
    w_.key("tuning");
    assignment(PathSeg(&at, "tuning"), c.tuning);
    w_.key("active");
    w_.boolean(c.active);
    if (!c.fundamental.empty()) {  // T3.6-fix1: written only when set, so older files and goldens are unchanged
      const PathSeg fp(&at, "fundamental");
      w_.key("fundamental");
      w_.beginObject(false);
      if (c.fundamental.lock) {
        w_.key("lock");
        w_.string(fp, ratioText(c.fundamental.lock->num(), c.fundamental.lock->den()));
      }
      if (!c.fundamental.steps.empty()) {
        w_.key("steps");
        w_.beginArray(false);
        for (const HarmonicStep& h : c.fundamental.steps) {
          w_.beginObject(false);
          w_.key("note");
          w_.integer(h.note.value);
          w_.key("denominator");
          w_.integer(h.denominator);
          w_.key("ratio");
          w_.string(fp, ratioText(h.ratio.num(), h.ratio.den()));
          w_.endObject();
        }
        w_.endArray();
      }
      w_.endObject();
    }
    const PathSeg notes(&at, "notes");
    w_.key("notes");
    w_.beginArray(true);
    size_t i = 0;
    for (const Note& n : c.notes) note(PathSeg(&notes, i++), n);
    w_.endArray();
    w_.endObject();
  }

  // T5.3: an audio clip, tagged "kind": "audio", one line; every field written (the derived Off length too, which the
  // reader re-derives from the tempo map).
  void audioClip(const PathSeg& at, const AudioClip& c) {
    w_.beginObject(false);
    w_.key("kind");
    w_.string(at, "audio");
    w_.key("id");
    w_.integer(c.id.value);
    w_.key("name");
    w_.string(PathSeg(&at, "name"), c.name);
    w_.key("colour");
    w_.string(at, colourText(c.colour));
    w_.key("start");
    w_.integer(c.start);
    w_.key("length");
    w_.integer(c.length);
    w_.key("sample");
    w_.integer(c.sample.value);
    w_.key("warp");
    w_.string(at, kWarpNames[static_cast<size_t>(c.warp)]);
    w_.key("contentStartFrame");
    w_.number(PathSeg(&at, "contentStartFrame"), c.contentStartFrame);
    w_.key("seconds");
    w_.number(PathSeg(&at, "seconds"), c.seconds);
    w_.key("contentStart");
    w_.integer(c.contentStart);
    w_.key("loop");
    w_.beginObject(false);
    w_.key("enabled");
    w_.boolean(c.loop.enabled);
    w_.key("start");
    w_.integer(c.loop.start);
    w_.key("end");
    w_.integer(c.loop.end);
    w_.endObject();
    w_.key("sourceBpm");
    w_.number(PathSeg(&at, "sourceBpm"), c.sourceBpm);
    w_.key("gainDb");
    w_.number(PathSeg(&at, "gainDb"), c.gainDb);
    const PathSeg tp(&at, "transpose");
    w_.key("transpose");
    w_.beginObject(false);
    switch (c.transpose.unit) {
      case TransposeUnit::Cents:
        w_.key("cents");
        w_.number(tp, c.transpose.cents);
        break;
      case TransposeUnit::Steps:
        w_.key("steps");
        w_.integer(c.transpose.steps);
        break;
      case TransposeUnit::Ratio:
        w_.key("ratio");
        w_.string(tp, ratioText(c.transpose.ratio.num(), c.transpose.ratio.den()));
        break;
    }
    w_.endObject();
    w_.key("detuneCents");
    w_.number(PathSeg(&at, "detuneCents"), c.detuneCents);
    w_.key("reverse");
    w_.boolean(c.reverse);
    for (const auto& [key, f] : {std::pair<const char*, const AudioFade*>{"fadeIn", &c.fadeIn}, {"fadeOut", &c.fadeOut}}) {
      w_.key(key);
      w_.beginObject(false);
      w_.key("length");
      w_.integer(f->length);
      w_.key("curve");
      w_.number(PathSeg(&at, key), f->curve);
      w_.endObject();
    }
    w_.key("crossfadeIn");
    w_.integer(c.crossfadeIn);
    w_.key("tuning");
    assignment(PathSeg(&at, "tuning"), c.tuning);
    w_.key("active");
    w_.boolean(c.active);
    w_.endObject();
  }

  // One line per note; fields at their default (release 64, chance 1, range 0, enabled) are omitted.
  void note(const PathSeg& at, const Note& n) {
    w_.beginObject(false);
    w_.key("id");
    w_.integer(n.id.value);
    w_.key("start");
    w_.integer(n.start);
    w_.key("length");
    w_.integer(n.length);
    w_.key("pitch");
    w_.beginObject(false);
    const tuning::PitchId& p = n.pitch;
    switch (p.kind()) {
      case tuning::PitchKind::Degree:
        w_.key("degree");
        w_.integer(p.degree());
        w_.key("equave");
        w_.integer(p.equave());
        break;
      case tuning::PitchKind::Ratio:
        w_.key("ratio");
        w_.string(at, ratioText(p.rawNum(), p.rawDen()));
        if (p.anchorKind() != tuning::AnchorKind::Reference) {
          w_.key("anchor");
          w_.string(at, kAnchorNames[static_cast<size_t>(p.anchorKind())]);
        }
        if (p.anchorId() != 0) {
          w_.key("anchorId");
          w_.integer(p.anchorId());
        }
        break;
      case tuning::PitchKind::Cents:
        w_.key("cents");
        w_.number(PathSeg(&at, "pitch"), p.cents());
        break;
      case tuning::PitchKind::Harmonic:
        w_.key("jiObject");
        w_.integer(p.jiObject());
        w_.key("voice");
        w_.integer(p.voiceIndex());
        break;
    }
    w_.endObject();
    w_.key("velocity");
    w_.number(PathSeg(&at, "velocity"), n.velocity);
    if (n.releaseVelocity != 64.0f || std::signbit(n.releaseVelocity)) {
      w_.key("releaseVelocity");
      w_.number(PathSeg(&at, "releaseVelocity"), n.releaseVelocity);
    }
    if (n.chance != 1.0f) {
      w_.key("chance");
      w_.number(PathSeg(&at, "chance"), n.chance);
    }
    if (n.velocityRange != 0.0f || std::signbit(n.velocityRange)) {
      w_.key("velocityRange");
      w_.number(PathSeg(&at, "velocityRange"), n.velocityRange);
    }
    if (n.flags & kNoteDisabled) {
      w_.key("disabled");
      w_.boolean(true);
    }
    if (n.flags & ~static_cast<uint32_t>(kNoteDisabled)) json::fail(at, "unknown note flags");
    w_.endObject();
  }

  void viewState(const Project& p, const ViewState& v) {
    std::vector<uint32_t> trackIds, laneIds;
    forEachTrack(p, [&](const Track& t, const Track*) {
      trackIds.push_back(t.id.value);
      for (const LanePtr& l : t.lanes) laneIds.push_back(l->id.value);
    });
    std::sort(trackIds.begin(), trackIds.end());
    std::sort(laneIds.begin(), laneIds.end());
    auto live = [](const std::vector<uint32_t>& ids, uint32_t id) { return std::binary_search(ids.begin(), ids.end(), id); };

    std::vector<std::pair<uint32_t, TrackView>> tv;
    for (const auto& [id, view] : v.tracks)
      if (live(trackIds, id.value)) tv.emplace_back(id.value, view);
    std::vector<std::pair<uint32_t, LaneView>> lv;
    for (const auto& [id, view] : v.lanes)
      if (live(laneIds, id.value)) lv.emplace_back(id.value, view);
    std::sort(tv.begin(), tv.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::sort(lv.begin(), lv.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    w_.key("view");
    w_.beginObject(true);
    w_.key("tracks");
    w_.beginArray(true);
    for (const auto& [id, view] : tv) {
      w_.beginObject(false);
      w_.key("id");
      w_.integer(id);
      w_.key("folded");
      w_.boolean(view.folded);
      w_.key("height");
      w_.integer(view.height);
      w_.endObject();
    }
    w_.endArray();
    w_.key("lanes");
    w_.beginArray(true);
    for (const auto& [id, view] : lv) {
      w_.beginObject(false);
      w_.key("id");
      w_.integer(id);
      w_.key("visible");
      w_.boolean(view.visible);
      w_.key("height");
      w_.integer(view.height);
      w_.endObject();
    }
    w_.endArray();
    // T4.13: the project patch view's nudges, only when there are any (files without them are unchanged).
    std::vector<uint32_t> entityIds = trackIds;
    forEachTrack(p, [&](const Track& t, const Track*) {
      for (const DevicePtr& d : t.devices) entityIds.push_back(d->id.value);
    });
    std::sort(entityIds.begin(), entityIds.end());
    std::vector<std::pair<uint32_t, PatchNudge>> nv;
    for (const auto& [id, n] : v.patchNudges)
      if (live(entityIds, id) && (n.dx != 0.0f || n.dy != 0.0f)) nv.emplace_back(id, n);
    std::sort(nv.begin(), nv.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (!nv.empty()) {
      w_.key("patch");
      w_.beginArray(true);
      const PathSeg at(nullptr, "patch");
      for (const auto& [id, n] : nv) {
        w_.beginObject(false);
        w_.key("id");
        w_.integer(id);
        w_.key("dx");
        w_.number(at, n.dx);
        w_.key("dy");
        w_.number(at, n.dy);
        w_.endObject();
      }
      w_.endArray();
    }
    w_.endObject();
  }

  std::string& out_;
  Writer w_;
};

// ==== Decoder =====================================================================================================

// Iterates the members of an object whose allowed keys are `names`: unknown and duplicate keys are errors.
class Members {
public:
  Members(Reader& r, const PathSeg& at, std::span<const std::string_view> names) : r_(r), at_(at), names_(names) {
    r_.beginObject(at_);
    start_ = r_.valueStart();
  }
  // Index of the next key in `names`, or −1 at the end of the object. T5.x-perf-audio: the name after the previous
  // match is tried first (files list most keys in `names` order), then all of them.
  int next() {
    std::string_view k;
    if (!r_.nextKey(at_, k)) return -1;
    if (hint_ < names_.size() && names_[hint_] == k) return take(hint_);
    for (size_t i = 0; i < names_.size(); ++i)
      if (names_[i] == k) return take(i);
    const std::string key(k);
    r_.failHere(PathSeg(&at_, key), "unknown key", ErrorCode::InvalidArgument);
  }
  bool has(int i) const noexcept { return (seen_ & (uint64_t{1} << i)) != 0; }
  int take(size_t i) {
    if (seen_ & (uint64_t{1} << i)) r_.failHere(PathSeg(&at_, names_[i]), "duplicate key");
    seen_ |= uint64_t{1} << i;
    hint_ = i + 1;
    return static_cast<int>(i);
  }
  // Errors for the first of `required` that did not appear.
  void require(std::initializer_list<int> required) const {
    for (int i : required)
      if (!has(i))
        json::fail(at_, "missing key \"" + std::string(names_[static_cast<size_t>(i)]) + "\"",
                   ErrorCode::InvalidArgument, start_);
  }
  size_t start() const noexcept { return start_; }

private:
  Reader& r_;
  const PathSeg& at_;
  std::span<const std::string_view> names_;
  uint64_t seen_ = 0;
  size_t start_ = 0;
  size_t hint_ = 0;
};

constexpr uint32_t kMaxId = 0xFFFFFFFEu;  // so IdSource::reserveThrough(maxEntityId) never wraps to 0

class Decoder {
public:
  explicit Decoder(std::string_view text) : r_(text) {}

  LoadedProject project() {
    const PathSeg root;
    static constexpr std::string_view K[] = {"schemaVersion", "timeline", "loop",    "locators", "tunings",
                                             "tracks",        "returns",  "master",  "view",     "wires", "samples"};
    enum { kVer, kTimeline, kLoop, kLocators, kTunings, kTracks, kReturns, kMaster, kView, kWires, kSamples };
    auto np = std::make_shared<Project>();
    LoadedProject out;
    Members m(r_, root, K);
    RawView rawView;
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg at(&root, K[static_cast<size_t>(k)]);
      switch (k) {
        case kVer: {
          const int64_t v = r_.readInt(at);
          if (v != kSchemaVersion) r_.failValue(at, "unexpected schema version " + std::to_string(v));
          break;
        }
        case kTimeline: np->timeline = timeline(at); break;
        case kLoop: np->loop = loop(at); break;
        case kLocators: np->locators = locators(at); break;
        case kTunings: np->tunings = tunings(at); break;
        case kTracks: np->tracks = trackArray(at, TrackList::Main, 1); break;
        case kReturns: np->returns = trackArray(at, TrackList::Returns, 1); break;
        case kMaster: np->master = track(at, TrackList::Master, 1); break;
        case kView: rawView = view(at); break;
        case kWires: np->wires = wireList(at); break;
        case kSamples: np->samples = sampleList(at); break;
      }
    }
    m.require({kVer, kTimeline, kLoop, kLocators, kTunings, kTracks, kReturns, kMaster});
    r_.expectEnd(root);
    // T5.3: warp-Off lengths are derived from the tempo map (whatever the file says).
    for (std::vector<TrackPtr>* list : {&np->tracks, &np->returns})
      for (TrackPtr& t : *list) t = syncedTrack(*np->timeline, t);

    validate(root, *np);
    out.project = np;
    out.view = resolveView(PathSeg(&root, "view"), rawView, *np);
    return out;
  }

  // ---- T5.11: user-library files (library_files.h). Keys 0, 1, 2 are "format", "schemaVersion", "name". ----
  void libraryKey(int k, const PathSeg& f, std::string_view format, std::string& name) {
    if (k == 0) {
      if (str(f) != format) r_.failValue(f, "format must be \"" + std::string(format) + "\"");
    } else if (k == 1) {
      const int64_t v = r_.readInt(f);
      if (v > kSchemaVersion)
        r_.failValue(f, "the file was saved by a newer pqdaw (schema " + std::to_string(v) + "; this build reads up to " +
                            std::to_string(kSchemaVersion) + ")", ErrorCode::Unsupported);
      if (v < 7) r_.failValue(f, "library files start at schema 7");
    } else {
      name = str(f);
    }
  }
  void finishLibrary(const PathSeg& root) {
    r_.expectEnd(root);
    std::sort(ids_.begin(), ids_.end());
    if (std::adjacent_find(ids_.begin(), ids_.end()) != ids_.end())
      json::fail(root, "id " + std::to_string(*std::adjacent_find(ids_.begin(), ids_.end())) + " is used twice");
  }
  PresetFile presetFile() {
    const PathSeg root;
    static constexpr std::string_view K[] = {"format", "schemaVersion", "name", "devices"};
    PresetFile out;
    Members m(r_, root, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&root, K[static_cast<size_t>(k)]);
      if (k < 3) {
        libraryKey(k, f, "pqpreset", out.name);
        continue;
      }
      const size_t pos = r_.valueStart();
      std::vector<DevicePtr> list = devices(f);
      if (list.size() != 1) json::fail(f, "a preset holds exactly one device", ErrorCode::InvalidArgument, pos);
      out.device = std::move(list[0]);
    }
    m.require({0, 1, 2, 3});
    finishLibrary(root);
    // A rack's mappings target devices inside it.
    forEachDeviceIn(*out.device, [&](const Device& d) {
      if (!d.rack) return;
      for (const MacroMapping& mp : d.rack->mappings)
        if (!deviceContains(d, mp.device) || mp.device == d.id)
          json::fail(PathSeg(&root, "devices"), "a mapping of rack " + std::to_string(d.id.value) +
                                                      " targets device " + std::to_string(mp.device.value) +
                                                      ", which is not inside it");
    });
    return out;
  }
  UserDeviceFile userDeviceFile() {
    const PathSeg root;
    static constexpr std::string_view K[] = {"format", "schemaVersion", "name",   "kind", "patch",
                                             "origin", "crlf",          "source", "panel"};
    UserDeviceFile out;
    bool crlf = false;
    Members m(r_, root, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&root, K[static_cast<size_t>(k)]);
      switch (k) {
        case 0:
        case 1:
        case 2: libraryKey(k, f, "pqdevice", out.name); break;
        case 3: out.kind = static_cast<DeviceKind>(name(f, kDeviceKindNames, "device kind")); break;
        case 4:
          out.patch = str(f);
          if (out.patch.empty()) r_.failValue(f, "must not be empty");
          break;
        case 5: out.origin = str(f); break;
        case 6: crlf = r_.readBool(f); break;
        case 7: out.text = joinedLines(f); break;
        case 8: out.panel = joinedLines(f); break;
      }
    }
    m.require({0, 1, 2, 3, 4, 5, 7});
    if (out.text.empty()) json::fail(PathSeg(&root, "source"), "must not be empty");
    if (crlf) {
      std::string t;
      t.reserve(out.text.size() + out.text.size() / 32);
      for (const char ch : out.text) {
        if (ch == '\n') t += '\r';
        t += ch;
      }
      out.text = std::move(t);
    }
    finishLibrary(root);
    return out;
  }
  TuningFile tuningFile() {
    const PathSeg root;
    static constexpr std::string_view K[] = {"format", "schemaVersion", "name", "colour", "def"};
    TuningFile out;
    Members m(r_, root, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&root, K[static_cast<size_t>(k)]);
      if (k < 3) libraryKey(k, f, "pqtuning", out.name);
      if (k == 3) out.colour = colour(f);
      if (k == 4) out.def = tuningDef(f);
    }
    m.require({0, 1, 2, 3, 4});
    finishLibrary(root);
    return out;
  }
  ClipFile clipFile() {
    const PathSeg root;
    static constexpr std::string_view K[] = {"format", "schemaVersion", "name", "clip", "samples"};
    ClipFile out;
    std::shared_ptr<const SampleList> table;
    Members m(r_, root, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&root, K[static_cast<size_t>(k)]);
      if (k < 3) libraryKey(k, f, "pqclip", out.name);
      if (k == 3) out.clip = clip(f);
      if (k == 4) table = sampleList(f);
    }
    m.require({0, 1, 2, 3});
    const PathSeg clipAt(&root, "clip");
    if (out.clip.audio) {
      if (!table || table->size() != 1 || table->front().id != out.clip.audio->sample)
        json::fail(PathSeg(&root, "samples"), "an audio clip carries exactly its own sample");
      out.sample = table->front();
    } else if (table) {
      json::fail(PathSeg(&root, "samples"), "only audio clips carry a sample");
    }
    // The clip file has no main tunings.
    const TuningAssignment& ta = out.clip.audio ? out.clip.audio->tuning : out.clip.clip->tuning;
    if (ta.mode == TuningMode::Main) json::fail(PathSeg(&clipAt, "tuning"), "a clip file has no main tunings");
    finishLibrary(root);
    return out;
  }

private:
  std::string joinedLines(const PathSeg& f) {
    std::string text, line;
    r_.beginArray(f);
    for (size_t j = 0; r_.nextElement(f); ++j) {
      line.clear();
      r_.readString(PathSeg(&f, j), line);
      if (j > 0) text += '\n';
      text += line;
    }
    return text;
  }

  // ---- Scalars ----
  uint32_t defineId(const PathSeg& at) {
    const uint32_t id = ref(at);
    ids_.push_back(id);
    return id;
  }
  uint32_t ref(const PathSeg& at) {
    const int64_t v = r_.readInt(at);
    if (v < 1 || v > kMaxId) r_.failValue(at, "id must be in [1, 4294967294]");
    return static_cast<uint32_t>(v);
  }
  uint32_t u32(const PathSeg& at) {
    const int64_t v = r_.readInt(at);
    if (v < 0 || v > std::numeric_limits<uint32_t>::max()) r_.failValue(at, "must be in [0, 4294967295]");
    return static_cast<uint32_t>(v);
  }
  int32_t i32(const PathSeg& at) {
    const int64_t v = r_.readInt(at);
    if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max())
      r_.failValue(at, "must be a 32-bit integer");
    return static_cast<int32_t>(v);
  }
  Ticks tick(const PathSeg& at) {
    const Ticks t = r_.readInt(at);
    check(at, detail::checkTick(t, "tick"));
    return t;
  }
  Ticks length(const PathSeg& at) {
    const Ticks t = r_.readInt(at);
    check(at, detail::checkLength(t, "length"));
    return t;
  }
  std::string str(const PathSeg& at) {
    std::string s;
    r_.readString(at, s);
    return s;
  }
  Colour colour(const PathSeg& at) {
    std::string s;
    r_.readString(at, s);
    uint32_t v = 0;
    bool ok = s.size() == 7 && s[0] == '#';
    for (size_t i = 1; ok && i < 7; ++i) {
      const char c = s[i];
      const int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                       : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
      if (h < 0) ok = false;
      v = (v << 4) | static_cast<uint32_t>(h < 0 ? 0 : h);
    }
    if (!ok) r_.failValue(at, "colour must be \"#RRGGBB\"");
    return Colour{v};
  }
  template <size_t N>
  int name(const PathSeg& at, const std::array<std::string_view, N>& names, const char* what) {
    std::string s;
    r_.readString(at, s);
    const int i = indexOf(names, s);
    if (i < 0) r_.failValue(at, std::string(what) + " must be " + choices(names));
    return i;
  }
  // Turns a failed model check into an error at `at` (the last value read).
  void check(const PathSeg& at, const Status& s) {
    if (!s) r_.failValue(at, s.error().message);
  }
  void checkAt(const PathSeg& at, size_t pos, const Status& s) {
    if (!s) json::fail(at, s.error().message, ErrorCode::InvalidArgument, pos);
  }

  // ---- Timeline ----
  std::shared_ptr<const Timeline> timeline(const PathSeg& at) {
    static constexpr std::string_view K[] = {"tempo", "signatures"};
    auto t = std::make_shared<Timeline>();
    t->tempo.clear();
    t->signatures.clear();
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg list(&at, K[static_cast<size_t>(k)]);
      r_.beginArray(list);
      const size_t listStart = r_.valueStart();
      for (size_t i = 0; r_.nextElement(list); ++i) {
        const PathSeg el(&list, i);
        if (k == 0) {
          static constexpr std::string_view P[] = {"tick", "bpm", "curve"};
          TempoPoint tp;
          Members pm(r_, el, P);
          for (int q; (q = pm.next()) >= 0;) {
            const PathSeg f(&el, P[static_cast<size_t>(q)]);
            if (q == 0) tp.tick = tick(f);
            if (q == 1) tp.bpm = r_.readDouble(f);
            if (q == 2) tp.curve = static_cast<TempoCurve>(name(f, kTempoCurveNames, "curve"));
          }
          pm.require({0, 1, 2});
          t->tempo.push_back(tp);
        } else {
          static constexpr std::string_view P[] = {"bar", "numerator", "denominator"};
          TimeSigChange c;
          Members pm(r_, el, P);
          for (int q; (q = pm.next()) >= 0;) {
            const PathSeg f(&el, P[static_cast<size_t>(q)]);
            if (q == 0) c.bar = r_.readInt(f);
            if (q == 1) c.sig.numerator = i32(f);
            if (q == 2) c.sig.denominator = i32(f);
          }
          pm.require({0, 1, 2});
          t->signatures.push_back(c);
        }
      }
      if (k == 0) {
        Result<TempoMap> map = TempoMap::create(t->tempo);
        checkAt(list, listStart, map ? Status(Ok{}) : Status(map.error()));
        t->tempoMap = std::move(map).value();
      } else {
        Result<TimeSigMap> map = TimeSigMap::create(t->signatures);
        checkAt(list, listStart, map ? Status(Ok{}) : Status(map.error()));
        t->sigMap = std::move(map).value();
      }
    }
    m.require({0, 1});
    return t;
  }

  LoopRegion loop(const PathSeg& at) {
    static constexpr std::string_view K[] = {"start", "end", "enabled"};
    LoopRegion l;
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      if (k == 0) l.start = tick(f);
      if (k == 1) l.end = tick(f);
      if (k == 2) l.enabled = r_.readBool(f);
    }
    m.require({0, 1, 2});
    if (l.end <= l.start) json::fail(at, "loop end must be after loop start", ErrorCode::InvalidArgument, m.start());
    return l;
  }

  std::shared_ptr<const LocatorList> locators(const PathSeg& at) {
    auto list = std::make_shared<LocatorList>();
    r_.beginArray(at);
    for (size_t i = 0; r_.nextElement(at); ++i) {
      const PathSeg el(&at, i);
      static constexpr std::string_view K[] = {"id", "tick", "name"};
      Locator l;
      Members m(r_, el, K);
      for (int k; (k = m.next()) >= 0;) {
        const PathSeg f(&el, K[static_cast<size_t>(k)]);
        if (k == 0) l.id = LocatorId{defineId(f)};
        if (k == 1) l.tick = tick(f);
        if (k == 2) l.name = str(f);
      }
      m.require({0, 1, 2});
      if (!list->empty() && std::pair(list->back().tick, list->back().id) >= std::pair(l.tick, l.id))
        json::fail(el, "locators must be sorted by (tick, id)", ErrorCode::InvalidArgument, m.start());
      list->push_back(std::move(l));
    }
    return list;
  }

  // ---- Tunings ----
  std::shared_ptr<const TuningList> tunings(const PathSeg& at) {
    auto list = std::make_shared<TuningList>();
    r_.beginArray(at);
    const size_t start = r_.valueStart();
    for (size_t i = 0; r_.nextElement(at); ++i) {
      const PathSeg el(&at, i);
      static constexpr std::string_view K[] = {"id", "name", "colour", "def"};
      TuningSlot s;
      Members m(r_, el, K);
      for (int k; (k = m.next()) >= 0;) {
        const PathSeg f(&el, K[static_cast<size_t>(k)]);
        if (k == 0) s.id = TuningId{defineId(f)};
        if (k == 1) s.name = str(f);
        if (k == 2) s.colour = colour(f);
        if (k == 3) s.def = tuningDef(f);
      }
      m.require({0, 1, 2, 3});
      list->push_back(std::make_shared<const TuningSlot>(std::move(s)));
    }
    if (list->empty()) json::fail(at, "a project needs at least one main tuning", ErrorCode::InvalidArgument, start);
    return list;
  }

  std::vector<double> doubleArray(const PathSeg& at) {
    std::vector<double> v;
    r_.beginArray(at);
    for (size_t i = 0; r_.nextElement(at); ++i) v.push_back(r_.readDouble(PathSeg(&at, i)));
    return v;
  }

  TuningDef tuningDef(const PathSeg& at) {
    static constexpr std::string_view K[] = {"kind",          "referenceHz", "n",           "equave",
                                             "equaveCents",   "generator",   "period",      "periodsPerEquave",
                                             "rotation",      "modmos",      "genSequence", "octaveStretch",
                                             "region",        "description", "pitches",     "mapping"};
    enum {
      kKind, kRef, kN, kEquave, kEquaveCents, kGen, kPeriod, kPpe, kRot, kModmos, kGenSeq, kStretch, kRegion,
      kDesc, kPitches, kMapping
    };
    constexpr std::array<std::string_view, 3> kKinds = {"edo", "mos", "scale"};
    int kind = -1;
    TuningDef d;
    int64_t n = 0;
    std::optional<tuning::Rational> equave;
    double equaveCents = 0.0;
    tuning::MosParams mos;
    ScaleDef scale;
    std::array<size_t, 16> pos{};
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      switch (k) {
        case kKind: kind = name(f, kKinds, "kind"); break;
        case kRef: d.referenceHz = r_.readDouble(f); break;
        case kN: n = r_.readInt(f); break;
        case kEquave: {
          const std::string s = str(f);
          equave = parseRatioText(s);
          if (!equave) r_.failValue(f, "equave must be a ratio \"n/d\" of positive integers");
          break;
        }
        case kEquaveCents: equaveCents = r_.readDouble(f); break;
        case kGen: mos.generator = r_.readDouble(f); break;
        case kPeriod: mos.period = r_.readDouble(f); break;
        case kPpe: mos.periodsPerEquave = i32(f); break;
        case kRot: mos.rotation = i32(f); break;
        case kModmos: mos.modmos = doubleArray(f); break;
        case kGenSeq: mos.genSequence = doubleArray(f); break;
        case kStretch: mos.octaveStretch = r_.readDouble(f); break;
        case kRegion: mos.region = region(f); break;
        case kDesc: scale.scale.description = str(f); break;
        case kPitches: scale.scale.pitches = scalaPitches(f); break;
        case kMapping: scale.mapping = mapping(f); break;
      }
      pos[static_cast<size_t>(k)] = r_.valueStart();
    }
    m.require({kKind, kRef});
    // Keys that belong to another kind.
    static constexpr std::array<uint32_t, 3> kAllowed = {
        (1u << kKind) | (1u << kRef) | (1u << kN) | (1u << kEquave) | (1u << kEquaveCents),
        (1u << kKind) | (1u << kRef) | (1u << kN) | (1u << kGen) | (1u << kPeriod) | (1u << kPpe) | (1u << kRot) |
            (1u << kModmos) | (1u << kGenSeq) | (1u << kStretch) | (1u << kRegion),
        (1u << kKind) | (1u << kRef) | (1u << kDesc) | (1u << kPitches) | (1u << kMapping)};
    for (int k = 0; k < 16; ++k)
      if (m.has(k) && !(kAllowed[static_cast<size_t>(kind)] & (1u << k)))
        json::fail(PathSeg(&at, K[static_cast<size_t>(k)]),
                   "does not apply to a tuning of kind \"" + std::string(kKinds[static_cast<size_t>(kind)]) + "\"",
                   ErrorCode::InvalidArgument, pos[static_cast<size_t>(k)]);
    switch (kind) {
      case 0: {
        m.require({kN});
        const PathSeg nAt(&at, "n");
        if (n < 1 || n > std::numeric_limits<uint32_t>::max())
          json::fail(nAt, "EDO size must be in [1, 4294967295]", ErrorCode::InvalidArgument, pos[kN]);
        if (m.has(kEquave) == m.has(kEquaveCents))
          json::fail(at, "an EDO needs exactly one of \"equave\" and \"equaveCents\"", ErrorCode::InvalidArgument,
                     m.start());
        const int ek = m.has(kEquave) ? kEquave : kEquaveCents;
        Result<tuning::Edo> e = m.has(kEquave) ? tuning::Edo::make(static_cast<uint32_t>(n), *equave)
                                               : tuning::Edo::makeCents(static_cast<uint32_t>(n), equaveCents);
        if (!e) json::fail(PathSeg(&at, K[ek]), e.error().message, ErrorCode::InvalidArgument, pos[static_cast<size_t>(ek)]);
        d.system = std::move(e).value();
        break;
      }
      case 1:
        m.require({kGen, kN, kPeriod, kPpe, kRot, kModmos, kGenSeq, kStretch});
        if (n < std::numeric_limits<int32_t>::min() || n > std::numeric_limits<int32_t>::max())
          json::fail(PathSeg(&at, "n"), "must be a 32-bit integer", ErrorCode::InvalidArgument, pos[kN]);
        mos.n = static_cast<int32_t>(n);
        d.system = std::move(mos);
        break;
      default:
        m.require({kDesc, kPitches});
        d.system = std::move(scale);
        break;
    }
    checkAt(at, m.start(), detail::checkTuningDef(d));
    return d;
  }

  tuning::RegionHint region(const PathSeg& at) {
    static constexpr std::string_view K[] = {"perPeriodSize", "lowCents", "highCents"};
    tuning::RegionHint h;
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      if (k == 0) h.perPeriodSize = i32(f);
      if (k == 1) h.lowCents = r_.readDouble(f);
      if (k == 2) h.highCents = r_.readDouble(f);
    }
    m.require({0, 1, 2});
    return h;
  }

  std::vector<tuning::ScalaPitch> scalaPitches(const PathSeg& at) {
    std::vector<tuning::ScalaPitch> v;
    r_.beginArray(at);
    for (size_t i = 0; r_.nextElement(at); ++i) {
      const PathSeg el(&at, i);
      const Reader::Kind k = r_.peek();
      if (k == Reader::Kind::String) {
        const std::string s = str(el);
        Result<tuning::ScalaPitch> p = tuning::parseScalaRatio(s);
        if (!p) r_.failValue(el, "scale pitch: " + p.error().message);
        v.push_back(std::move(p).value());
      } else if (k == Reader::Kind::Number) {
        v.push_back(tuning::ScalaPitch::fromCents(r_.readDouble(el)));
      } else {
        r_.failHere(el, "expected a ratio string like \"5/4\" or a number of cents", ErrorCode::Type);
      }
    }
    return v;
  }

  tuning::KeyboardMapping mapping(const PathSeg& at) {
    static constexpr std::string_view K[] = {"mapSize",      "firstKey",    "lastKey",            "middleKey",
                                             "referenceKey", "referenceHz", "formalOctaveDegree", "mapping"};
    tuning::KeyboardMapping k;
    Members m(r_, at, K);
    for (int q; (q = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(q)]);
      switch (q) {
        case 0: k.mapSize = i32(f); break;
        case 1: k.firstKey = i32(f); break;
        case 2: k.lastKey = i32(f); break;
        case 3: k.middleKey = i32(f); break;
        case 4: k.referenceKey = i32(f); break;
        case 5: k.referenceHz = r_.readDouble(f); break;
        case 6: k.formalOctaveDegree = i32(f); break;
        case 7:
          r_.beginArray(f);
          for (size_t i = 0; r_.nextElement(f); ++i) {
            const PathSeg el(&f, i);
            if (r_.peek() == Reader::Kind::Null) {
              r_.readNull(el);
              k.mapping.push_back(tuning::kUnmappedKey);
            } else {
              const int32_t e = i32(el);
              if (e == tuning::kUnmappedKey) r_.failValue(el, "degree out of range (use null for an unmapped key)");
              k.mapping.push_back(e);
            }
          }
          break;
      }
    }
    m.require({0, 1, 2, 3, 4, 5, 6, 7});
    checkAt(at, m.start(), tuning::validateKbm(k));
    return k;
  }

  TuningAssignment assignment(const PathSeg& at) {
    if (r_.peek() == Reader::Kind::String) {
      if (str(at) != "inherit") r_.failValue(at, "tuning must be \"inherit\", {\"main\": id} or {\"private\": {...}}");
      return TuningAssignment::inherit();
    }
    static constexpr std::string_view K[] = {"main", "private"};
    TuningAssignment a;
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      if (k == 0) a = TuningAssignment::mainTuning(TuningId{ref(f)});
      if (k == 1) a = TuningAssignment::privateTuning(tuningDef(f));
    }
    if (m.has(0) == m.has(1))
      json::fail(at, "needs exactly one of \"main\" and \"private\"", ErrorCode::InvalidArgument, m.start());
    return a;
  }

  // ---- Tracks ----
  std::vector<TrackPtr> trackArray(const PathSeg& at, TrackList list, uint32_t depth) {
    std::vector<TrackPtr> v;
    r_.beginArray(at);
    for (size_t i = 0; r_.nextElement(at); ++i) v.push_back(track(PathSeg(&at, i), list, depth));
    return v;
  }

  TrackPtr track(const PathSeg& at, TrackList list, uint32_t depth) {
    static constexpr std::string_view K[] = {"id",     "kind",  "name",  "colour", "active",  "tuning",
                                             "mixer", "lanes", "clips", "children", "devices"};
    enum { kId, kKind, kName, kColour, kActive, kTuning, kMixer, kLanes, kClips, kChildren, kDevices };
    auto t = std::make_shared<Track>();
    std::array<size_t, 11> pos{};
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      switch (k) {
        case kId: t->id = TrackId{defineId(f)}; break;
        case kKind: t->kind = static_cast<TrackKind>(name(f, kTrackKindNames, "track kind")); break;
        case kName: t->name = str(f); break;
        case kColour: t->colour = colour(f); break;
        case kActive: t->active = r_.readBool(f); break;
        case kTuning: t->tuning = assignment(f); break;
        case kMixer: t->mixer = mixer(f); break;
        case kLanes: t->lanes = lanes(f); break;
        case kClips: t->clips = clips(f); break;
        case kChildren:
          if (depth + 1 > kMaxTrackDepth)
            r_.failHere(f, "groups nested more than " + std::to_string(kMaxTrackDepth) + " levels deep",
                        ErrorCode::InvalidArgument);
          t->children = trackArray(f, TrackList::Main, depth + 1);
          break;
        case kDevices: t->devices = devices(f); break;
      }
      pos[static_cast<size_t>(k)] = r_.valueStart();
    }
    m.require({kId, kKind, kName, kColour, kActive, kTuning, kMixer, kDevices});
    auto failKey = [&](int k, std::string msg) {
      json::fail(PathSeg(&at, K[static_cast<size_t>(k)]), std::move(msg), ErrorCode::InvalidArgument,
                 pos[static_cast<size_t>(k)]);
    };
    const TrackKind kind = t->kind;
    switch (list) {
      case TrackList::Main:
        if (kind != TrackKind::Midi && kind != TrackKind::Group && kind != TrackKind::Audio)
          failKey(kKind, "must be \"midi\", \"audio\" or \"group\" here");
        break;
      case TrackList::Returns:
        if (kind != TrackKind::Return) failKey(kKind, "must be \"return\" here");
        break;
      case TrackList::Master:
        if (kind != TrackKind::Master) failKey(kKind, "must be \"master\" here");
        break;
    }
    if (m.has(kChildren) && kind != TrackKind::Group) failKey(kChildren, "only groups have child tracks");
    if (m.has(kClips) && kind != TrackKind::Midi && kind != TrackKind::Audio)
      failKey(kClips, "only MIDI and audio tracks hold clips");
    for (const ClipSlot& c : t->clips)  // T5.3
      if ((kind == TrackKind::Audio) != c.isAudio())
        failKey(kClips, kind == TrackKind::Audio ? "an audio track holds audio clips only" : "a MIDI track holds note clips only");
    if (Status s = detail::checkChain(kind, t->devices); !s) failKey(kDevices, s.error().message);
    if (t->tuning.mode != TuningMode::Inherit && kind != TrackKind::Midi && kind != TrackKind::Group &&
        kind != TrackKind::Audio)
      failKey(kTuning, "only MIDI, audio and group tracks have a tuning");
    const Mixer& mx = t->mixer;
    if (mx.arm && kind != TrackKind::Midi) failKey(kMixer, "only MIDI tracks can be armed");
    if (!mx.sends.empty() && !detail::trackHasSends(kind)) failKey(kMixer, "this track has no sends");
    if (kind == TrackKind::Master) {
      if (!t->active) failKey(kActive, "the master track cannot be deactivated");
      if (mx.mute || mx.solo) failKey(kMixer, "the master track has no mute or solo");
      if (mx.output.kind != OutputKind::Default) failKey(kMixer, "the master's output is the device (\"default\")");
    }
    return t;
  }

  // T4.1: the device chain. T4.8: racks hold chains of devices (checked as a whole by checkList when the track is read).
  // M4 review M9: read iteratively — an explicit stack of the open levels (a device list, a device, a rack, a chain) on
  // the heap, their path segments in a deque (stable addresses for the Members readers and the inner levels' paths) —
  // so racks nest as deep as memory allows. Each level's own members are read by deviceKey / rackKey / chainKey.
  static constexpr std::string_view kDeviceKeys[] = {"id",       "patch",  "kind", "active", "params",
                                                     "settings", "source", "crlf", "rack"};
  static constexpr std::string_view kRackKeys[] = {"chains",     "mappings",         "macroNames", "visibleMacros",
                                                   "variations", "randomizeExclude", "keyUnit",    "voiceChains",
                                                   "showChains", "showMacros"};
  static constexpr std::string_view kChainKeys[] = {"id",  "name",     "volumeDb", "pan",    "active",
                                                    "solo", "key", "velocity", "select", "devices"};
  using ChainZones = std::array<std::optional<Zone>, 3>;

  std::vector<DevicePtr> devices(const PathSeg& listAt) {
    enum class Kind : uint8_t { List, Device, Rack, Chain };
    struct Frame {
      Kind kind = Kind::List;
      const PathSeg* at = nullptr;     // List: the array's path; Rack: the "rack" member's path
      PathSeg el;                      // Device / Chain: the element's path
      PathSeg key;                     // the member being read (a child level's parent path)
      size_t index = 0;                // List: next element; Rack: next chain
      std::optional<Members> members;  // Device / Rack / Chain
      std::vector<DevicePtr> list;     // List
      std::shared_ptr<Device> device;  // Device
      bool crlf = false;               // Device
      std::shared_ptr<Rack> rack;      // Rack
      bool inChains = false;           // Rack: reading its "chains" array
      std::vector<std::shared_ptr<Chain>> chains;  // Rack
      std::vector<ChainZones> zones;               // Rack: per chain ("keyUnit" may come after the chains)
      std::shared_ptr<Chain> chain;    // Chain
      ChainZones chainZones;           // Chain
    };
    std::deque<Frame> stack;
    // What the level just closed hands to the one below it.
    std::vector<DevicePtr> doneList;
    bool listDone = false;
    std::shared_ptr<Device> doneDevice;
    std::shared_ptr<Rack> doneRack;
    std::shared_ptr<Chain> doneChain;
    ChainZones doneZones;
    stack.emplace_back();
    stack.back().at = &listAt;
    r_.beginArray(listAt);
    while (!stack.empty()) {
      Frame& f = stack.back();
      switch (f.kind) {
        case Kind::List: {
          if (doneDevice) f.list.push_back(std::move(doneDevice));
          if (!r_.nextElement(*f.at)) {
            doneList = std::move(f.list);
            listDone = true;
            stack.pop_back();
            break;
          }
          const size_t i = f.index++;
          Frame& d = stack.emplace_back();  // `f` stays valid: a deque keeps its elements in place
          d.kind = Kind::Device;
          d.el = PathSeg(f.at, i);
          d.device = std::make_shared<Device>();
          d.members.emplace(r_, d.el, kDeviceKeys);
          break;
        }
        case Kind::Device: {
          if (doneRack) f.device->rack = std::move(doneRack);
          bool descended = false;
          for (int k; !descended && (k = f.members->next()) >= 0;) {
            f.key = PathSeg(&f.el, kDeviceKeys[static_cast<size_t>(k)]);
            if (k == 8) {  // "rack": its members level
              Frame& r = stack.emplace_back();
              r.kind = Kind::Rack;
              r.at = &f.key;
              r.rack = std::make_shared<Rack>();
              r.members.emplace(r_, f.key, kRackKeys);
              descended = true;
            } else {
              deviceKey(k, f.key, *f.device, f.crlf);
            }
          }
          if (descended) break;
          finishDevice(f.el, *f.members, *f.device, f.crlf);
          doneDevice = std::move(f.device);
          stack.pop_back();
          break;
        }
        case Kind::Rack: {
          if (doneChain) {
            f.chains.push_back(std::move(doneChain));
            f.zones.push_back(doneZones);
          }
          if (f.inChains) {
            if (r_.nextElement(f.key)) {
              const size_t i = f.index++;
              Frame& c = stack.emplace_back();
              c.kind = Kind::Chain;
              c.el = PathSeg(&f.key, i);
              c.chain = std::make_shared<Chain>();
              c.members.emplace(r_, c.el, kChainKeys);
              break;
            }
            f.inChains = false;
          }
          bool descended = false;
          for (int k; !descended && (k = f.members->next()) >= 0;) {
            f.key = PathSeg(f.at, kRackKeys[static_cast<size_t>(k)]);
            if (k == 0) {  // "chains": one level per chain
              r_.beginArray(f.key);
              f.inChains = true;
              descended = true;
            } else {
              rackKey(k, f.key, *f.rack);
            }
          }
          if (descended) break;
          f.members->require({0});
          for (size_t i = 0; i < f.chains.size(); ++i) {  // absent zones are off (in the rack's key unit)
            const ZoneKind kinds[3] = {ZoneKind::Key, ZoneKind::Velocity, ZoneKind::Select};
            for (size_t k = 0; k < 3; ++k)
              f.chains[i]->zone(kinds[k]) = f.zones[i][k].value_or(fullZone(kinds[k], f.rack->keyUnit));
            f.rack->chains.push_back(std::move(f.chains[i]));
          }
          doneRack = std::move(f.rack);
          stack.pop_back();
          break;
        }
        case Kind::Chain: {
          if (listDone) {
            f.chain->devices = std::move(doneList);
            listDone = false;
          }
          bool descended = false;
          for (int k; !descended && (k = f.members->next()) >= 0;) {
            f.key = PathSeg(&f.el, kChainKeys[static_cast<size_t>(k)]);
            if (k == 9) {  // "devices": a list level
              Frame& l = stack.emplace_back();
              l.kind = Kind::List;
              l.at = &f.key;
              r_.beginArray(f.key);
              descended = true;
            } else {
              chainKey(k, f.key, *f.chain, f.chainZones);
            }
          }
          if (descended) break;
          f.members->require({0, 1, 9});
          doneChain = std::move(f.chain);
          doneZones = f.chainZones;
          stack.pop_back();
          break;
        }
      }
    }
    return doneList;
  }

  void deviceKey(int k, const PathSeg& f, Device& d, bool& crlf) {
    switch (k) {
      case 0: d.id = DeviceId{defineId(f)}; break;
      case 1:
        d.patch = str(f);
        if (d.patch.empty()) r_.failValue(f, "must not be empty");
        break;
      case 2: d.kind = static_cast<DeviceKind>(name(f, kDeviceKindNames, "device kind")); break;
      case 3: d.active = r_.readBool(f); break;
      case 4: d.params = deviceValues(f); break;
      case 5: d.settings = deviceValues(f); break;
      case 6: {  // T4.4-fix1: a user device's patch text, one string per line
        auto src = std::make_shared<UserPatch>();
        r_.beginArray(f);
        std::string line;
        for (size_t j = 0; r_.nextElement(f); ++j) {
          line.clear();
          r_.readString(PathSeg(&f, j), line);
          if (j > 0) src->text += '\n';
          src->text += line;
        }
        if (src->text.empty()) r_.failValue(f, "must not be empty");
        d.source = std::move(src);
        break;
      }
      case 7: crlf = r_.readBool(f); break;
    }
  }

  void finishDevice(const PathSeg& el, const Members& m, Device& d, bool crlf) {
    m.require({0, 1, 2});
    if (crlf && d.source) {  // the lines were written without their '\r'
      std::string t;
      t.reserve(d.source->text.size() + d.source->text.size() / 32);
      for (const char ch : d.source->text) {
        if (ch == '\n') t += '\r';
        t += ch;
      }
      auto src = std::make_shared<UserPatch>();
      src->text = std::move(t);
      d.source = std::move(src);
    } else if (crlf) {
      r_.failValue(el, "\"crlf\" without a \"source\"");
    }
    if (d.rack && d.source) r_.failValue(el, "a rack has no \"source\"");
  }

  void chainKey(int ck, const PathSeg& cf, Chain& c, ChainZones& z) {
    switch (ck) {
      case 0: c.id = ChainId{defineId(cf)}; break;
      case 1: c.name = str(cf); break;
      case 2: c.volumeDb = static_cast<float>(r_.readDouble(cf)); break;
      case 3: c.pan = static_cast<float>(r_.readDouble(cf)); break;
      case 4: c.active = r_.readBool(cf); break;
      case 5: c.solo = r_.readBool(cf); break;
      case 6: z[0] = zone(cf); break;
      case 7: z[1] = zone(cf); break;
      case 8: z[2] = zone(cf); break;
    }
  }

  Zone zone(const PathSeg& at) {
    double v[4] = {};
    size_t n = 0;
    r_.beginArray(at);
    for (size_t i = 0; r_.nextElement(at); ++i) {
      const PathSeg el(&at, i);
      if (n == 4) r_.failHere(el, "a zone is [lo, fadeLo, fadeHi, hi]", ErrorCode::InvalidArgument);
      v[n++] = r_.readDouble(el);
    }
    if (n != 4) r_.failHere(at, "a zone is [lo, fadeLo, fadeHi, hi]", ErrorCode::InvalidArgument);
    return Zone{v[0], v[1], v[2], v[3]};
  }

  // A rack's members other than "chains".
  void rackKey(int k, const PathSeg& f, Rack& r) {
    switch (k) {
      case 1: {
        r_.beginArray(f);
        for (size_t i = 0; r_.nextElement(f); ++i) {
          const PathSeg el(&f, i);
          static constexpr std::string_view MK[] = {"id", "macro", "device", "param", "min", "max", "curve"};
          MacroMapping mp;
          Members mm(r_, el, MK);
          for (int mk; (mk = mm.next()) >= 0;) {
            const PathSeg mf(&el, MK[static_cast<size_t>(mk)]);
            switch (mk) {
              case 0: mp.id = MappingId{defineId(mf)}; break;
              case 1: {
                const int64_t v = r_.readInt(mf);
                if (v < 1 || v > static_cast<int64_t>(kMacroCount)) r_.failValue(mf, "macros are 1 … 16");
                mp.macro = static_cast<uint8_t>(v - 1);
                break;
              }
              case 2: mp.device = DeviceId{ref(mf)}; break;
              case 3: mp.param = str(mf); break;
              case 4: mp.min = r_.readDouble(mf); break;
              case 5: mp.max = r_.readDouble(mf); break;
              case 6: mp.curve = r_.readDouble(mf); break;
            }
          }
          mm.require({0, 1, 2, 3, 4, 5});
          check(el, detail::checkMapping(mp));
          r.mappings.push_back(std::move(mp));
        }
        break;
      }
      case 2: {
        size_t n = 0;
        r_.beginArray(f);
        for (size_t i = 0; r_.nextElement(f); ++i) {
          const PathSeg el(&f, i);
          if (n == kMacroCount) r_.failHere(el, "a rack has 16 macros", ErrorCode::InvalidArgument);
          r.macroNames[n++] = str(el);
        }
        break;
      }
      case 3: {
        const int64_t v = r_.readInt(f);
        if (v < 1 || v > static_cast<int64_t>(kMacroCount)) r_.failValue(f, "a rack shows 1 … 16 macros");
        r.visibleMacros = static_cast<uint8_t>(v);
        break;
      }
      case 4: {
        r_.beginArray(f);
        for (size_t i = 0; r_.nextElement(f); ++i) {
          const PathSeg el(&f, i);
          static constexpr std::string_view VK[] = {"name", "values"};
          MacroVariation var;
          Members vm(r_, el, VK);
          for (int vk; (vk = vm.next()) >= 0;) {
            const PathSeg vf(&el, VK[static_cast<size_t>(vk)]);
            if (vk == 0) var.name = str(vf);
            if (vk == 1) {
              size_t n = 0;
              r_.beginArray(vf);
              for (size_t j = 0; r_.nextElement(vf); ++j) {
                const PathSeg xe(&vf, j);
                if (n == kMacroCount) r_.failHere(xe, "a variation holds 16 values", ErrorCode::InvalidArgument);
                var.values[n++] = r_.readDouble(xe);
              }
              if (n != kMacroCount) r_.failHere(vf, "a variation holds 16 values", ErrorCode::InvalidArgument);
            }
          }
          vm.require({0, 1});
          r.variations.push_back(std::move(var));
        }
        break;
      }
      case 5: {
        const int64_t v = r_.readInt(f);
        if (v < 0 || v > 0xFFFF) r_.failValue(f, "a 16-bit mask");
        r.randomizeExclude = static_cast<uint16_t>(v);
        break;
      }
      case 6: {
        static constexpr std::array<std::string_view, 2> kUnits = {"hz", "degree"};
        r.keyUnit = static_cast<KeyZoneUnit>(name(f, kUnits, "key zone unit"));
        break;
      }
      case 7: r.voiceChains = r_.readBool(f); break;
      case 8: r.showChains = r_.readBool(f); break;
      case 9: r.showMacros = r_.readBool(f); break;
    }
  }

  // T5.3: the sample table, sorted by id in the file.
  std::shared_ptr<const SampleList> sampleList(const PathSeg& at) {
    SampleList v;
    r_.beginArray(at);
    for (size_t i = 0; r_.nextElement(at); ++i) {
      const PathSeg el(&at, i);
      static constexpr std::string_view K[] = {"id", "path", "name", "hash", "size", "frames", "rate", "channels"};
      SampleRef s;
      Members m(r_, el, K);
      for (int k; (k = m.next()) >= 0;) {
        const PathSeg f(&el, K[static_cast<size_t>(k)]);
        switch (k) {
          case 0: s.id = SampleId{defineId(f)}; break;
          case 1: s.path = str(f); break;
          case 2: s.displayName = str(f); break;
          case 3: {
            const std::optional<uint64_t> h = parseHashText(str(f));
            if (!h) r_.failValue(f, "hash must be 16 lower-case hex digits");
            s.contentHash = *h;
            break;
          }
          case 4: {
            const int64_t bytes = r_.readInt(f);
            if (bytes < 0) r_.failValue(f, "size must be >= 0");
            s.size = static_cast<uint64_t>(bytes);
            break;
          }
          case 5: s.frames = r_.readInt(f); break;
          case 6: s.rate = r_.readDouble(f); break;
          case 7: s.channels = u32(f); break;
        }
      }
      m.require({0, 1, 2, 3, 4, 5, 6, 7});
      check(el, detail::checkSampleRef(s));
      if (!v.empty() && !(v.back().id < s.id)) r_.failValue(PathSeg(&el, "id"), "samples must be sorted by id");
      v.push_back(std::move(s));
    }
    return v.empty() ? nullptr : std::make_shared<const SampleList>(std::move(v));
  }

  // T4.7: the wires, sorted by id in the file (ends are checked against the devices in validate()).
  std::shared_ptr<const WireList> wireList(const PathSeg& at) {
    WireList v;
    r_.beginArray(at);
    for (size_t i = 0; r_.nextElement(at); ++i) {
      const PathSeg el(&at, i);
      static constexpr std::string_view K[] = {"id", "from", "port", "to", "param", "amount", "reducer"};
      Wire w;
      Members m(r_, el, K);
      for (int k; (k = m.next()) >= 0;) {
        const PathSeg f(&el, K[static_cast<size_t>(k)]);
        switch (k) {
          case 0: w.id = WireId{defineId(f)}; break;
          case 1: w.from = DeviceId{ref(f)}; break;
          case 2: w.port = str(f); break;
          case 3: w.to = DeviceId{ref(f)}; break;
          case 4: w.param = str(f); break;
          case 5: w.amount = r_.readDouble(f); break;
          case 6: w.reducer = static_cast<WireReducer>(name(f, kWireReducerNames, "wire reducer")); break;
        }
      }
      m.require({0, 1, 2, 3, 4, 5});
      check(el, detail::checkWire(w));
      if (!v.empty() && !(v.back().id < w.id)) r_.failValue(PathSeg(&el, "id"), "wires must be sorted by id");
      v.push_back(std::move(w));
    }
    return v.empty() ? nullptr : std::make_shared<const WireList>(std::move(v));
  }

  // {name: number, …}: any key order in the file; sorted in the model.
  std::vector<DeviceValue> deviceValues(const PathSeg& at) {
    std::vector<DeviceValue> v;
    r_.beginObject(at);
    std::string_view key;
    while (r_.nextKey(at, key)) {
      DeviceValue x;
      x.name = std::string(key);
      const PathSeg f(&at, x.name);
      x.value = r_.readDouble(f);
      check(f, detail::checkDeviceValue(x.name, x.value));
      v.push_back(std::move(x));
    }
    std::sort(v.begin(), v.end(), [](const DeviceValue& a, const DeviceValue& b) { return a.name < b.name; });
    for (size_t i = 1; i < v.size(); ++i)
      if (v[i - 1].name == v[i].name) r_.failHere(PathSeg(&at, v[i].name), "duplicate key");
    return v;
  }

  Mixer mixer(const PathSeg& at) {
    static constexpr std::string_view K[] = {"volumeDb", "pan", "mute", "solo", "arm", "sends", "output"};
    Mixer mx;
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      switch (k) {
        case 0:
          mx.volumeDb = r_.readFloat(f);
          check(f, detail::checkVolume(mx.volumeDb));
          break;
        case 1:
          mx.pan = r_.readFloat(f);
          check(f, detail::checkParamValue(ParamKind::TrackPan, mx.pan));
          break;
        case 2: mx.mute = r_.readBool(f); break;
        case 3: mx.solo = r_.readBool(f); break;
        case 4: mx.arm = r_.readBool(f); break;
        case 5: mx.sends = sends(f); break;
        case 6: mx.output = output(f); break;
      }
    }
    m.require({0, 1, 2, 3, 4, 5, 6});
    return mx;
  }

  std::vector<Send> sends(const PathSeg& at) {
    std::vector<Send> v;
    r_.beginArray(at);
    for (size_t i = 0; r_.nextElement(at); ++i) {
      const PathSeg el(&at, i);
      static constexpr std::string_view K[] = {"target", "levelDb", "preFader"};
      Send s;
      Members m(r_, el, K);
      for (int k; (k = m.next()) >= 0;) {
        const PathSeg f(&el, K[static_cast<size_t>(k)]);
        if (k == 0) s.target = TrackId{ref(f)};
        if (k == 1) {
          s.levelDb = r_.readFloat(f);
          check(f, detail::checkVolume(s.levelDb));
        }
        if (k == 2) s.preFader = r_.readBool(f);
      }
      m.require({0, 1, 2});
      if (s.levelDb == kMinVolumeDb && !s.preFader)
        json::fail(el, "a send at the default (-70 dB, post-fader) is implicit and must not be stored",
                   ErrorCode::InvalidArgument, m.start());
      if (!v.empty() && v.back().target >= s.target)
        json::fail(el, "sends must be sorted by target id, one per target", ErrorCode::InvalidArgument, m.start());
      v.push_back(s);
    }
    return v;
  }

  Output output(const PathSeg& at) {
    if (r_.peek() == Reader::Kind::String) {
      const std::string s = str(at);
      if (s == "default") return Output{OutputKind::Default, {}};
      if (s == "master") return Output{OutputKind::Master, {}};
      if (s == "none") return Output{OutputKind::None, {}};
      r_.failValue(at, "output must be \"default\", \"master\", \"none\" or {\"track\": id}");
    }
    static constexpr std::string_view K[] = {"track"};
    Output o{OutputKind::Track, {}};
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) o.track = TrackId{ref(PathSeg(&at, "track"))};
    m.require({0});
    return o;
  }

  std::vector<LanePtr> lanes(const PathSeg& at) {
    std::vector<LanePtr> v;
    r_.beginArray(at);
    for (size_t i = 0; r_.nextElement(at); ++i) {
      const PathSeg el(&at, i);
      static constexpr std::string_view K[] = {"id", "param", "points"};
      auto l = std::make_shared<AutomationLane>();
      Members m(r_, el, K);
      for (int k; (k = m.next()) >= 0;) {
        const PathSeg f(&el, K[static_cast<size_t>(k)]);
        if (k == 0) l->id = LaneId{defineId(f)};
        if (k == 1) l->param = param(f);
        if (k == 2) l->points = points(f, l->param, m.has(1));
      }
      m.require({0, 1, 2});
      v.push_back(std::move(l));
    }
    return v;
  }

  ParamRef param(const PathSeg& at) {
    static constexpr std::string_view K[] = {"kind", "owner", "target"};
    ParamRef r;
    Members m(r_, at, K);
    size_t targetPos = 0;
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      if (k == 0) r.kind = static_cast<ParamKind>(name(f, kParamKindNames, "parameter kind"));
      if (k == 1) r.owner = ref(f);
      if (k == 2) {
        r.target = ref(f);
        targetPos = r_.valueStart();
      }
    }
    m.require({0, 1});
    if (r.kind == ParamKind::TrackSend) m.require({2});
    else if (m.has(2))
      json::fail(PathSeg(&at, "target"), "only sends have a target", ErrorCode::InvalidArgument, targetPos);
    return r;
  }

  // Breakpoints are validated against the lane's parameter range, so "param" must come before "points".
  std::vector<Breakpoint> points(const PathSeg& at, const ParamRef& param, bool haveParam) {
    if (!haveParam) r_.failHere(at, "\"param\" must come before \"points\"", ErrorCode::InvalidArgument);
    const ParamRange range = paramRange(param.kind);
    std::vector<Breakpoint> v;
    r_.beginArray(at);
    for (size_t i = 0; r_.nextElement(at); ++i) {
      const PathSeg el(&at, i);
      static constexpr std::string_view K[] = {"id", "tick", "value", "curve"};
      Breakpoint b;
      Members m(r_, el, K);
      for (int k; (k = m.next()) >= 0;) {
        const PathSeg f(&el, K[static_cast<size_t>(k)]);
        if (k == 0) b.id = BreakpointId{defineId(f)};
        if (k == 1) b.tick = tick(f);
        if (k == 2) {
          b.value = r_.readFloat(f);
          if (!detail::finiteIn(b.value, range.min, range.max)) r_.failValue(f, "value out of the parameter's range");
        }
        if (k == 3) {
          b.curve = r_.readFloat(f);
          if (!detail::finiteIn(b.curve, -1.0, 1.0)) r_.failValue(f, "curve must be in [-1, 1]");
        }
      }
      m.require({0, 1, 2, 3});
      if (!v.empty() && !detail::pointLess(v.back(), b))
        json::fail(el, "breakpoints must be sorted by (tick, id)", ErrorCode::InvalidArgument, m.start());
      v.push_back(b);
    }
    return v;
  }

  // ---- Clips and notes ----
  std::vector<ClipSlot> clips(const PathSeg& at) {
    std::vector<ClipSlot> v;
    r_.beginArray(at);
    for (size_t i = 0; r_.nextElement(at); ++i) {
      const PathSeg el(&at, i);
      const size_t start = r_.pos();
      ClipSlot c = clip(el);
      if (!v.empty() && std::pair(v.back().start(), v.back().id) >= std::pair(c.start(), c.id))
        json::fail(el, "clips must be sorted by (start, id)", ErrorCode::InvalidArgument, start);
      v.push_back(std::move(c));
    }
    return v;
  }

  // A note clip, or (T5.3) an audio clip tagged "kind": "audio": one key list for both (a pull reader meets the tag
  // wherever it is), each kind requiring its own keys and refusing the other's.
  ClipSlot clip(const PathSeg& at) {
    static constexpr std::string_view K[] = {
        "id",     "name",    "colour", "start",  "length",  "contentStart", "loop",   "signature", "tuning",
        "active", "notes",   "fundamental",
        "kind",   "sample",  "warp",   "contentStartFrame", "seconds", "sourceBpm", "gainDb", "transpose",
        "detuneCents", "reverse", "fadeIn", "fadeOut", "crossfadeIn"};
    enum { kKind = 12, kSample, kWarp, kFrame, kSeconds, kBpm, kGain, kTranspose, kDetune, kReverse, kFadeIn, kFadeOut, kXfade };
    NoteClip note;  // the fields both kinds have are read into it (T5.x-perf-audio: on the stack, not the heap)
    NoteClip* c = &note;
    AudioClip a;
    bool audio = false;
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      switch (k) {
        case kKind: {
          const std::string kind = str(f);
          if (kind != "audio") r_.failValue(f, "clip kind must be \"audio\" (note clips have no kind)");
          audio = true;
          break;
        }
        case kSample: a.sample = SampleId{ref(f)}; break;
        case kWarp: a.warp = static_cast<AudioWarp>(name(f, kWarpNames, "warp mode")); break;
        case kFrame: a.contentStartFrame = r_.readDouble(f); break;
        case kSeconds: a.seconds = r_.readDouble(f); break;
        case kBpm: a.sourceBpm = r_.readDouble(f); break;
        case kGain: a.gainDb = r_.readFloat(f); break;
        case kTranspose: a.transpose = transpose(f); break;
        case kDetune: a.detuneCents = r_.readFloat(f); break;
        case kReverse: a.reverse = r_.readBool(f); break;
        case kFadeIn: a.fadeIn = fade(f); break;
        case kFadeOut: a.fadeOut = fade(f); break;
        case kXfade: a.crossfadeIn = r_.readInt(f); break;
        case 0: c->id = ClipId{defineId(f)}; break;
        case 1: c->name = str(f); break;
        case 2: c->colour = colour(f); break;
        case 3: c->start = tick(f); break;
        case 4: c->length = length(f); break;
        case 5: c->contentStart = tick(f); break;
        case 6: {
          static constexpr std::string_view L[] = {"enabled", "start", "end"};
          Members lm(r_, f, L);
          for (int q; (q = lm.next()) >= 0;) {
            const PathSeg g(&f, L[static_cast<size_t>(q)]);
            if (q == 0) c->loop.enabled = r_.readBool(g);
            if (q == 1) c->loop.start = tick(g);
            if (q == 2) c->loop.end = tick(g);
          }
          lm.require({0, 1, 2});
          checkAt(f, lm.start(), detail::checkLoop(c->loop));
          break;
        }
        case 7: {
          static constexpr std::string_view S[] = {"numerator", "denominator"};
          Members sm(r_, f, S);
          for (int q; (q = sm.next()) >= 0;) {
            const PathSeg g(&f, S[static_cast<size_t>(q)]);
            if (q == 0) c->signature.numerator = i32(g);
            if (q == 1) c->signature.denominator = i32(g);
          }
          sm.require({0, 1});
          checkAt(f, sm.start(), detail::checkSignature(c->signature));
          break;
        }
        case 8: c->tuning = assignment(f); break;
        case 9: c->active = r_.readBool(f); break;
        case 10: c->notes = notes(f); break;
        case 11: {
          const size_t fstart = r_.pos();
          c->fundamental = fundamental(f);
          checkAt(f, fstart, detail::checkFundamental(c->fundamental));
          break;
        }
      }
    }
    if (!audio) {
      for (int k = kSample; k <= kXfade; ++k)
        if (m.has(k)) json::fail(PathSeg(&at, K[static_cast<size_t>(k)]), "only audio clips have this key");
      m.require({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
      const ClipId id = note.id;
      return ClipSlot{id, std::make_shared<const NoteClip>(std::move(note)), nullptr};
    }
    for (int k : {7, 10, 11})
      if (m.has(k)) json::fail(PathSeg(&at, K[static_cast<size_t>(k)]), "only note clips have this key");
    m.require({0, 1, 2, 3, 4, 5, 6, 8, 9, kKind, kSample, kWarp, kFrame, kSeconds, kBpm, kGain, kTranspose, kDetune,
               kReverse, kFadeIn, kFadeOut, kXfade});
    a.id = c->id;
    a.name = std::move(c->name);
    a.colour = c->colour;
    a.start = c->start;
    a.length = c->length;
    a.contentStart = c->contentStart;
    a.loop = c->loop;
    a.tuning = c->tuning;
    a.active = c->active;
    return ClipSlot{a.id, nullptr, std::make_shared<const AudioClip>(std::move(a))};
  }

  // T5.3: {"cents": x} | {"steps": n} | {"ratio": "n/d"} (exactly one).
  AudioTranspose transpose(const PathSeg& at) {
    static constexpr std::string_view K[] = {"cents", "steps", "ratio"};
    AudioTranspose t;
    int count = 0;
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      ++count;
      if (k == 0) {
        t.unit = TransposeUnit::Cents;
        t.cents = r_.readDouble(f);
      } else if (k == 1) {
        t.unit = TransposeUnit::Steps;
        t.steps = i32(f);
      } else {
        std::string s;
        r_.readString(f, s);
        const std::optional<tuning::Rational> q = parseRatioText(s);
        if (!q || !q->isPositive()) r_.failValue(f, "ratio must be a positive \"n/d\"");
        t.unit = TransposeUnit::Ratio;
        t.ratio = *q;
      }
    }
    if (count != 1) json::fail(at, "a transpose has exactly one of \"cents\", \"steps\", \"ratio\"");
    return t;
  }

  // T5.3: {"length": ticks, "curve": number}.
  AudioFade fade(const PathSeg& at) {
    static constexpr std::string_view K[] = {"length", "curve"};
    AudioFade fd;
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      if (k == 0) fd.length = r_.readInt(f);
      else fd.curve = r_.readFloat(f);
    }
    m.require({0, 1});
    return fd;
  }

  // T3.6-fix1: {"lock": "n/d", "steps": [{"note": id, "denominator": D, "ratio": "n/d"}]}, both optional.
  ClipFundamental fundamental(const PathSeg& at) {
    static constexpr std::string_view K[] = {"lock", "steps"};
    ClipFundamental out;
    auto ratioOf = [&](const PathSeg& f) {
      std::string s;
      r_.readString(f, s);
      const auto r = parseRatioText(s);
      if (!r || !r->isPositive()) r_.failValue(f, "ratio must be \"n/d\" with positive integers n, d");
      return r.value_or(tuning::Rational(1));
    };
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      if (k == 0) out.lock = ratioOf(f);
      if (k == 1) {
        r_.beginArray(f);
        for (size_t i = 0; r_.nextElement(f); ++i) {
          const PathSeg el(&f, i);
          static constexpr std::string_view S[] = {"note", "denominator", "ratio"};
          HarmonicStep h;
          Members sm(r_, el, S);
          for (int q; (q = sm.next()) >= 0;) {
            const PathSeg g(&el, S[static_cast<size_t>(q)]);
            if (q == 0) h.note = NoteId{u32(g)};
            if (q == 1) h.denominator = r_.readInt(g);
            if (q == 2) h.ratio = ratioOf(g);
          }
          sm.require({0, 1, 2});
          out.steps.push_back(h);
        }
      }
    }
    return out;
  }

  NoteSeq notes(const PathSeg& at) {
    std::vector<Note> v;
    r_.beginArray(at);
    const size_t start = r_.valueStart();
    for (size_t i = 0; r_.nextElement(at); ++i) {
      const PathSeg el(&at, i);
      const size_t noteStart = r_.pos();
      const Note n = note(el);
      if (Status s = validateNote(n); !s) json::fail(el, s.error().message, ErrorCode::InvalidArgument, noteStart);
      ids_.push_back(n.id.value);
      v.push_back(n);
    }
    Result<NoteSeq> seq = NoteSeq::fromNotes(std::move(v));
    if (!seq) json::fail(at, seq.error().message, ErrorCode::InvalidArgument, start);
    return std::move(seq).value();
  }

  Note note(const PathSeg& at) {
    static constexpr std::string_view K[] = {"id",       "start",           "length", "pitch",        "velocity",
                                             "releaseVelocity", "chance", "velocityRange", "disabled"};
    Note n;
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      switch (k) {
        case 0: n.id = NoteId{ref(f)}; break;  // collected by notes() after validation
        case 1: n.start = r_.readInt(f); break;
        case 2: n.length = r_.readInt(f); break;
        case 3: n.pitch = pitch(f); break;
        case 4: n.velocity = r_.readFloat(f); break;
        case 5: n.releaseVelocity = r_.readFloat(f); break;
        case 6: n.chance = r_.readFloat(f); break;
        case 7: n.velocityRange = r_.readFloat(f); break;
        case 8:
          if (r_.readBool(f)) n.flags |= kNoteDisabled;
          break;
      }
    }
    m.require({0, 1, 2, 3, 4});
    return n;
  }

  tuning::PitchId pitch(const PathSeg& at) {
    static constexpr std::string_view K[] = {"degree", "equave", "ratio", "anchor", "anchorId", "cents", "jiObject",
                                             "voice"};
    enum { kDegree, kEquave, kRatio, kAnchor, kAnchorId, kCents, kJi, kVoice };
    int32_t degree = 0, equave = 0;
    std::optional<tuning::Rational> ratio;
    int anchor = 0;
    uint32_t anchorId = 0, ji = 0, voice = 0;
    double cents = 0.0;
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg f(&at, K[static_cast<size_t>(k)]);
      switch (k) {
        case kDegree: degree = i32(f); break;
        case kEquave: equave = i32(f); break;
        case kRatio: {
          std::string s;
          r_.readString(f, s);
          ratio = parseRatioText(s);
          if (!ratio || !ratio->isPositive()) r_.failValue(f, "ratio must be \"n/d\" with positive integers n, d");
          break;
        }
        case kAnchor: anchor = name(f, kAnchorNames, "anchor"); break;
        case kAnchorId: anchorId = u32(f); break;
        case kCents: cents = r_.readDouble(f); break;
        case kJi: ji = u32(f); break;
        case kVoice: voice = u32(f); break;
      }
    }
    const int kinds = int(m.has(kDegree)) + int(m.has(kRatio)) + int(m.has(kCents)) + int(m.has(kJi));
    if (kinds != 1)
      json::fail(at, "a pitch needs exactly one of \"degree\", \"ratio\", \"cents\" and \"jiObject\"",
                 ErrorCode::InvalidArgument, m.start());
    auto only = [&](int key, bool allowed, const char* owner) {
      if (m.has(key) && !allowed)
        json::fail(PathSeg(&at, K[static_cast<size_t>(key)]), std::string("only allowed with \"") + owner + "\"",
                   ErrorCode::InvalidArgument, m.start());
    };
    only(kEquave, m.has(kDegree), "degree");
    only(kAnchor, m.has(kRatio), "ratio");
    only(kAnchorId, m.has(kRatio), "ratio");
    only(kVoice, m.has(kJi), "jiObject");
    if (m.has(kDegree)) return tuning::PitchId::fromDegree(degree, equave);
    if (m.has(kRatio)) return tuning::PitchId::fromRatio(*ratio, static_cast<tuning::AnchorKind>(anchor), anchorId);
    if (m.has(kCents)) return tuning::PitchId::fromCents(cents);
    m.require({kVoice});
    return tuning::PitchId::fromHarmonic(ji, voice);
  }

  // ---- Cross-references (after the whole project is read) ----
  void validate(const PathSeg& root, const Project& p) {
    std::set<ParamRef> automated;
    auto walk = [&](auto& self, const PathSeg& listAt, const std::vector<TrackPtr>& list) -> void {
      for (size_t i = 0; i < list.size(); ++i) {
        const PathSeg at(&listAt, i);
        validateTrack(at, p, *list[i], automated);
        if (!list[i]->children.empty()) self(self, PathSeg(&at, "children"), list[i]->children);
      }
    };
    walk(walk, PathSeg(&root, "tracks"), p.tracks);
    walk(walk, PathSeg(&root, "returns"), p.returns);
    validateTrack(PathSeg(&root, "master"), p, *p.master, automated);
    if (Status s = detail::checkRouting(p); !s) json::fail(root, s.error().message);
    const PathSeg wiresAt(&root, "wires");
    const WireList& wl = wiresOf(p);
    for (size_t i = 0; i < wl.size(); ++i) {
      const PathSeg el(&wiresAt, i);
      if (!findDevice(p, wl[i].from).track) json::fail(PathSeg(&el, "from"), "no device with this id");
      if (!findDevice(p, wl[i].to).track) json::fail(PathSeg(&el, "to"), "no device with this id");
      for (size_t j = 0; j < i; ++j)
        if (wl[j].from == wl[i].from && wl[j].port == wl[i].port && wl[j].to == wl[i].to && wl[j].param == wl[i].param)
          json::fail(el, "these two ports are already wired (wire " + std::to_string(wl[j].id.value) + ")");
    }
    // T4.8: a param has at most one macro mapping in the project.
    std::set<std::pair<uint32_t, std::string>> mapped;
    forEachDevice(p, [&](const Device& d, const Track&) {
      if (!d.rack) return;
      for (const MacroMapping& m : d.rack->mappings)
        if (!mapped.emplace(m.device.value, m.param).second)
          json::fail(root, "device " + std::to_string(m.device.value) + " param '" + m.param + "' is mapped to two macros");
    });

    std::sort(ids_.begin(), ids_.end());
    const auto dup = std::adjacent_find(ids_.begin(), ids_.end());
    if (dup != ids_.end()) duplicateId(root, p, *dup);
  }

  void validateTrack(const PathSeg& at, const Project& p, const Track& t, std::set<ParamRef>& automated) {
    if (Status s = detail::checkAssignment(p, t.tuning); !s) json::fail(PathSeg(&at, "tuning"), s.error().message);
    const PathSeg mx(&at, "mixer");
    const PathSeg sends(&mx, "sends");
    for (size_t i = 0; i < t.mixer.sends.size(); ++i) {
      const Track* target = findTrackPtr(p, t.mixer.sends[i].target);
      if (!target || target->kind != TrackKind::Return || target->id == t.id) {
        const PathSeg el(&sends, i);
        json::fail(PathSeg(&el, "target"), "send target must be another return track");
      }
    }
    if (t.mixer.output.kind == OutputKind::Track) {
      const Track* g = findTrackPtr(p, t.mixer.output.track);
      if (!g || g->kind != TrackKind::Group || g->id == t.id)
        json::fail(PathSeg(&mx, "output"), "output target must be another group track");
    }
    const PathSeg lanes(&at, "lanes");
    for (size_t i = 0; i < t.lanes.size(); ++i) {
      const PathSeg la(&lanes, i);
      const AutomationLane& l = *t.lanes[i];
      if (Status s = detail::checkLaneParam(p, t, l.param); !s) json::fail(PathSeg(&la, "param"), s.error().message);
      if (!automated.insert(l.param).second)
        json::fail(PathSeg(&la, "param"), "this parameter already has an automation lane");
    }
    const PathSeg clips(&at, "clips");
    for (size_t i = 0; i < t.clips.size(); ++i) {
      const PathSeg cl(&clips, i);
      if (const AudioClip* a = t.clips[i].audio.get()) {  // T5.3
        if (!findSample(p, a->sample))
          json::fail(PathSeg(&cl, "sample"), "unknown sample id " + std::to_string(a->sample.value));
        if (Status s = detail::checkAudioClip(p, *a); !s) json::fail(cl, s.error().message);
        continue;
      }
      if (Status s = detail::checkAssignment(p, t.clips[i].clip->tuning); !s)
        json::fail(PathSeg(&cl, "tuning"), s.error().message);
    }
  }

  [[noreturn]] void duplicateId(const PathSeg& root, const Project& p, uint32_t id) {
    // Find where it is used (file order of the containers); notes are reported by their clip.
    std::vector<std::string> where;
    auto see = [&](uint32_t v, const PathSeg& at) {
      if (v == id && where.size() < 2) where.push_back(json::renderPath(at));
    };
    const PathSeg tuns(&root, "tunings");
    for (size_t i = 0; i < p.tunings->size(); ++i) see((*p.tunings)[i]->id.value, PathSeg(&tuns, i));
    const PathSeg locs(&root, "locators");
    for (size_t i = 0; i < p.locators->size(); ++i) see((*p.locators)[i].id.value, PathSeg(&locs, i));
    const PathSeg wiresAt(&root, "wires");
    for (size_t i = 0; i < wiresOf(p).size(); ++i) see(wiresOf(p)[i].id.value, PathSeg(&wiresAt, i));
    const PathSeg samplesAt(&root, "samples");
    for (size_t i = 0; i < samplesOf(p).size(); ++i) see(samplesOf(p)[i].id.value, PathSeg(&samplesAt, i));
    auto walkTrack = [&](auto& self, const PathSeg& at, const Track& t) -> void {
      see(t.id.value, at);
      const PathSeg devs(&at, "devices");
      // T4.8: devices, chains, mappings and inner devices, in file order. M4 review M9: iterative (a stack of open
      // device lists and racks; the path segments live in a deque, which keeps their addresses).
      struct Open {
        const PathSeg* at;                     // the list's path, or the rack's ("…rack")
        const std::vector<DevicePtr>* list;    // a device list (null: a rack, walking its chains)
        const Rack* rack;
        size_t next;
      };
      std::deque<PathSeg> paths;
      std::vector<Open> open{{&devs, &t.devices, nullptr, 0}};
      while (!open.empty()) {
        Open& o = open.back();
        if (o.list != nullptr) {
          if (o.next == o.list->size()) {
            open.pop_back();
            continue;
          }
          const Device& d = *(*o.list)[o.next];
          const PathSeg& da = paths.emplace_back(o.at, o.next++);
          see(d.id.value, da);
          if (d.rack) open.push_back({&paths.emplace_back(&da, "rack"), nullptr, d.rack.get(), 0});
          continue;
        }
        const Rack& r = *o.rack;
        if (o.next < r.chains.size()) {
          const size_t j = o.next++;
          const PathSeg& cj = paths.emplace_back(&paths.emplace_back(o.at, "chains"), j);
          see(r.chains[j]->id.value, cj);
          open.push_back({&paths.emplace_back(&cj, "devices"), &r.chains[j]->devices, nullptr, 0});
          continue;
        }
        const PathSeg ma(o.at, "mappings");
        for (size_t j = 0; j < r.mappings.size(); ++j) see(r.mappings[j].id.value, PathSeg(&ma, j));
        open.pop_back();
      }
      const PathSeg lanes(&at, "lanes");
      for (size_t i = 0; i < t.lanes.size(); ++i) {
        const PathSeg la(&lanes, i);
        see(t.lanes[i]->id.value, la);
        const PathSeg pts(&la, "points");
        for (size_t j = 0; j < t.lanes[i]->points.size(); ++j) see(t.lanes[i]->points[j].id.value, PathSeg(&pts, j));
      }
      const PathSeg clips(&at, "clips");
      for (size_t i = 0; i < t.clips.size(); ++i) {
        const PathSeg cl(&clips, i);
        see(t.clips[i].id.value, cl);
        const PathSeg notes(&cl, "notes");
        if (t.clips[i].clip)
          for (const Note& n : t.clips[i].clip->notes) see(n.id.value, notes);
      }
      const PathSeg kids(&at, "children");
      for (size_t i = 0; i < t.children.size(); ++i) self(self, PathSeg(&kids, i), *t.children[i]);
    };
    const PathSeg tracks(&root, "tracks");
    for (size_t i = 0; i < p.tracks.size(); ++i) walkTrack(walkTrack, PathSeg(&tracks, i), *p.tracks[i]);
    const PathSeg rets(&root, "returns");
    for (size_t i = 0; i < p.returns.size(); ++i) walkTrack(walkTrack, PathSeg(&rets, i), *p.returns[i]);
    walkTrack(walkTrack, PathSeg(&root, "master"), *p.master);
    std::string msg = "id " + std::to_string(id) + " is used by more than one entity";
    if (where.size() == 2) msg = where[1] + ": " + msg + " (also " + where[0] + ")";
    else if (!where.empty()) msg = where[0] + ": " + msg;
    throw json::JsonFail{ErrorCode::InvalidArgument, "", std::move(msg), std::string_view::npos};
  }

  // ---- View state ----
  // Read during the main pass; ids are checked against the finished project afterwards.
  struct RawViewEntry {
    uint32_t id;
    bool flag;
    int32_t height;
    size_t pos;
  };
  struct RawNudge {
    uint32_t id;
    PatchNudge nudge;
    size_t pos;
  };
  struct RawView {
    std::vector<RawViewEntry> tracks, lanes;
    std::vector<RawNudge> nudges;  // T4.13 (optional "patch")
  };

  RawView view(const PathSeg& at) {
    RawView v;
    static constexpr std::string_view K[] = {"tracks", "lanes", "patch"};
    Members m(r_, at, K);
    for (int k; (k = m.next()) >= 0;) {
      const PathSeg list(&at, K[static_cast<size_t>(k)]);
      if (k == 2) {
        r_.beginArray(list);
        for (size_t i = 0; r_.nextElement(list); ++i) {
          const PathSeg el(&list, i);
          static constexpr std::string_view N[] = {"id", "dx", "dy"};
          RawNudge e{0, {}, r_.pos()};
          Members em(r_, el, N);
          for (int q; (q = em.next()) >= 0;) {
            const PathSeg f(&el, N[static_cast<size_t>(q)]);
            if (q == 0) e.id = ref(f);
            if (q == 1) e.nudge.dx = r_.readFloat(f);
            if (q == 2) e.nudge.dy = r_.readFloat(f);
          }
          em.require({0, 1, 2});
          v.nudges.push_back(e);
        }
        continue;
      }
      std::vector<RawViewEntry>& out = k == 0 ? v.tracks : v.lanes;
      r_.beginArray(list);
      for (size_t i = 0; r_.nextElement(list); ++i) {
        const PathSeg el(&list, i);
        static constexpr std::string_view T[] = {"id", "folded", "height"};
        static constexpr std::string_view L[] = {"id", "visible", "height"};
        const std::span<const std::string_view> names = k == 0 ? std::span<const std::string_view>(T)
                                                               : std::span<const std::string_view>(L);
        RawViewEntry e{0, false, 0, r_.pos()};
        Members em(r_, el, names);
        for (int q; (q = em.next()) >= 0;) {
          const PathSeg f(&el, names[static_cast<size_t>(q)]);
          if (q == 0) e.id = ref(f);
          if (q == 1) e.flag = r_.readBool(f);
          if (q == 2) {
            e.height = i32(f);
            if (e.height < 0) r_.failValue(f, "height must be >= 0 (0 = the theme's default)");
          }
        }
        em.require({0, 1, 2});
        out.push_back(e);
      }
    }
    m.require({0, 1});
    return v;
  }

  ViewState resolveView(const PathSeg& at, const RawView& raw, const Project& p) {
    std::vector<uint32_t> trackIds, laneIds;
    forEachTrack(p, [&](const Track& t, const Track*) {
      trackIds.push_back(t.id.value);
      for (const LanePtr& l : t.lanes) laneIds.push_back(l->id.value);
    });
    std::sort(trackIds.begin(), trackIds.end());
    std::sort(laneIds.begin(), laneIds.end());
    ViewState v;
    for (int k = 0; k < 2; ++k) {
      const PathSeg list(&at, k == 0 ? "tracks" : "lanes");
      const std::vector<RawViewEntry>& entries = k == 0 ? raw.tracks : raw.lanes;
      const std::vector<uint32_t>& ids = k == 0 ? trackIds : laneIds;
      for (size_t i = 0; i < entries.size(); ++i) {
        const RawViewEntry& e = entries[i];
        const PathSeg el(&list, i);
        if (!std::binary_search(ids.begin(), ids.end(), e.id))
          json::fail(PathSeg(&el, "id"), k == 0 ? "no track has this id" : "no automation lane has this id",
                     ErrorCode::InvalidArgument, e.pos);
        const bool fresh = k == 0 ? v.tracks.emplace(TrackId{e.id}, TrackView{e.flag, e.height}).second
                                  : v.lanes.emplace(LaneId{e.id}, LaneView{e.flag, e.height}).second;
        if (!fresh) json::fail(el, "duplicate view entry for this id", ErrorCode::InvalidArgument, e.pos);
      }
    }
    if (!raw.nudges.empty()) {  // T4.13: by track or device id
      std::vector<uint32_t> entityIds = trackIds;
      forEachTrack(p, [&](const Track& t, const Track*) {
        for (const DevicePtr& d : t.devices) entityIds.push_back(d->id.value);
      });
      std::sort(entityIds.begin(), entityIds.end());
      const PathSeg list(&at, "patch");
      for (size_t i = 0; i < raw.nudges.size(); ++i) {
        const RawNudge& e = raw.nudges[i];
        const PathSeg el(&list, i);
        if (!std::binary_search(entityIds.begin(), entityIds.end(), e.id))
          json::fail(PathSeg(&el, "id"), "no track or device has this id", ErrorCode::InvalidArgument, e.pos);
        if (!std::isfinite(e.nudge.dx) || !std::isfinite(e.nudge.dy))
          json::fail(el, "a nudge must be finite", ErrorCode::InvalidArgument, e.pos);
        if (!v.patchNudges.emplace(e.id, e.nudge).second)
          json::fail(el, "duplicate view entry for this id", ErrorCode::InvalidArgument, e.pos);
      }
    }
    return v;
  }

  Reader r_;
  std::vector<uint32_t> ids_;
};

// ---- Errors ------------------------------------------------------------------------------------------------------

Error toError(const json::JsonFail& f, std::string_view text, std::string_view fileName, bool positionsValid) {
  Error e;
  e.code = f.code;
  e.message = f.path.empty() ? f.message : f.path + ": " + f.message;
  e.location.file = std::string(fileName);
  if (positionsValid && f.pos != std::string_view::npos && f.pos <= text.size()) {
    uint32_t line = 1, col = 1;
    for (size_t i = 0; i < f.pos; ++i) {
      if (text[i] == '\n') {
        ++line;
        col = 1;
      } else if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
        ++col;  // count code points, not UTF-8 continuation bytes
      }
    }
    e.location.line = line;
    e.location.col = col;
  }
  return e;
}

// schemaVersion: the first member in files we write; otherwise found by skipping the other members.
int64_t readSchemaVersion(std::string_view text) {
  Reader r(text);
  const PathSeg root;
  r.beginObject(root);
  std::string_view k;
  while (r.nextKey(root, k)) {
    if (k == "schemaVersion") return r.readInt(PathSeg(&root, "schemaVersion"));
    const std::string key(k);
    r.skipValue(PathSeg(&root, key));
  }
  json::fail(root, "missing key \"schemaVersion\"", ErrorCode::InvalidArgument, 0);
}

}  // namespace

// ==== Migrations ==================================================================================================

namespace {

// The tick resolution is part of the file format.
static_assert(kPpq == 221760, "changing PPQ needs a schema bump and a migration that rescales every stored tick");

// Schema 1 → 2 (T0.5-fix1): PPQ went from 27,720 to 221,760, so every stored tick is multiplied by 8. The tick
// fields are: loop.start/end, locators[].tick, timeline.tempo[].tick, lanes[].points[].tick (on every track,
// return, nested child and the master), clips[].start/length/contentStart, clips[].loop.start/end and
// clips[].notes[].start/length. Bars (time signatures), ids, BPM, view state and every other number are unchanged.
// Members of the wrong shape or type are left as they are for the decoder to report with their path; a tick that
// would overflow int64 when scaled is an error here (and anything beyond kMaxTicks is rejected by the decoder).
class TickScaler {
public:
  static constexpr int64_t kScale = 8;  // 221760 / 27720; fixed by history, independent of the current kPpq

  Status run(nlohmann::json& doc) {
    if (!doc.is_object()) return Ok{};
    if (auto* loop = member(doc, "loop")) {
      tick(*loop, "start", "$.loop");
      tick(*loop, "end", "$.loop");
    }
    each(doc, "locators", "$.locators", [&](nlohmann::json& l, const std::string& at) { tick(l, "tick", at); });
    if (auto* tl = member(doc, "timeline"))
      each(*tl, "tempo", "$.timeline.tempo", [&](nlohmann::json& p, const std::string& at) { tick(p, "tick", at); });
    each(doc, "tracks", "$.tracks", [&](nlohmann::json& t, const std::string& at) { track(t, at); });
    each(doc, "returns", "$.returns", [&](nlohmann::json& t, const std::string& at) { track(t, at); });
    if (auto* m = member(doc, "master")) track(*m, "$.master");
    if (!error_.empty()) return Error{ErrorCode::InvalidArgument, error_, {}};
    return Ok{};
  }

private:
  static nlohmann::json* member(nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return nullptr;
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
  }

  template <class F>
  void each(nlohmann::json& obj, const char* key, const std::string& path, F&& f) {
    nlohmann::json* arr = member(obj, key);
    if (!arr || !arr->is_array()) return;
    for (size_t i = 0; i < arr->size(); ++i) f((*arr)[i], path + "[" + std::to_string(i) + "]");
  }

  void tick(nlohmann::json& obj, const char* key, const std::string& path) {
    nlohmann::json* v = member(obj, key);
    if (!v || !v->is_number_integer()) return;
    constexpr int64_t kLimit = std::numeric_limits<int64_t>::max() / kScale;
    if (v->is_number_unsigned()) {
      const uint64_t u = v->get<uint64_t>();
      if (u <= static_cast<uint64_t>(kLimit)) {
        *v = static_cast<int64_t>(u) * kScale;
        return;
      }
    } else {
      const int64_t t = v->get<int64_t>();
      if (t <= kLimit && t >= -kLimit) {
        *v = t * kScale;
        return;
      }
    }
    if (error_.empty()) error_ = path + "." + key + ": tick out of range";
  }

  void track(nlohmann::json& t, const std::string& at) {
    each(t, "lanes", at + ".lanes", [&](nlohmann::json& lane, const std::string& la) {
      each(lane, "points", la + ".points", [&](nlohmann::json& p, const std::string& pa) { tick(p, "tick", pa); });
    });
    each(t, "clips", at + ".clips", [&](nlohmann::json& c, const std::string& ca) {
      tick(c, "start", ca);
      tick(c, "length", ca);
      tick(c, "contentStart", ca);
      if (auto* loop = member(c, "loop")) {
        tick(*loop, "start", ca + ".loop");
        tick(*loop, "end", ca + ".loop");
      }
      each(c, "notes", ca + ".notes", [&](nlohmann::json& n, const std::string& na) {
        tick(n, "start", na);
        tick(n, "length", na);
      });
    });
    each(t, "children", at + ".children", [&](nlohmann::json& c, const std::string& ca) { track(c, ca); });
  }

  std::string error_;
};

Status migrateTicksPpq27720To221760(nlohmann::json& doc) { return TickScaler().run(doc); }

// Schema 2 → 3 (T4.1, QUESTIONS Q129): device chains. Before, every MIDI track played the default instrument
// implicitly; now it is the first device of its chain. Every track gets "devices": MIDI tracks
// [{"id": <new>, "patch": "BasicSynth", "kind": "instrument"}], every other track []. New ids continue above the
// largest "id" anywhere in the file (one counter for every entity), in track order: tracks depth-first, returns,
// master. Members of the wrong shape are left for the decoder to report.
class ChainAdder {
public:
  Status run(nlohmann::json& doc) {
    if (!doc.is_object()) return Ok{};
    maxId(doc);
    for (const char* list : {"tracks", "returns"})
      if (auto it = doc.find(list); it != doc.end() && it->is_array())
        for (nlohmann::json& t : *it) track(t);
    if (auto it = doc.find("master"); it != doc.end()) track(*it);
    if (next_ > 0xFFFFFFFEull) return Error{ErrorCode::InvalidArgument, "no id left for the default instruments", {}};
    return Ok{};
  }

private:
  void maxId(const nlohmann::json& node) {
    if (node.is_object()) {
      for (auto it = node.begin(); it != node.end(); ++it) {
        if (it.key() == "id" && it->is_number_unsigned()) {
          next_ = std::max<uint64_t>(next_, it->get<uint64_t>() + 1);
        } else if (it.key() == "id" && it->is_number_integer()) {
          const int64_t v = it->get<int64_t>();
          if (v >= 0) next_ = std::max<uint64_t>(next_, static_cast<uint64_t>(v) + 1);
        }
        maxId(*it);
      }
    } else if (node.is_array()) {
      for (const nlohmann::json& e : node) maxId(e);
    }
  }
  void track(nlohmann::json& t) {
    if (!t.is_object() || t.contains("devices")) return;
    nlohmann::json chain = nlohmann::json::array();
    if (auto k = t.find("kind"); k != t.end() && k->is_string() && k->get<std::string>() == "midi") {
      nlohmann::json d = nlohmann::json::object();
      d["id"] = next_++;
      d["patch"] = std::string(kDefaultInstrument);
      d["kind"] = "instrument";
      chain.push_back(std::move(d));
    }
    if (auto c = t.find("children"); c != t.end() && c->is_array())
      for (nlohmann::json& child : *c) track(child);
    t["devices"] = std::move(chain);
  }
  uint64_t next_ = 1;
};

Status migrateDefaultInstrumentToChain(nlohmann::json& doc) { return ChainAdder().run(doc); }

// 3 → 4 (T4.7): schema 4 adds the optional top-level "wires" list; a schema-3 file has none, so nothing changes.
Status migrateAddWires(nlohmann::json& doc) {
  if (!doc.is_object()) return Error{ErrorCode::InvalidArgument, "not an object", {}};
  return Ok{};
}

// 4 → 5 (T4.4-fix1): schema 5 adds the optional per-device "source" (a user device's own patch text); every device of
// a schema-4 file plays its stock patch, so nothing changes. Self-contained (renumber, do not merge, if another schema
// step lands first).
Status migrateAddUserDeviceSource(nlohmann::json& doc) {
  if (!doc.is_object()) return Error{ErrorCode::InvalidArgument, "not an object", {}};
  return Ok{};
}

// 5 → 6 (T4.8): schema 6 lets a device hold a "rack" (chains, macro mappings, options); a schema-5 file has no rack, so
// nothing changes. Self-contained: it reads nothing the other steps write.
Status migrateAddRacks(nlohmann::json& doc) {
  if (!doc.is_object()) return Error{ErrorCode::InvalidArgument, "not an object", {}};
  return Ok{};
}

// 6 → 7 (T5.3): schema 7 adds audio tracks ("kind": "audio"), audio clips (tagged "kind": "audio" in a track's
// "clips") and the optional top-level "samples" table; a schema-6 file has none of them, so nothing changes.
// Self-contained (renumber, do not merge, if another schema step lands first).
Status migrateAddAudio(nlohmann::json& doc) {
  if (!doc.is_object()) return Error{ErrorCode::InvalidArgument, "not an object", {}};
  return Ok{};
}

constexpr Migration kBuiltinMigrations[] = {
    {1, migrateTicksPpq27720To221760},
    {2, migrateDefaultInstrumentToChain},
    {3, migrateAddWires},
    {4, migrateAddUserDeviceSource},
    {5, migrateAddRacks},
    {6, migrateAddAudio},
};
static_assert(std::size(kBuiltinMigrations) == static_cast<size_t>(kSchemaVersion - 1),
              "one built-in migration per schema step");

}  // namespace

std::span<const Migration> builtinMigrations() noexcept { return kBuiltinMigrations; }

Result<std::string> serializeProject(const Project& p, const ViewState& view) {
  std::string out;
  size_t notes = 0, audio = 0;
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const ClipSlot& c : t.clips) {
      notes += c.clip ? c.clip->notes.size() : 0;
      audio += c.audio ? 1 : 0;
    }
  });
  out.reserve(4096 + notes * 112 + audio * 560);  // T5.x-perf-audio: an audio clip line is ~520 bytes
  try {
    Encoder e(out);
    e.project(p, view);
  } catch (const json::JsonFail& f) {
    return Error{f.code, f.path + ": " + f.message, {}};
  } catch (const std::bad_alloc&) {
    return Error{ErrorCode::Unknown, "out of memory while saving the project", {}};
  }
  return out;
}

Result<LoadedProject> parseProject(std::string_view text, const LoadOptions& options) {
  if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
  std::string migrated;
  int64_t version = 0;
  try {
    version = readSchemaVersion(text);
    if (version > kSchemaVersion) {
      return Error{ErrorCode::Unsupported,
                   "$.schemaVersion: the project was saved by a newer pqdaw (schema " + std::to_string(version) +
                       "; this build reads up to " + std::to_string(kSchemaVersion) + ")",
                   {std::string(options.fileName), 0, 0}};
    }
    if (version < kSchemaVersion) {
      // Precise syntax errors first (positions in the original text), then the generic DOM migration.
      Reader syntax(text);
      const PathSeg root;
      syntax.skipValue(root);
      syntax.expectEnd(root);
      nlohmann::json doc = nlohmann::json::parse(text.begin(), text.end());
      for (int64_t v = version; v < kSchemaVersion; ++v) {
        const Migration* step = nullptr;
        for (const Migration& m : options.migrations)
          if (m.from == v && m.fn) step = &m;
        if (!step)
          return Error{ErrorCode::Unsupported, "$.schemaVersion: no migration from schema version " + std::to_string(v),
                       {std::string(options.fileName), 0, 0}};
        if (Status s = step->fn(doc); !s)
          return Error{s.error().code,
                       "$: migrating schema " + std::to_string(v) + " to " + std::to_string(v + 1) + " failed: " +
                           s.error().message,
                       {std::string(options.fileName), 0, 0}};
        doc["schemaVersion"] = v + 1;
      }
      migrated = doc.dump(2);
    }
  } catch (const json::JsonFail& f) {
    return toError(f, text, options.fileName, true);
  } catch (const std::exception& e) {
    return Error{ErrorCode::Parse, std::string("$: ") + e.what(), {std::string(options.fileName), 0, 0}};
  }

  const bool wasMigrated = version < kSchemaVersion;
  const std::string_view body = wasMigrated ? std::string_view(migrated) : text;
  try {
    Decoder d(body);
    LoadedProject out = d.project();
    out.fileSchemaVersion = static_cast<int32_t>(version);
    return out;
  } catch (const json::JsonFail& f) {
    return toError(f, body, options.fileName, !wasMigrated);
  } catch (const std::bad_alloc&) {
    return Error{ErrorCode::Unknown, "out of memory while loading the project", {std::string(options.fileName), 0, 0}};
  }
}

}  // namespace pq::doc

// ==== T5.11: user-library files (library_files.h) ==================================================================

namespace pq::doc {

namespace {

template <class F>
Result<std::string> encodeLibrary(F&& f) {
  std::string out;
  try {
    Encoder e(out);
    f(e);
  } catch (const json::JsonFail& fail) {
    return Error{fail.code, fail.path + ": " + fail.message, {}};
  } catch (const std::bad_alloc&) {
    return Error{ErrorCode::Unknown, "out of memory while writing a library file", {}};
  }
  return out;
}

template <class T, class F>
Result<T> decodeLibrary(std::string_view text, std::string_view fileName, F&& f) {
  if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
  try {
    Decoder d(text);
    return f(d);
  } catch (const json::JsonFail& fail) {
    return toError(fail, text, fileName, true);
  } catch (const std::bad_alloc&) {
    return Error{ErrorCode::Unknown, "out of memory while reading a library file", {std::string(fileName), 0, 0}};
  }
}

// A clip's Main tuning as a private one (the file has no main tunings).
TuningAssignment portable(const TuningAssignment& a, const Project* p) {
  if (a.mode != TuningMode::Main) return a;
  const TuningSlot* s = p ? findTuning(*p, a.main) : nullptr;
  return s ? TuningAssignment::privateTuning(s->def) : TuningAssignment::inherit();
}

}  // namespace

Result<std::string> serializePreset(const Device& device, std::string_view name) {
  return encodeLibrary([&](Encoder& e) { e.presetFile(device, name); });
}
Result<PresetFile> parsePreset(std::string_view text, std::string_view fileName) {
  return decodeLibrary<PresetFile>(text, fileName, [](Decoder& d) { return d.presetFile(); });
}
Result<std::string> serializeUserDevice(const UserDeviceFile& f) {
  if (f.text.empty()) return Error{ErrorCode::InvalidArgument, "$.source: must not be empty", {}};
  if (f.patch.empty()) return Error{ErrorCode::InvalidArgument, "$.patch: must not be empty", {}};
  return encodeLibrary([&](Encoder& e) { e.userDeviceFile(f); });
}
Result<UserDeviceFile> parseUserDevice(std::string_view text, std::string_view fileName) {
  return decodeLibrary<UserDeviceFile>(text, fileName, [](Decoder& d) { return d.userDeviceFile(); });
}
DevicePtr makeUserDevice(DeviceId id, const UserDeviceFile& f) {
  auto d = std::make_shared<Device>();
  d->id = id;
  d->patch = f.patch;
  d->kind = f.kind;
  auto src = std::make_shared<UserPatch>();
  src->text = f.text;
  d->source = std::move(src);
  return d;
}
Result<std::string> serializeTuning(const TuningFile& f) {
  return encodeLibrary([&](Encoder& e) { e.tuningFile(f); });
}
Result<TuningFile> parseTuning(std::string_view text, std::string_view fileName) {
  return decodeLibrary<TuningFile>(text, fileName, [](Decoder& d) { return d.tuningFile(); });
}
Result<std::string> serializeClip(const ClipFile& f, const Project* p) {
  ClipFile g = f;
  if (g.clip.audio) {
    if (!g.sample || g.sample->id != g.clip.audio->sample)
      return Error{ErrorCode::InvalidArgument, "$.samples: an audio clip needs its sample", {}};
    if (g.clip.audio->tuning.mode == TuningMode::Main) {
      AudioClip a = *g.clip.audio;
      a.tuning = portable(a.tuning, p);
      g.clip.audio = std::make_shared<const AudioClip>(std::move(a));
    }
  } else if (!g.clip.clip) {
    return Error{ErrorCode::InvalidArgument, "$.clip: no clip", {}};
  } else if (g.clip.clip->tuning.mode == TuningMode::Main) {
    NoteClip c = withNotes(*g.clip.clip, g.clip.clip->notes);
    c.tuning = portable(c.tuning, p);
    g.clip.clip = std::make_shared<const NoteClip>(std::move(c));
  }
  if (!g.clip.audio) g.sample.reset();
  return encodeLibrary([&](Encoder& e) { e.clipFile(g); });
}
Result<ClipFile> parseClip(std::string_view text, std::string_view fileName) {
  return decodeLibrary<ClipFile>(text, fileName, [](Decoder& d) { return d.clipFile(); });
}

}  // namespace pq::doc
