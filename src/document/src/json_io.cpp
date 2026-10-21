#include "json_io.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <system_error>
#include <vector>

namespace pq::doc::json {

namespace {

constexpr int kMaxSkipDepth = 256;  // nesting accepted by skipValue (the schema itself is far shallower)

bool isWs(char c) noexcept { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

int hexVal(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Length of the valid UTF-8 sequence starting at s[i] (a non-ASCII lead byte), or 0 if it is not valid.
size_t utf8SeqLen(std::string_view s, size_t i) noexcept {
  const auto c = static_cast<unsigned char>(s[i]);
  size_t len = 0;
  uint32_t cp = 0;
  if ((c & 0xE0) == 0xC0) {
    len = 2;
    cp = c & 0x1Fu;
  } else if ((c & 0xF0) == 0xE0) {
    len = 3;
    cp = c & 0x0Fu;
  } else if ((c & 0xF8) == 0xF0) {
    len = 4;
    cp = c & 0x07u;
  } else {
    return 0;
  }
  if (i + len > s.size()) return 0;
  for (size_t k = 1; k < len; ++k) {
    const auto cc = static_cast<unsigned char>(s[i + k]);
    if ((cc & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (cc & 0x3Fu);
  }
  if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
      (cp >= 0xD800 && cp <= 0xDFFF))
    return 0;
  return len;
}

}  // namespace

bool validUtf8(std::string_view s) noexcept {
  size_t i = 0;
  while (i < s.size()) {
    if (static_cast<unsigned char>(s[i]) < 0x80) {
      ++i;
      continue;
    }
    const size_t n = utf8SeqLen(s, i);
    if (n == 0) return false;
    i += n;
  }
  return true;
}

std::string renderPath(const PathSeg& at) {
  const PathSeg* segs[256];
  size_t n = 0;
  for (const PathSeg* p = &at; p && p->parent && n < 256; p = p->parent) segs[n++] = p;
  std::string out = "$";
  while (n > 0) {
    const PathSeg* p = segs[--n];
    if (!p->key.empty()) {
      out += '.';
      out += p->key;
    } else {
      out += '[';
      out += std::to_string(p->index);
      out += ']';
    }
  }
  return out;
}

void fail(const PathSeg& at, std::string message, ErrorCode code, size_t pos) {
  throw JsonFail{code, renderPath(at), std::move(message), pos};
}

// ---- Reader ------------------------------------------------------------------------------------------------------

void Reader::ws() noexcept {
  while (p_ < s_.size() && isWs(s_[p_])) ++p_;
}

void Reader::failValue(const PathSeg& at, std::string message, ErrorCode code) const {
  fail(at, std::move(message), code, valueStart_);
}

void Reader::failHere(const PathSeg& at, std::string message, ErrorCode code) const {
  fail(at, std::move(message), code, p_);
}

Reader::Kind Reader::peek() {
  ws();
  if (p_ >= s_.size()) return Kind::End;
  switch (s_[p_]) {
    case '{': return Kind::Object;
    case '[': return Kind::Array;
    case '"': return Kind::String;
    case 't':
    case 'f': return Kind::Bool;
    case 'n': return Kind::Null;
    default: return (s_[p_] == '-' || isDigit(s_[p_])) ? Kind::Number : Kind::Invalid;
  }
}

namespace {
const char* kindName(Reader::Kind k) {
  switch (k) {
    case Reader::Kind::Object: return "an object";
    case Reader::Kind::Array: return "an array";
    case Reader::Kind::String: return "a string";
    case Reader::Kind::Number: return "a number";
    case Reader::Kind::Bool: return "a boolean";
    case Reader::Kind::Null: return "null";
    case Reader::Kind::End: return "the end of the file";
    case Reader::Kind::Invalid: break;
  }
  return "an invalid character";
}
}  // namespace

void Reader::expect(const PathSeg& at, char c, const char* what) {
  const Kind k = peek();
  valueStart_ = p_;
  if (p_ >= s_.size() || s_[p_] != c) {
    if (k == Kind::End || k == Kind::Invalid) failHere(at, std::string("expected ") + what + ", found " + kindName(k));
    failHere(at, std::string("expected ") + what + ", found " + kindName(k), ErrorCode::Type);
  }
  ++p_;
}

void Reader::beginObject(const PathSeg& at) {
  expect(at, '{', "an object");
  afterOpen_ = true;
}

void Reader::beginArray(const PathSeg& at) {
  expect(at, '[', "an array");
  afterOpen_ = true;
}

bool Reader::nextKey(const PathSeg& at, std::string_view& key) {
  ws();
  if (p_ < s_.size() && s_[p_] == '}') {
    ++p_;
    afterOpen_ = false;
    return false;
  }
  if (!afterOpen_) {
    if (p_ >= s_.size() || s_[p_] != ',') failHere(at, "expected ',' or '}' after an object member");
    ++p_;
    ws();
  }
  afterOpen_ = false;
  if (p_ < s_.size() && s_[p_] == '}') failHere(at, "trailing comma in object");
  if (p_ >= s_.size() || s_[p_] != '"') failHere(at, "expected a key (a string in double quotes)");
  key = scanString(at, keyScratch_);
  ws();
  if (p_ >= s_.size() || s_[p_] != ':') failHere(at, "expected ':' after a key");
  ++p_;
  return true;
}

bool Reader::nextElement(const PathSeg& at) {
  ws();
  if (p_ < s_.size() && s_[p_] == ']') {
    ++p_;
    afterOpen_ = false;
    return false;
  }
  if (!afterOpen_) {
    if (p_ >= s_.size() || s_[p_] != ',') failHere(at, "expected ',' or ']' after an array element");
    ++p_;
    ws();
    if (p_ < s_.size() && s_[p_] == ']') failHere(at, "trailing comma in array");
  }
  afterOpen_ = false;
  return true;
}

std::string_view Reader::scanString(const PathSeg& at, std::string& scratch) {
  // s_[p_] == '"'
  const size_t start = ++p_;
  // Fast path: plain ASCII without escapes.
  size_t i = start;
  while (i < s_.size()) {
    const auto c = static_cast<unsigned char>(s_[i]);
    if (c == '"' || c == '\\' || c < 0x20 || c >= 0x80) break;
    ++i;
  }
  if (i < s_.size() && s_[i] == '"') {
    p_ = i + 1;
    return s_.substr(start, i - start);
  }
  scratch.assign(s_.data() + start, i - start);
  while (true) {
    if (i >= s_.size()) {
      p_ = i;
      failHere(at, "unterminated string");
    }
    const auto c = static_cast<unsigned char>(s_[i]);
    if (c == '"') {
      p_ = i + 1;
      return scratch;
    }
    if (c < 0x20) {
      p_ = i;
      failHere(at, "control character in a string (must be escaped)");
    }
    if (c >= 0x80) {
      const size_t n = utf8SeqLen(s_, i);
      if (n == 0) {
        p_ = i;
        failHere(at, "invalid UTF-8 in a string");
      }
      scratch.append(s_.data() + i, n);
      i += n;
      continue;
    }
    if (c != '\\') {
      scratch += static_cast<char>(c);
      ++i;
      continue;
    }
    if (i + 1 >= s_.size()) {
      p_ = i;
      failHere(at, "unterminated string");
    }
    const char e = s_[i + 1];
    i += 2;
    switch (e) {
      case '"': scratch += '"'; break;
      case '\\': scratch += '\\'; break;
      case '/': scratch += '/'; break;
      case 'b': scratch += '\b'; break;
      case 'f': scratch += '\f'; break;
      case 'n': scratch += '\n'; break;
      case 'r': scratch += '\r'; break;
      case 't': scratch += '\t'; break;
      case 'u': {
        auto hex4 = [&](size_t at4, uint32_t& v) {
          if (at4 + 4 > s_.size()) return false;
          v = 0;
          for (size_t k = 0; k < 4; ++k) {
            const int h = hexVal(s_[at4 + k]);
            if (h < 0) return false;
            v = (v << 4) | static_cast<uint32_t>(h);
          }
          return true;
        };
        uint32_t cp = 0;
        if (!hex4(i, cp)) {
          p_ = i;
          failHere(at, "invalid \\u escape");
        }
        i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          uint32_t lo = 0;
          if (i + 1 < s_.size() && s_[i] == '\\' && s_[i + 1] == 'u' && hex4(i + 2, lo) && lo >= 0xDC00 &&
              lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 6;
          } else {
            p_ = i;
            failHere(at, "unpaired surrogate in a \\u escape");
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          p_ = i;
          failHere(at, "unpaired surrogate in a \\u escape");
        }
        appendUtf8(scratch, cp);
        break;
      }
      default:
        p_ = i - 1;
        failHere(at, "invalid escape in a string");
    }
  }
}

std::string_view Reader::scanNumber(const PathSeg& at, bool& isInt) {
  const size_t start = p_;
  size_t i = p_;
  if (i < s_.size() && s_[i] == '-') ++i;
  if (i < s_.size() && s_[i] == '0') {
    ++i;
  } else if (i < s_.size() && isDigit(s_[i])) {
    while (i < s_.size() && isDigit(s_[i])) ++i;
  } else {
    p_ = i;
    failHere(at, "invalid number");
  }
  isInt = true;
  if (i < s_.size() && s_[i] == '.') {
    isInt = false;
    ++i;
    if (i >= s_.size() || !isDigit(s_[i])) {
      p_ = i;
      failHere(at, "invalid number (digits must follow '.')");
    }
    while (i < s_.size() && isDigit(s_[i])) ++i;
  }
  if (i < s_.size() && (s_[i] == 'e' || s_[i] == 'E')) {
    isInt = false;
    ++i;
    if (i < s_.size() && (s_[i] == '+' || s_[i] == '-')) ++i;
    if (i >= s_.size() || !isDigit(s_[i])) {
      p_ = i;
      failHere(at, "invalid number (digits must follow the exponent)");
    }
    while (i < s_.size() && isDigit(s_[i])) ++i;
  }
  if (i < s_.size() && (isDigit(s_[i]) || s_[i] == '.' || s_[i] == '+' || s_[i] == '-' || s_[i] == 'e' ||
                        s_[i] == 'E')) {
    p_ = i;
    failHere(at, "invalid number");  // e.g. a leading zero ("01")
  }
  p_ = i;
  return s_.substr(start, i - start);
}

void Reader::readString(const PathSeg& at, std::string& out) {
  const Kind k = peek();
  valueStart_ = p_;
  if (k != Kind::String) {
    if (k == Kind::End || k == Kind::Invalid) failHere(at, std::string("expected a string, found ") + kindName(k));
    failHere(at, std::string("expected a string, found ") + kindName(k), ErrorCode::Type);
  }
  const std::string_view v = scanString(at, out);
  if (v.data() != out.data()) out.assign(v.data(), v.size());
}

int64_t Reader::readInt(const PathSeg& at) {
  const Kind k = peek();
  valueStart_ = p_;
  if (k != Kind::Number) {
    if (k == Kind::End || k == Kind::Invalid) failHere(at, std::string("expected an integer, found ") + kindName(k));
    failHere(at, std::string("expected an integer, found ") + kindName(k), ErrorCode::Type);
  }
  bool isInt = false;
  const std::string_view t = scanNumber(at, isInt);
  if (!isInt) failValue(at, "expected an integer, found a number with a fraction or exponent", ErrorCode::Type);
  int64_t v = 0;
  const auto r = std::from_chars(t.data(), t.data() + t.size(), v);
  if (r.ec != std::errc{}) failValue(at, "integer out of range");
  return v;
}

double Reader::readDouble(const PathSeg& at) {
  const Kind k = peek();
  valueStart_ = p_;
  if (k != Kind::Number) {
    if (k == Kind::End || k == Kind::Invalid) failHere(at, std::string("expected a number, found ") + kindName(k));
    failHere(at, std::string("expected a number, found ") + kindName(k), ErrorCode::Type);
  }
  bool isInt = false;
  const std::string_view t = scanNumber(at, isInt);
  double v = 0.0;
  const auto r = std::from_chars(t.data(), t.data() + t.size(), v);
  if (r.ec != std::errc{} || !std::isfinite(v)) failValue(at, "number out of range");
  return v;
}

float Reader::readFloat(const PathSeg& at) {
  const Kind k = peek();
  valueStart_ = p_;
  if (k != Kind::Number) {
    if (k == Kind::End || k == Kind::Invalid) failHere(at, std::string("expected a number, found ") + kindName(k));
    failHere(at, std::string("expected a number, found ") + kindName(k), ErrorCode::Type);
  }
  bool isInt = false;
  const std::string_view t = scanNumber(at, isInt);
  float v = 0.0f;
  const auto r = std::from_chars(t.data(), t.data() + t.size(), v);
  if (r.ec != std::errc{} || !std::isfinite(v)) failValue(at, "number out of range");
  return v;
}

bool Reader::readBool(const PathSeg& at) {
  const Kind k = peek();
  valueStart_ = p_;
  if (s_.substr(p_, 4) == "true") {
    p_ += 4;
  } else if (s_.substr(p_, 5) == "false") {
    p_ += 5;
  } else {
    if (k == Kind::Bool || k == Kind::End || k == Kind::Invalid)
      failHere(at, std::string("expected true or false"));
    failHere(at, std::string("expected a boolean, found ") + kindName(k), ErrorCode::Type);
  }
  if (p_ < s_.size() && std::isalnum(static_cast<unsigned char>(s_[p_]))) failValue(at, "invalid literal");
  return s_[valueStart_] == 't';
}

void Reader::readNull(const PathSeg& at) {
  const Kind k = peek();
  valueStart_ = p_;
  if (s_.substr(p_, 4) != "null") {
    if (k == Kind::Null || k == Kind::End || k == Kind::Invalid) failHere(at, "expected null");
    failHere(at, std::string("expected null, found ") + kindName(k), ErrorCode::Type);
  }
  p_ += 4;
  if (p_ < s_.size() && std::isalnum(static_cast<unsigned char>(s_[p_]))) failValue(at, "invalid literal");
}

void Reader::skip(const PathSeg& at, int depth) {
  if (depth > kMaxSkipDepth) failHere(at, "nesting too deep");
  switch (peek()) {
    case Kind::Object: {
      beginObject(at);
      std::string_view k;
      while (nextKey(at, k)) skip(at, depth + 1);
      return;
    }
    case Kind::Array: {
      beginArray(at);
      while (nextElement(at)) skip(at, depth + 1);
      return;
    }
    case Kind::String: {
      std::string tmp;
      readString(at, tmp);
      return;
    }
    case Kind::Number: {
      bool isInt = false;
      valueStart_ = p_;
      (void)scanNumber(at, isInt);
      return;
    }
    case Kind::Bool: (void)readBool(at); return;
    case Kind::Null: readNull(at); return;
    case Kind::End: failHere(at, "unexpected end of file");
    case Kind::Invalid: break;
  }
  failHere(at, "unexpected character");
}

void Reader::skipValue(const PathSeg& at) { skip(at, 0); }

void Reader::expectEnd(const PathSeg& at) {
  ws();
  if (p_ != s_.size()) failHere(at, "unexpected content after the end of the document");
}

// ---- Writer ------------------------------------------------------------------------------------------------------

void Writer::indent(size_t depth) {
  out_ += '\n';
  out_.append(depth * 2, ' ');
}

void Writer::separator(Level& l) {
  if (!l.first) out_ += ',';
  if (l.multiline)
    indent(stack_.size());
  else if (!l.first)
    out_ += ' ';
  l.first = false;
}

void Writer::element() {
  if (!stack_.empty() && stack_.back().array) separator(stack_.back());
}

void Writer::beginObject(bool multiline) {
  element();
  out_ += '{';
  stack_.push_back({multiline, true, false});
}

void Writer::endObject() {
  const Level l = stack_.back();
  stack_.pop_back();
  if (l.multiline && !l.first) indent(stack_.size());
  out_ += '}';
}

void Writer::beginArray(bool multiline) {
  element();
  out_ += '[';
  stack_.push_back({multiline, true, true});
}

void Writer::endArray() {
  const Level l = stack_.back();
  stack_.pop_back();
  if (l.multiline && !l.first) indent(stack_.size());
  out_ += ']';
}

void Writer::key(std::string_view k) {
  separator(stack_.back());
  out_ += '"';
  out_ += k;
  out_ += "\": ";
}

void Writer::string(const PathSeg& at, std::string_view v) {
  element();
  out_ += '"';
  size_t i = 0;
  while (i < v.size()) {
    const auto c = static_cast<unsigned char>(v[i]);
    if (c >= 0x80) {
      const size_t n = utf8SeqLen(v, i);
      if (n == 0) fail(at, "string is not valid UTF-8");
      out_.append(v.data() + i, n);
      i += n;
      continue;
    }
    switch (c) {
      case '"': out_ += "\\\""; break;
      case '\\': out_ += "\\\\"; break;
      case '\n': out_ += "\\n"; break;
      case '\r': out_ += "\\r"; break;
      case '\t': out_ += "\\t"; break;
      case '\b': out_ += "\\b"; break;
      case '\f': out_ += "\\f"; break;
      default:
        if (c < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          out_ += "\\u00";
          out_ += kHex[c >> 4];
          out_ += kHex[c & 15];
        } else {
          out_ += static_cast<char>(c);
        }
    }
    ++i;
  }
  out_ += '"';
}

void Writer::integer(int64_t v) {
  element();
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  out_.append(buf, r.ptr);
}

namespace {
template <class T>
void appendShortest(std::string& out, T v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);  // shortest round-trip representation
  const std::string_view t(buf, static_cast<size_t>(r.ptr - buf));
  out += t;
  // Keep it a JSON number with a fraction, so it reads back as floating point (and -0.0 keeps its sign).
  if (t.find_first_of(".e") == std::string_view::npos) out += ".0";
}
}  // namespace

void Writer::number(const PathSeg& at, double v) {
  if (!std::isfinite(v)) fail(at, "number is not finite (NaN and infinities cannot be stored)");
  element();
  appendShortest(out_, v);
}

void Writer::number(const PathSeg& at, float v) {
  if (!std::isfinite(v)) fail(at, "number is not finite (NaN and infinities cannot be stored)");
  element();
  appendShortest(out_, v);
}

void Writer::boolean(bool v) {
  element();
  out_ += v ? "true" : "false";
}

void Writer::null() {
  element();
  out_ += "null";
}

}  // namespace pq::doc::json
