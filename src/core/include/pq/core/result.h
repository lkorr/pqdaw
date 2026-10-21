#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace pq {

// Where an error came from (a source file being parsed, not the C++ source). line/col are 1-based; 0 = unknown.
struct SourceLocation {
  std::string file;
  uint32_t line = 0;
  uint32_t col = 0;
};

// Error codes are coarse categories; the message carries the detail.
enum class ErrorCode : uint32_t {
  Unknown,
  InvalidArgument,
  Parse,
  Type,
  Overflow,
  NotFound,
  Io,
  Device,
  Unsupported,
};

struct Error {
  ErrorCode code = ErrorCode::Unknown;
  std::string message;
  SourceLocation location;
};

// Value or Error, for non-RT paths (ARCHITECTURE §6). Not for the audio thread: Error owns strings.
template <class T>
class [[nodiscard]] Result {
public:
  Result(T value) : v_(std::move(value)) {}
  Result(Error error) : v_(std::move(error)) {}

  bool ok() const noexcept { return v_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  T& value() & { assert(ok()); return std::get<0>(v_); }
  const T& value() const& { assert(ok()); return std::get<0>(v_); }
  T&& value() && { assert(ok()); return std::get<0>(std::move(v_)); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }

  const Error& error() const& { assert(!ok()); return std::get<1>(v_); }
  Error&& error() && { assert(!ok()); return std::get<1>(std::move(v_)); }

private:
  std::variant<T, Error> v_;
};

// Result<void> equivalent.
struct Ok {};
using Status = Result<Ok>;

}  // namespace pq
