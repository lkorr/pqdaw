#pragma once

// Lexer of the patch language (PATCH_LANGUAGE §1). Private to pq_graph.
//
// The whole file is tokenised up front into one vector (one allocation, reserved from the source size): tokens are
// 24 bytes and refer to the source by offset, so there is no per-token allocation, and the parser gets arbitrary
// lookahead (`in x :` needs two tokens). Numbers are converted once here (std::from_chars, correctly rounded).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pq/graph/types.h"

namespace pq::graph::detail {

enum class Tok : uint8_t {
  End,
  Ident,
  Number,
  String,
  // Keywords (PATCH_LANGUAGE §1).
  KwPatch,
  KwImport,
  KwParam,
  KwIn,
  KwOut,
  KwExport,
  KwSolve,
  KwIterations,
  KwLayout,
  KwTrue,
  KwFalse,
  // Punctuation.
  LBrace,
  RBrace,
  LParen,
  RParen,
  LBracket,
  RBracket,
  Comma,
  Colon,
  Semi,
  Assign,    // =
  Eq,        // ==
  Ne,        // !=
  Lt,        // <
  Le,        // <=
  Gt,        // >
  Ge,        // >=
  Plus,      // +
  Minus,     // -
  Star,      // *
  Slash,     // /
  Percent,   // % (the operator; directly after a number it is the percent unit)
  Bang,      // !
  AndAnd,    // &&
  OrOr,      // ||
  XorXor,    // ^^
  Question,  // ?
  Dot,       // .
  DotDot,    // ..
  At,        // @
  Arrow,     // ->
};

std::string_view tokText(Tok t) noexcept;  // for messages: "identifier", "'{'", "'patch'", …

namespace tok_flags {
inline constexpr uint8_t kLineStart = 1;  // first token on its line (used for error recovery and port-type ends)
inline constexpr uint8_t kInteger = 2;    // Number: only digits, no fraction, exponent or unit
}  // namespace tok_flags

struct Token {
  Tok kind = Tok::End;
  Unit unit = Unit::None;  // Number
  uint8_t flags = 0;
  uint32_t offset = 0;
  uint32_t length = 0;
  double number = 0.0;  // Number: the literal as written, before unit normalisation

  bool lineStart() const noexcept { return (flags & tok_flags::kLineStart) != 0; }
  bool integer() const noexcept { return (flags & tok_flags::kInteger) != 0; }
};

struct LexError {
  uint32_t offset = 0;
  uint32_t length = 0;
  std::string message;
};

// Tokenises `src`; always ends with a Tok::End token at src.size(). Lexing continues after an error (the bad
// character is skipped, a bad number still yields a Number token), so the parser can report further problems.
void lex(std::string_view src, std::vector<Token>& tokens, std::vector<LexError>& errors);

// Decodes a String token (with its quotes) into `out`: `\"` and `\\` escapes.
void decodeString(std::string_view tokenText, std::string& out);

// Keyword tokens are names in some positions (`in`/`out`, Q7; any keyword after `.`, Q8).
inline bool isKeywordTok(Tok t) noexcept { return t >= Tok::KwPatch && t <= Tok::KwFalse; }

}  // namespace pq::graph::detail
