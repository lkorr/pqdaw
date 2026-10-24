#pragma once

// Tests only (T1.x-text-ids): edits of .pqp TEXT as a user makes them in a text editor — insert a line, add an
// export, rename a binding, reorder lines — for checking that a re-parse keeps NodeIds (id_match.h). Every edit is
// checked by its caller against the IR it should produce (the text helpers are deliberately simple: a rename that
// would hit a named argument is detected by that check, not avoided here).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace text_edits {

inline std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t b = 0;
  while (b <= text.size()) {
    const size_t e = text.find('\n', b);
    if (e == std::string::npos) {
      lines.push_back(text.substr(b));
      break;
    }
    lines.push_back(text.substr(b, e - b));
    b = e + 1;
  }
  return lines;
}

inline std::string joinLines(const std::vector<std::string>& lines) {
  std::string s;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) s += '\n';
    s += lines[i];
  }
  return s;
}

inline bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
inline bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Lexical state at the start of each line (and one past the last line).
struct LineState {
  int braces = 0, parens = 0;  // { } depth; ( and [ depth
  bool inComment = false;      // inside /* */
};

// Calls f(offset, length) for every identifier outside comments, strings and number literals (unit suffixes are
// part of the number) and fills per-line states.
template <class F>
void scan(const std::string& t, F&& f, std::vector<LineState>* states = nullptr) {
  LineState st;
  if (states) states->assign(1, st);
  size_t i = 0;
  while (i < t.size()) {
    const char c = t[i];
    if (c == '\n') {
      ++i;
      if (states) states->push_back(st);
      continue;
    }
    if (st.inComment) {
      if (c == '*' && i + 1 < t.size() && t[i + 1] == '/') {
        st.inComment = false;
        i += 2;
      } else {
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') {
      while (i < t.size() && t[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < t.size() && t[i + 1] == '*') {
      st.inComment = true;
      i += 2;
      continue;
    }
    if (c == '"') {
      ++i;
      while (i < t.size() && t[i] != '"' && t[i] != '\n') i += t[i] == '\\' ? 2 : 1;
      ++i;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      while (i < t.size() && (identChar(t[i]) || t[i] == '.' ||
                              ((t[i] == '+' || t[i] == '-') && (t[i - 1] == 'e' || t[i - 1] == 'E'))))
        ++i;
      continue;
    }
    if (identStart(c)) {
      const size_t b = i;
      while (i < t.size() && identChar(t[i])) ++i;
      f(b, i - b);
      continue;
    }
    if (c == '{') ++st.braces;
    if (c == '}') --st.braces;
    if (c == '(' || c == '[') ++st.parens;
    if (c == ')' || c == ']') --st.parens;
    ++i;
  }
  if (states) states->push_back(st);  // one past the last line
}

inline std::vector<LineState> lineStates(const std::string& t) {
  std::vector<LineState> s;
  scan(t, [](size_t, size_t) {}, &s);
  return s;
}

// The line that opens each patch (its `{` takes the brace depth from 0 to 1) and the line that closes it.
struct PatchLines {
  size_t open = 0, close = 0;
};
inline std::vector<PatchLines> patchLines(const std::vector<LineState>& st, size_t numLines) {
  std::vector<PatchLines> out;
  for (size_t l = 0; l < numLines; ++l) {
    if (st[l].braces == 0 && st[l + 1].braces >= 1) out.push_back({l, l});
    if (!out.empty() && st[l].braces >= 1 && st[l + 1].braces == 0) out.back().close = l;
  }
  return out;
}

// A complete one-line binding `name = …` / `node.input = …` directly in a patch body.
inline bool isBindingLine(const std::string& line, const LineState& begin, const LineState& end) {
  if (begin.braces != 1 || end.braces != 1 || begin.parens != 0 || end.parens != 0 || begin.inComment || end.inComment)
    return false;
  size_t i = 0;
  while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
  if (i >= line.size() || !identStart(line[i])) return false;
  while (i < line.size() && identChar(line[i])) ++i;
  if (i < line.size() && line[i] == '.') {
    ++i;
    if (i >= line.size() || !identStart(line[i])) return false;
    while (i < line.size() && identChar(line[i])) ++i;
  }
  while (i < line.size() && line[i] == ' ') ++i;
  if (!(i + 1 < line.size() && line[i] == '=' && line[i + 1] != '=')) return false;
  // Line breaks are insignificant: `x = a` followed by a line `+ b` is one binding over two lines.
  size_t e = line.size();
  while (e > 0 && std::isspace(static_cast<unsigned char>(line[e - 1]))) --e;
  return e > 0 && std::string_view("+-*/%&|^?:<>=!,.(").find(line[e - 1]) == std::string_view::npos;
}

inline bool startsWithOperator(const std::string& line) {
  size_t i = 0;
  while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
  return i < line.size() && std::string_view("+-*/%&|^?:<>=!,.").find(line[i]) != std::string_view::npos &&
         line.compare(i, 2, "//") != 0 && line.compare(i, 2, "/*") != 0;
}

// The one-line bindings of the patch body [open + 1, close) that no next line continues.
inline std::vector<size_t> bindingLines(const std::vector<std::string>& lines, const std::vector<LineState>& st,
                                        size_t open, size_t close) {
  std::vector<size_t> out;
  for (size_t l = open + 1; l < close; ++l) {
    if (!isBindingLine(lines[l], st[l], st[l + 1])) continue;
    size_t next = l + 1;
    while (next < close && lines[next].find_first_not_of(" \t\r") == std::string::npos) ++next;
    if (next < close && startsWithOperator(lines[next])) continue;
    out.push_back(l);
  }
  return out;
}

// Inserts `line` after the opening line of every patch.
inline std::string insertAtTop(const std::string& text, const std::string& line) {
  std::vector<std::string> lines = splitLines(text);
  const auto st = lineStates(text);
  const auto patches = patchLines(st, lines.size());
  for (size_t k = patches.size(); k-- > 0;) lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(patches[k].open + 1), line);
  return joinLines(lines);
}

// Inserts `line` as the last member of every patch: before its closing line, or before its `layout` block (which
// the grammar puts last).
inline std::string insertAtBottom(const std::string& text, const std::string& line) {
  std::vector<std::string> lines = splitLines(text);
  const auto st = lineStates(text);
  const auto patches = patchLines(st, lines.size());
  for (size_t k = patches.size(); k-- > 0;) {
    size_t at = patches[k].close;
    for (size_t l = patches[k].open + 1; l < patches[k].close; ++l)  // before a layout block
      if (st[l].braces == 1 && lines[l].find("layout") != std::string::npos && st[l + 1].braces == 2) {
        at = l;
        break;
      }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), line);
  }
  return joinLines(lines);
}

// Inserts `line` before the middle binding line of every patch (after the opening line if it has none).
inline std::string insertInMiddle(const std::string& text, const std::string& line) {
  std::vector<std::string> lines = splitLines(text);
  const auto st = lineStates(text);
  const auto patches = patchLines(st, lines.size());
  std::vector<size_t> at;
  for (const PatchLines& p : patches) {
    const std::vector<size_t> bind = bindingLines(lines, st, p.open, p.close);
    at.push_back(bind.empty() ? p.open + 1 : bind[bind.size() / 2]);
  }
  for (size_t k = at.size(); k-- > 0;) lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at[k]), line);
  return joinLines(lines);
}

// Shuffles the one-line bindings of every patch among their own lines (order does not matter in the language).
inline std::string shuffleBindings(const std::string& text, uint32_t seed) {
  std::vector<std::string> lines = splitLines(text);
  const auto st = lineStates(text);
  std::mt19937 rng(seed);
  for (const PatchLines& p : patchLines(st, lines.size())) {
    const std::vector<size_t> bind = bindingLines(lines, st, p.open, p.close);
    std::vector<std::string> moved;
    for (size_t l : bind) moved.push_back(lines[l]);
    std::shuffle(moved.begin(), moved.end(), rng);
    for (size_t k = 0; k < bind.size(); ++k) lines[bind[k]] = moved[k];
  }
  return joinLines(lines);
}

// Renames identifier `from` to `to` inside the line range [firstLine, lastLine] (a patch), except after a `.`
// (an output or member name).
inline std::string renameIdent(const std::string& text, std::string_view from, std::string_view to, size_t firstLine,
                               size_t lastLine) {
  std::vector<size_t> lineStart{0};
  for (size_t i = 0; i < text.size(); ++i)
    if (text[i] == '\n') lineStart.push_back(i + 1);
  const size_t lo = lineStart[std::min(firstLine, lineStart.size() - 1)];
  const size_t hi = lastLine + 1 < lineStart.size() ? lineStart[lastLine + 1] : text.size();
  std::vector<size_t> hits;
  scan(text, [&](size_t off, size_t len) {
    if (off < lo || off >= hi || std::string_view(text).substr(off, len) != from) return;
    size_t p = off;
    while (p > 0 && text[p - 1] == ' ') --p;
    if (p > 0 && text[p - 1] == '.') return;
    hits.push_back(off);
  });
  std::string out = text;
  for (size_t k = hits.size(); k-- > 0;) out.replace(hits[k], from.size(), to);
  return out;
}

}  // namespace text_edits
