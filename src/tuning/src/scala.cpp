#include "pq/tuning/scala.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace pq::tuning {

namespace {

struct Line {
  std::string_view text;  // without the line break
  uint32_t number;        // 1-based
};

// Splits on LF, CRLF or a lone CR. A final line without a break is kept; an empty tail after the last break is not.
std::vector<Line> splitLines(std::string_view text) {
  std::vector<Line> lines;
  size_t start = 0;
  uint32_t number = 1;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c != '\n' && c != '\r') continue;
    lines.push_back({text.substr(start, i - start), number++});
    if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
    start = i + 1;
  }
  if (start < text.size()) lines.push_back({text.substr(start), number});
  return lines;
}

std::string_view stripBom(std::string_view text) {
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF)
    return text.substr(3);
  return text;
}

bool isSpace(char c) { return c == ' ' || c == '\t'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
  return s;
}

bool isComment(const Line& l) { return !l.text.empty() && l.text.front() == '!'; }
bool isBlank(const Line& l) { return trim(l.text).empty(); }

Error parseError(std::string message, std::string_view fileName, uint32_t line) {
  return Error{ErrorCode::Parse, std::move(message), SourceLocation{std::string(fileName), line, 0}};
}

// Decimal digit string (no sign) → uint64, nullopt if it does not fit.
std::optional<uint64_t> parseU64(std::string_view digits) {
  uint64_t v = 0;
  const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v);
  if (r.ec != std::errc{} || r.ptr != digits.data() + digits.size()) return std::nullopt;
  return v;
}

std::string_view stripLeadingZeros(std::string_view digits) {
  while (digits.size() > 1 && digits.front() == '0') digits.remove_prefix(1);
  return digits;
}

bool isZeroDigits(std::string_view digits) { return stripLeadingZeros(digits) == "0" || digits.empty(); }

// log2 of a positive decimal integer of any length: the leading 17 significant digits as a double (relative error
// ~1e-16) times 10^(remaining digits). Good to ~1e-13 cents.
double log2OfDigits(std::string_view digits) {
  digits = stripLeadingZeros(digits);
  const size_t lead = digits.size() < 17 ? digits.size() : 17;
  double m = 0.0;
  std::from_chars(digits.data(), digits.data() + lead, m);
  return std::log2(m) + static_cast<double>(digits.size() - lead) * std::log2(10.0);
}

// Parses the pitch value at the start of a pitch line (see scala.h for the rules). The message is empty on success.
struct PitchParse {
  ScalaPitch pitch;
  std::string message;
};

PitchParse parsePitchLine(std::string_view line) {
  PitchParse out;
  size_t i = 0;
  while (i < line.size() && isSpace(line[i])) ++i;
  const size_t valueStart = i;
  char sign = 0;
  if (i < line.size() && (line[i] == '-' || line[i] == '+')) sign = line[i++];
  const size_t intStart = i;
  while (i < line.size() && isDigit(line[i])) ++i;
  const std::string_view intDigits = line.substr(intStart, i - intStart);

  if (i < line.size() && line[i] == '.') {
    // Cents: [sign]digits.[digits]
    if (intDigits.empty()) {
      out.message = "cents value must have digits before the period";
      return out;
    }
    ++i;
    while (i < line.size() && isDigit(line[i])) ++i;
    const size_t numStart = (sign == '+') ? valueStart + 1 : valueStart;  // from_chars takes '-' but not '+'
    double v = 0.0;
    const auto r = std::from_chars(line.data() + numStart, line.data() + i, v, std::chars_format::fixed);
    if (r.ec != std::errc{} || !std::isfinite(v)) {
      out.message = "cents value out of range";
      return out;
    }
    out.pitch = ScalaPitch::fromCents(v);
    return out;
  }

  // Ratio: digits[/digits]
  if (intDigits.empty()) {
    out.message = "expected a pitch value (a ratio like 3/2 or cents like 701.955)";
    return out;
  }
  if (sign == '-') {
    out.message = "negative ratios are not allowed";
    return out;
  }
  if (sign == '+') {
    out.message = "a ratio cannot have a sign";
    return out;
  }
  std::string_view denDigits = "1";
  if (i < line.size() && line[i] == '/') {
    ++i;
    const size_t denStart = i;
    while (i < line.size() && isDigit(line[i])) ++i;
    denDigits = line.substr(denStart, i - denStart);
    if (denDigits.empty()) {
      out.message = "ratio is missing its denominator";
      return out;
    }
  }
  if (isZeroDigits(intDigits)) {
    out.message = "ratio must be positive";
    return out;
  }
  if (isZeroDigits(denDigits)) {
    out.message = "ratio has a zero denominator";
    return out;
  }
  constexpr uint64_t kMax = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  const auto n = parseU64(intDigits);
  const auto d = parseU64(denDigits);
  if (n && d && *n <= kMax && *d <= kMax) {
    out.pitch = ScalaPitch::fromRatio(Rational(static_cast<int64_t>(*n), static_cast<int64_t>(*d)));
    return out;
  }
  ScalaPitch p;
  p.kind = ScalaPitch::Kind::LargeRatio;
  p.largeNum = std::string(stripLeadingZeros(intDigits));
  p.largeDen = std::string(stripLeadingZeros(denDigits));
  p.cents = 1200.0 * (log2OfDigits(p.largeNum) - log2OfDigits(p.largeDen));
  out.pitch = std::move(p);
  return out;
}

// Shortest fixed-notation decimal that reads back to exactly v, always with a period (so Scala reads it as cents).
std::string centsText(double v) {
  char buf[512];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed);
  std::string s(buf, r.ptr);
  if (s.find('.') == std::string::npos) s += ".0";
  return s;
}

bool allDigits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!isDigit(c)) return false;
  return true;
}

// Integer at the start of a .kbm value line (optional sign, digits); the rest of the line is ignored.
std::optional<int64_t> leadingInt(std::string_view line) {
  line = trim(line);
  size_t i = 0;
  if (i < line.size() && (line[i] == '-' || line[i] == '+')) ++i;
  const size_t digitsStart = i;
  while (i < line.size() && isDigit(line[i])) ++i;
  if (i == digitsStart) return std::nullopt;
  int64_t v = 0;
  const size_t from = line[0] == '+' ? 1 : 0;
  const auto r = std::from_chars(line.data() + from, line.data() + i, v);
  if (r.ec != std::errc{}) return std::nullopt;
  return v;
}

std::optional<int32_t> leadingInt32(std::string_view line) {
  const auto v = leadingInt(line);
  if (!v || *v <= std::numeric_limits<int32_t>::min() || *v > std::numeric_limits<int32_t>::max())
    return std::nullopt;
  return static_cast<int32_t>(*v);
}

std::optional<double> leadingDouble(std::string_view line) {
  line = trim(line);
  size_t end = 0;
  while (end < line.size() && !isSpace(line[end])) ++end;
  const size_t from = (!line.empty() && line[0] == '+') ? 1 : 0;
  double v = 0.0;
  const auto r = std::from_chars(line.data() + from, line.data() + end, v);
  if (r.ec != std::errc{} || r.ptr == line.data() + from) return std::nullopt;
  return v;
}

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

// Mapping entry index of the reference key (floor-mod), or nullopt for a linear mapping (map size 0).
std::optional<int32_t> referenceEntry(const KeyboardMapping& k) {
  if (k.mapSize <= 0) return std::nullopt;
  const int64_t i = static_cast<int64_t>(k.referenceKey) - k.middleKey;
  const int64_t r = i - floorDiv(i, k.mapSize) * k.mapSize;
  return static_cast<int32_t>(r);
}

bool referenceUnmapped(const KeyboardMapping& k) {
  const auto e = referenceEntry(k);
  if (!e) return false;
  return static_cast<size_t>(*e) >= k.mapping.size() || k.mapping[static_cast<size_t>(*e)] == kUnmappedKey;
}

bool validUtf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    size_t len = 0;
    uint32_t cp = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07u;
    } else {
      return false;
    }
    if (i + len > s.size()) return false;
    for (size_t k = 1; k < len; ++k) {
      const auto cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    // Reject overlong forms, surrogates and values above U+10FFFF.
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += len;
  }
  return true;
}

}  // namespace

// ---- ScalaPitch / ScalaScale -----------------------------------------------------------------------------------

ScalaPitch ScalaPitch::fromRatio(const Rational& r) {
  ScalaPitch p;
  p.kind = Kind::Ratio;
  p.ratio = r;
  p.cents = tuning::cents(r);
  return p;
}

ScalaPitch ScalaPitch::fromCents(double c) {
  ScalaPitch p;
  p.kind = Kind::Cents;
  p.cents = c;
  return p;
}

bool operator==(const ScalaPitch& a, const ScalaPitch& b) noexcept {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case ScalaPitch::Kind::Ratio:
      return a.ratio == b.ratio;
    case ScalaPitch::Kind::Cents:
      return a.cents == b.cents;
    case ScalaPitch::Kind::LargeRatio:
      return a.largeNum == b.largeNum && a.largeDen == b.largeDen;
  }
  return false;
}

std::string ScalaScale::descriptionUtf8() const {
  if (validUtf8(description)) return description;
  std::string out;
  out.reserve(description.size() * 2);
  for (char ch : description) {
    const auto c = static_cast<unsigned char>(ch);
    if (c < 0x80) {
      out.push_back(ch);
    } else {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

// ---- .scl ------------------------------------------------------------------------------------------------------

Result<ScalaScale> parseScl(std::string_view text, std::string_view fileName) {
  const std::vector<Line> lines = splitLines(stripBom(text));
  size_t i = 0;
  while (i < lines.size() && isComment(lines[i])) ++i;
  if (i == lines.size()) return parseError("missing description line", fileName, 0);
  ScalaScale scale;
  scale.description = std::string(lines[i].text);
  ++i;

  auto nextValueLine = [&]() -> const Line* {
    while (i < lines.size() && (isComment(lines[i]) || isBlank(lines[i]))) ++i;
    return i < lines.size() ? &lines[i++] : nullptr;
  };

  const Line* countLine = nextValueLine();
  if (!countLine) return parseError("missing note count line", fileName, 0);
  const std::string_view countText = trim(countLine->text);
  const auto count = allDigits(countText) ? parseU64(countText) : std::nullopt;
  if (!count) return parseError("note count must be a non-negative integer", fileName, countLine->number);
  // A count larger than the remaining lines cannot be satisfied; checking first keeps a bogus count from reserving.
  if (*count > lines.size() - i)
    return parseError("file has fewer pitch lines than the note count " + std::to_string(*count), fileName,
                      countLine->number);
  scale.pitches.reserve(static_cast<size_t>(*count));

  for (uint64_t k = 0; k < *count; ++k) {
    const Line* l = nextValueLine();
    if (!l)
      return parseError("file has fewer pitch lines than the note count " + std::to_string(*count), fileName,
                        lines.empty() ? 0 : lines.back().number);
    PitchParse p = parsePitchLine(l->text);
    if (!p.message.empty()) return parseError(std::move(p.message), fileName, l->number);
    scale.pitches.push_back(std::move(p.pitch));
  }
  return scale;
}

Result<std::string> writeScl(const ScalaScale& scale, std::string_view fileName) {
  auto bad = [](std::string why) { return Error{ErrorCode::InvalidArgument, std::move(why), {}}; };
  if (scale.description.find_first_of("\r\n") != std::string::npos) return bad("description must be one line");
  if (!scale.description.empty() && scale.description.front() == '!')
    return bad("description cannot start with '!' (it would read back as a comment)");
  if (fileName.find_first_of("\r\n") != std::string_view::npos) return bad("file name must be one line");

  std::string out;
  out.reserve(64 + scale.description.size() + scale.pitches.size() * 16);
  out += "! ";
  out += fileName;
  out += "\r\n!\r\n";
  out += scale.description;
  out += "\r\n ";
  out += std::to_string(scale.pitches.size());
  out += "\r\n!\r\n";
  for (const ScalaPitch& p : scale.pitches) {
    out += ' ';
    switch (p.kind) {
      case ScalaPitch::Kind::Ratio:
        if (!p.ratio.isPositive()) return bad("ratio pitches must be positive");
        out += std::to_string(p.ratio.num());
        out += '/';
        out += std::to_string(p.ratio.den());
        break;
      case ScalaPitch::Kind::Cents:
        if (!std::isfinite(p.cents)) return bad("cents pitches must be finite");
        out += centsText(p.cents);
        break;
      case ScalaPitch::Kind::LargeRatio:
        if (!allDigits(p.largeNum) || !allDigits(p.largeDen) || isZeroDigits(p.largeNum) || isZeroDigits(p.largeDen))
          return bad("large ratio digits must be positive decimal integers");
        out += p.largeNum;
        out += '/';
        out += p.largeDen;
        break;
    }
    out += "\r\n";
  }
  return out;
}

// ---- .kbm ------------------------------------------------------------------------------------------------------

KeyboardMapping KeyboardMapping::linear(int32_t middleKey, int32_t referenceKey, double referenceHz) {
  KeyboardMapping k;
  k.mapSize = 0;
  k.firstKey = 0;
  k.lastKey = 127;
  k.middleKey = middleKey;
  k.referenceKey = referenceKey;
  k.referenceHz = referenceHz;
  k.formalOctaveDegree = 0;
  return k;
}

Result<KeyboardMapping> parseKbm(std::string_view text, std::string_view fileName) {
  const std::vector<Line> lines = splitLines(stripBom(text));
  size_t i = 0;
  auto nextValueLine = [&]() -> const Line* {
    while (i < lines.size() && (isComment(lines[i]) || isBlank(lines[i]))) ++i;
    return i < lines.size() ? &lines[i++] : nullptr;
  };
  const uint32_t lastLine = lines.empty() ? 0 : lines.back().number;

  KeyboardMapping k;
  struct IntField {
    int32_t* dst;
    const char* name;
  };
  const IntField first[] = {{&k.mapSize, "map size"},
                            {&k.firstKey, "first key to retune"},
                            {&k.lastKey, "last key to retune"},
                            {&k.middleKey, "middle key"},
                            {&k.referenceKey, "reference key"}};
  for (const IntField& f : first) {
    const Line* l = nextValueLine();
    if (!l) return parseError(std::string("missing ") + f.name, fileName, lastLine);
    const auto v = leadingInt32(l->text);
    if (!v) return parseError(std::string(f.name) + " must be an integer", fileName, l->number);
    *f.dst = *v;
  }
  {
    const Line* l = nextValueLine();
    if (!l) return parseError("missing reference frequency", fileName, lastLine);
    const auto v = leadingDouble(l->text);
    if (!v || !std::isfinite(*v) || !(*v > 0.0))
      return parseError("reference frequency must be a positive number", fileName, l->number);
    k.referenceHz = *v;
  }
  {
    const Line* l = nextValueLine();
    if (!l) return parseError("missing formal octave degree", fileName, lastLine);
    const auto v = leadingInt32(l->text);
    if (!v) return parseError("formal octave degree must be an integer", fileName, l->number);
    k.formalOctaveDegree = *v;
  }
  if (k.mapSize < 0) return parseError("map size must be ≥ 0", fileName, 0);
  if (k.firstKey > k.lastKey) return parseError("first key to retune is above the last key", fileName, 0);

  k.mapping.reserve(static_cast<size_t>(std::min<int64_t>(k.mapSize, static_cast<int64_t>(lines.size()))));
  while (static_cast<int32_t>(k.mapping.size()) < k.mapSize) {
    const Line* l = nextValueLine();
    if (!l) break;  // missing tail entries are unmapped
    const std::string_view t = trim(l->text);
    if (t.front() == 'x' || t.front() == 'X') {
      k.mapping.push_back(kUnmappedKey);
      continue;
    }
    const auto v = leadingInt32(t);
    if (!v) return parseError("mapping entry must be a scale degree or 'x'", fileName, l->number);
    k.mapping.push_back(*v);
  }
  if (referenceUnmapped(k)) return parseError("the reference key is unmapped", fileName, 0);
  return k;
}

Result<std::string> writeKbm(const KeyboardMapping& k) {
  auto bad = [](const char* why) { return Error{ErrorCode::InvalidArgument, why, {}}; };
  if (k.mapSize < 0) return bad("map size must be >= 0");
  if (k.mapping.size() > static_cast<size_t>(k.mapSize)) return bad("more mapping entries than the map size");
  if (k.firstKey > k.lastKey) return bad("first key to retune is above the last key");
  if (!std::isfinite(k.referenceHz) || !(k.referenceHz > 0.0)) return bad("reference frequency must be positive");
  if (referenceUnmapped(k)) return bad("the reference key is unmapped");

  auto line = [](std::string& out, const char* comment, const std::string& value) {
    out += comment;
    out += "\r\n";
    out += value;
    out += "\r\n";
  };
  std::string out;
  out += "! Scala keyboard mapping\r\n!\r\n";
  line(out, "! map size (pattern length in keys; 0 = linear)", std::to_string(k.mapSize));
  line(out, "! lowest key retuned", std::to_string(k.firstKey));
  line(out, "! highest key retuned", std::to_string(k.lastKey));
  line(out, "! key that plays degree 0", std::to_string(k.middleKey));
  line(out, "! reference key", std::to_string(k.referenceKey));
  line(out, "! reference key frequency in Hz", centsText(k.referenceHz));
  line(out, "! degrees per pattern repetition (formal octave)", std::to_string(k.formalOctaveDegree));
  out += "! degree for each key of the pattern (x = unmapped)\r\n";
  for (int32_t e : k.mapping) {
    out += e == kUnmappedKey ? std::string("x") : std::to_string(e);
    out += "\r\n";
  }
  return out;
}

std::optional<int64_t> mapKeyToDegree(const KeyboardMapping& k, int32_t key) noexcept {  // RT-safe
  if (key < k.firstKey || key > k.lastKey) return std::nullopt;
  const int64_t i = static_cast<int64_t>(key) - k.middleKey;
  if (k.mapSize <= 0) return i;
  int64_t q = i / k.mapSize;
  int64_t r = i % k.mapSize;
  if (r < 0) {
    r += k.mapSize;
    --q;
  }
  if (static_cast<size_t>(r) >= k.mapping.size()) return std::nullopt;
  const int32_t e = k.mapping[static_cast<size_t>(r)];
  if (e == kUnmappedKey) return std::nullopt;
  // |q| < 2^32 and |formalOctaveDegree| < 2^31, so the product and the sum fit int64.
  return static_cast<int64_t>(e) + q * static_cast<int64_t>(k.formalOctaveDegree);
}

std::optional<PitchId> mapKey(const KeyboardMapping& k, int32_t key, int32_t scaleSize) noexcept {  // RT-safe
  if (scaleSize < 1) return std::nullopt;
  const auto d = mapKeyToDegree(k, key);
  if (!d) return std::nullopt;
  int64_t q = *d / scaleSize;
  int64_t r = *d % scaleSize;
  if (r < 0) {
    r += scaleSize;
    --q;
  }
  if (q < std::numeric_limits<int32_t>::min() || q > std::numeric_limits<int32_t>::max()) return std::nullopt;
  return PitchId::fromDegree(static_cast<int32_t>(r), static_cast<int32_t>(q));
}

}  // namespace pq::tuning
