#include "pq/graph/types.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>

namespace pq::graph {

std::string_view toText(Kind k) noexcept {
  switch (k) {
    case Kind::Infer: return "?";
    case Kind::Real: return "real";
    case Kind::Bool: return "bool";
  }
  return "?";
}

std::string_view toText(Rate r) noexcept {
  switch (r) {
    case Rate::Infer: return "?";
    case Rate::Const: return "const";
    case Rate::Param: return "param";
    case Rate::Audio: return "audio";
    case Rate::Event: return "event";
  }
  return "?";
}

std::string_view toText(Poly p) noexcept {
  switch (p) {
    case Poly::Infer: return "?";
    case Poly::Global: return "global";
    case Poly::Voice: return "voice";
  }
  return "?";
}

std::string toText(const PortType& t) {
  std::string s;
  auto add = [&](std::string_view w) {
    if (!s.empty()) s += ' ';
    s += w;
  };
  if (t.kind != Kind::Infer) add(toText(t.kind));
  if (t.rate != Rate::Infer) add(toText(t.rate));
  if (t.poly != Poly::Infer) add(toText(t.poly));
  if (t.width == kWidthMono) add("mono");
  else if (t.width == kWidthStereo) add("stereo");
  else if (t.width != kWidthInfer) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "ch(%u)", static_cast<unsigned>(t.width));
    add(buf);
  }
  return s.empty() ? std::string("?") : s;
}

namespace {
struct UnitName {
  Unit unit;
  std::string_view suffix;
};
constexpr std::array<UnitName, 10> kUnits = {{{Unit::None, ""},
                                              {Unit::Hz, "Hz"},
                                              {Unit::kHz, "kHz"},
                                              {Unit::ms, "ms"},
                                              {Unit::s, "s"},
                                              {Unit::smp, "smp"},
                                              {Unit::dB, "dB"},
                                              {Unit::cents, "c"},
                                              {Unit::st, "st"},
                                              {Unit::percent, "%"}}};
}  // namespace

std::string_view unitSuffix(Unit u) noexcept {
  for (const UnitName& n : kUnits)
    if (n.unit == u) return n.suffix;
  return "";
}

bool unitFromSuffix(std::string_view s, Unit& out) noexcept {
  if (s.empty()) return false;
  for (const UnitName& n : kUnits) {
    if (n.suffix == s) {
      out = n.unit;
      return true;
    }
  }
  return false;
}

double Constant::value() const noexcept {
  switch (unit) {
    case Unit::None:
    case Unit::Hz:
    case Unit::s:
    case Unit::smp:
    case Unit::cents: return literal;
    case Unit::kHz: return literal * 1000.0;
    case Unit::ms: return literal * 0.001;
    case Unit::dB: return std::pow(10.0, literal / 20.0);
    case Unit::st: return literal * 100.0;
    case Unit::percent: return literal * 0.01;
  }
  return literal;
}

bool operator==(const Constant& a, const Constant& b) noexcept {
  return a.kind == b.kind && a.unit == b.unit &&
         std::bit_cast<uint64_t>(a.literal) == std::bit_cast<uint64_t>(b.literal);
}

bool isIdentifier(std::string_view s) noexcept {
  if (s.empty()) return false;
  auto alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
  if (!alpha(s[0])) return false;
  for (char c : s.substr(1))
    if (!alpha(c) && !(c >= '0' && c <= '9')) return false;
  return true;
}

bool isKeyword(std::string_view s) noexcept {
  constexpr std::string_view kKeywords[] = {"patch", "import", "param",      "in",     "out",  "export",
                                            "solve", "iterations", "layout", "true", "false"};
  for (std::string_view k : kKeywords)
    if (k == s) return true;
  return false;
}

bool isKeywordNotName(std::string_view s) noexcept { return isKeyword(s) && s != "in" && s != "out"; }

bool isReservedName(std::string_view s) noexcept { return s == "voice" || s == "transport" || s == "sr"; }

}  // namespace pq::graph
