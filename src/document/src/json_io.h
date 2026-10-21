#pragma once

// A specialised JSON pull reader and streaming writer for project files (T3.5). Private to pq_doc.
//
// Why not nlohmann/json here (pq_bench_serialize, release, 14 MB / 100k-note project): its SAX parser with a no-op
// handler takes 114–133 ms and its DOM parse 239–373 ms (over the whole 250 ms load budget), while this reader loads,
// validates and builds the snapshot in ~54–65 ms. It decodes straight into the model (no DOM), validates UTF-8 and the
// JSON grammar, and converts numbers with std::from_chars (exact; floats parsed as float, no double rounding). Errors are
// thrown as JsonFail (exceptions are allowed at parsing boundaries, ARCHITECTURE §6) and caught by
// parseProject/serializeProject, which turn them into pq::Error.
//
// JSON paths: decoders pass a PathSeg chain on the stack (no allocation); it is rendered ("$.tracks[0].name") only
// when an error is reported.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"

namespace pq::doc::json {

// One step of a JSON path. The root has parent == nullptr. A segment is either an object key or an array index.
struct PathSeg {
  const PathSeg* parent = nullptr;
  std::string_view key;  // empty = array index
  size_t index = 0;

  PathSeg() = default;
  PathSeg(const PathSeg* p, std::string_view k) : parent(p), key(k) {}
  PathSeg(const PathSeg* p, size_t i) : parent(p), index(i) {}
};
std::string renderPath(const PathSeg& at);

// What a decoder or encoder throws. pos = byte offset in the text (npos = unknown).
struct JsonFail {
  ErrorCode code = ErrorCode::Parse;
  std::string path;
  std::string message;
  size_t pos = std::string_view::npos;
};
[[noreturn]] void fail(const PathSeg& at, std::string message, ErrorCode code = ErrorCode::InvalidArgument,
                       size_t pos = std::string_view::npos);

class Reader {
public:
  enum class Kind : uint8_t { Object, Array, String, Number, Bool, Null, End, Invalid };

  explicit Reader(std::string_view text) noexcept : s_(text) {}

  size_t pos() const noexcept { return p_; }
  // Byte offset where the most recently read value started (for errors about its value).
  size_t valueStart() const noexcept { return valueStart_; }

  Kind peek();  // skips whitespace; does not consume
  void beginObject(const PathSeg& at);
  // Next member key of the object being read, or false at its '}' (consumed). The view is valid until the next
  // read call.
  bool nextKey(const PathSeg& at, std::string_view& key);
  void beginArray(const PathSeg& at);
  // True if another element follows (the reader is positioned on it), false at ']' (consumed).
  bool nextElement(const PathSeg& at);

  void readString(const PathSeg& at, std::string& out);
  int64_t readInt(const PathSeg& at);
  double readDouble(const PathSeg& at);  // integers are accepted (converted exactly as from_chars would)
  float readFloat(const PathSeg& at);
  bool readBool(const PathSeg& at);
  void readNull(const PathSeg& at);
  void skipValue(const PathSeg& at);
  // Only whitespace may follow the top-level value.
  void expectEnd(const PathSeg& at);

  // Throws with the given message at the start of the last value read.
  [[noreturn]] void failValue(const PathSeg& at, std::string message, ErrorCode code = ErrorCode::InvalidArgument) const;
  [[noreturn]] void failHere(const PathSeg& at, std::string message, ErrorCode code = ErrorCode::Parse) const;

private:
  void ws() noexcept;
  void expect(const PathSeg& at, char c, const char* what);
  // Scans a string starting at '"'. Returns a view into the text when it has no escapes, otherwise decodes into
  // `scratch` and returns a view of it.
  std::string_view scanString(const PathSeg& at, std::string& scratch);
  // Scans a number token; sets isInt (no fraction, no exponent).
  std::string_view scanNumber(const PathSeg& at, bool& isInt);
  void skip(const PathSeg& at, int depth);

  std::string_view s_;
  size_t p_ = 0;
  size_t valueStart_ = 0;
  bool afterOpen_ = false;  // just consumed '{' or '[' (no comma before the first member/element)
  std::string keyScratch_;
};

// Streaming writer with fixed formatting: containers are either multi-line (one member per line, 2-space indent)
// or inline (`{"a": 1, "b": 2}`). Keys are trusted ASCII constants.
class Writer {
public:
  explicit Writer(std::string& out) : out_(out) { stack_.reserve(80); }

  void beginObject(bool multiline);
  void endObject();
  void beginArray(bool multiline);
  void endArray();
  void key(std::string_view k);

  // Values (in an array they are separated automatically; in an object call key() first).
  void string(const PathSeg& at, std::string_view v);  // throws for invalid UTF-8
  void integer(int64_t v);
  void number(const PathSeg& at, double v);  // shortest round trip, always with '.' or exponent; throws if not finite
  void number(const PathSeg& at, float v);
  void boolean(bool v);
  void null();

private:
  struct Level {
    bool multiline;
    bool first;
    bool array;
  };
  void element();  // separator before an array element
  void separator(Level& l);
  void indent(size_t depth);

  std::string& out_;
  std::vector<Level> stack_;
};

bool validUtf8(std::string_view s) noexcept;

}  // namespace pq::doc::json
