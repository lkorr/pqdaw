#include "lexer.h"

#include <charconv>
#include <cstring>

namespace pq::graph::detail {

namespace {

constexpr bool isAlpha(char c) noexcept { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool isAlnum(char c) noexcept { return isAlpha(c) || isDigit(c); }

Tok keyword(std::string_view s) noexcept {
  switch (s.size()) {
    case 2:
      if (s == "in") return Tok::KwIn;
      break;
    case 3:
      if (s == "out") return Tok::KwOut;
      break;
    case 4:
      if (s == "true") return Tok::KwTrue;
      break;
    case 5:
      if (s == "patch") return Tok::KwPatch;
      if (s == "param") return Tok::KwParam;
      if (s == "solve") return Tok::KwSolve;
      if (s == "false") return Tok::KwFalse;
      break;
    case 6:
      if (s == "import") return Tok::KwImport;
      if (s == "export") return Tok::KwExport;
      if (s == "layout") return Tok::KwLayout;
      break;
    case 10:
      if (s == "iterations") return Tok::KwIterations;
      break;
    default: break;
  }
  return Tok::Ident;
}

// Length of the UTF-8 sequence starting with lead byte c (1 for invalid bytes, so lexing always advances).
uint32_t utf8Length(unsigned char c) noexcept {
  if (c >= 0xF0 && c <= 0xF4) return 4;
  if (c >= 0xE0) return c <= 0xEF ? 3 : 1;
  if (c >= 0xC2) return 2;
  return 1;
}

}  // namespace

std::string_view tokText(Tok t) noexcept {
  switch (t) {
    case Tok::End: return "end of file";
    case Tok::Ident: return "identifier";
    case Tok::Number: return "number";
    case Tok::String: return "string";
    case Tok::KwPatch: return "'patch'";
    case Tok::KwImport: return "'import'";
    case Tok::KwParam: return "'param'";
    case Tok::KwIn: return "'in'";
    case Tok::KwOut: return "'out'";
    case Tok::KwExport: return "'export'";
    case Tok::KwSolve: return "'solve'";
    case Tok::KwIterations: return "'iterations'";
    case Tok::KwLayout: return "'layout'";
    case Tok::KwTrue: return "'true'";
    case Tok::KwFalse: return "'false'";
    case Tok::LBrace: return "'{'";
    case Tok::RBrace: return "'}'";
    case Tok::LParen: return "'('";
    case Tok::RParen: return "')'";
    case Tok::LBracket: return "'['";
    case Tok::RBracket: return "']'";
    case Tok::Comma: return "','";
    case Tok::Colon: return "':'";
    case Tok::Semi: return "';'";
    case Tok::Assign: return "'='";
    case Tok::Eq: return "'=='";
    case Tok::Ne: return "'!='";
    case Tok::Lt: return "'<'";
    case Tok::Le: return "'<='";
    case Tok::Gt: return "'>'";
    case Tok::Ge: return "'>='";
    case Tok::Plus: return "'+'";
    case Tok::Minus: return "'-'";
    case Tok::Star: return "'*'";
    case Tok::Slash: return "'/'";
    case Tok::Percent: return "'%'";
    case Tok::Bang: return "'!'";
    case Tok::AndAnd: return "'&&'";
    case Tok::OrOr: return "'||'";
    case Tok::XorXor: return "'^^'";
    case Tok::Question: return "'?'";
    case Tok::Dot: return "'.'";
    case Tok::DotDot: return "'..'";
    case Tok::At: return "'@'";
    case Tok::Arrow: return "'->'";
  }
  return "token";
}

void lex(std::string_view src, std::vector<Token>& tokens, std::vector<LexError>& errors) {
  tokens.clear();
  tokens.reserve(src.size() / 3 + 8);  // typical .pqp text: one token per 3–5 bytes; one allocation in practice
  const char* const s = src.data();
  const auto n = static_cast<uint32_t>(src.size());
  uint32_t i = 0;
  bool lineStart = true;

  auto error = [&](uint32_t off, uint32_t len, std::string msg) { errors.push_back({off, len, std::move(msg)}); };
  auto push = [&](Tok k, uint32_t off, uint32_t len) -> Token& {
    Token t;
    t.kind = k;
    t.offset = off;
    t.length = len;
    if (lineStart) t.flags |= tok_flags::kLineStart;
    lineStart = false;
    tokens.push_back(t);
    return tokens.back();
  };

  while (i < n) {
    const char c = s[i];
    // Whitespace and comments.
    if (c == '\n') {
      lineStart = true;
      ++i;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '/') {
      while (i < n && s[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      const uint32_t start = i;
      i += 2;
      while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
        if (s[i] == '\n') lineStart = true;
        ++i;
      }
      if (i + 1 >= n) {
        error(start, 2, "unterminated comment: '/*' has no matching '*/'");
        i = n;
      } else {
        i += 2;
      }
      continue;
    }

    const uint32_t start = i;
    if (isAlpha(c)) {
      while (i < n && isAlnum(s[i])) ++i;
      push(keyword(src.substr(start, i - start)), start, i - start);
      continue;
    }

    if (isDigit(c)) {
      bool integer = true;
      while (i < n && isDigit(s[i])) ++i;
      // A fraction needs a digit after the '.', so `0..20` lexes as 0 '..' 20.
      if (i + 1 < n && s[i] == '.' && isDigit(s[i + 1])) {
        integer = false;
        ++i;
        while (i < n && isDigit(s[i])) ++i;
      }
      if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        uint32_t j = i + 1;
        if (j < n && (s[j] == '+' || s[j] == '-')) ++j;
        if (j < n && isDigit(s[j])) {
          integer = false;
          i = j;
          while (i < n && isDigit(s[i])) ++i;
        }
      }
      const uint32_t numEnd = i;
      double value = 0.0;
      const auto [ptr, ec] = std::from_chars(s + start, s + numEnd, value, std::chars_format::general);
      bool bad = false;
      if (ec != std::errc{} || ptr != s + numEnd) {
        error(start, numEnd - start, "number '" + std::string(src.substr(start, numEnd - start)) + "' is out of range");
        bad = true;
        value = 0.0;
      }
      Unit unit = Unit::None;
      // A unit suffix follows with no space (PATCH_LANGUAGE §1); `%` directly after a number is the percent unit.
      if (i < n && s[i] == '%') {
        unit = Unit::percent;
        ++i;
      } else if (i < n && isAlpha(s[i])) {
        const uint32_t u = i;
        while (i < n && isAlnum(s[i])) ++i;
        const std::string_view suffix = src.substr(u, i - u);
        if (!unitFromSuffix(suffix, unit)) {
          if (!bad)
            error(u, i - u, "unknown unit suffix '" + std::string(suffix) +
                                "' (units: Hz kHz ms s smp dB c st %)");
          unit = Unit::None;
        }
      }
      Token& t = push(Tok::Number, start, i - start);
      t.number = value;
      t.unit = unit;
      if (integer && unit == Unit::None && i == numEnd) t.flags |= tok_flags::kInteger;
      continue;
    }

    if (c == '"') {
      ++i;
      bool closed = false;
      while (i < n && s[i] != '\n') {
        if (s[i] == '\\' && i + 1 < n && (s[i + 1] == '"' || s[i + 1] == '\\')) {
          i += 2;
          continue;
        }
        if (s[i] == '\\') {
          error(i, i + 1 < n ? 2 : 1, "unknown escape in string (only \\\" and \\\\ are allowed)");
          ++i;
          continue;
        }
        if (s[i] == '"') {
          closed = true;
          ++i;
          break;
        }
        ++i;
      }
      if (!closed) error(start, 1, "unterminated string");
      push(Tok::String, start, i - start);
      continue;
    }

    auto two = [&](char next) { return i + 1 < n && s[i + 1] == next; };
    Tok k = Tok::End;
    uint32_t len = 1;
    switch (c) {
      case '{': k = Tok::LBrace; break;
      case '}': k = Tok::RBrace; break;
      case '(': k = Tok::LParen; break;
      case ')': k = Tok::RParen; break;
      case '[': k = Tok::LBracket; break;
      case ']': k = Tok::RBracket; break;
      case ',': k = Tok::Comma; break;
      case ':': k = Tok::Colon; break;
      case ';': k = Tok::Semi; break;
      case '+': k = Tok::Plus; break;
      case '*': k = Tok::Star; break;
      case '/': k = Tok::Slash; break;
      case '%': k = Tok::Percent; break;
      case '?': k = Tok::Question; break;
      case '@': k = Tok::At; break;
      case '=': k = two('=') ? (len = 2, Tok::Eq) : Tok::Assign; break;
      case '!': k = two('=') ? (len = 2, Tok::Ne) : Tok::Bang; break;
      case '<': k = two('=') ? (len = 2, Tok::Le) : Tok::Lt; break;
      case '>': k = two('=') ? (len = 2, Tok::Ge) : Tok::Gt; break;
      case '-': k = two('>') ? (len = 2, Tok::Arrow) : Tok::Minus; break;
      case '.': k = two('.') ? (len = 2, Tok::DotDot) : Tok::Dot; break;
      case '&':
        if (two('&')) k = Tok::AndAnd, len = 2;
        break;
      case '|':
        if (two('|')) k = Tok::OrOr, len = 2;
        break;
      case '^':
        if (two('^')) k = Tok::XorXor, len = 2;
        break;
      default: break;
    }
    if (k == Tok::End) {
      const uint32_t clen = utf8Length(static_cast<unsigned char>(c));
      const uint32_t l = start + clen <= n ? clen : n - start;
      std::string msg = "unexpected character";
      if (c == '&' || c == '|' || c == '^') msg += std::string(" '") + c + "' (did you mean '" + c + c + "'?)";
      else if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F) msg += std::string(" '") + c + "'";
      error(start, l, std::move(msg));
      i += l;
      continue;
    }
    push(k, start, len);
    i += len;
  }
  Token& end = push(Tok::End, n, 0);
  (void)end;
}

void decodeString(std::string_view text, std::string& out) {
  out.clear();
  if (text.size() < 2) return;
  const bool closed = text.size() >= 2 && text.back() == '"';
  const std::string_view body = text.substr(1, text.size() - (closed ? 2 : 1));
  out.reserve(body.size());
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] == '\\' && i + 1 < body.size() && (body[i + 1] == '"' || body[i + 1] == '\\')) {
      out += body[++i];
      continue;
    }
    out += body[i];
  }
}

}  // namespace pq::graph::detail
