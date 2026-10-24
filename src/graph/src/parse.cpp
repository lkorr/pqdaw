// Parser of the patch language (docs/PATCH_LANGUAGE.md v0), T1.2.
//
// Pipeline: lex (lexer.cpp) → syntax (recursive descent into a flat AST + per-patch member lists) → lowering (AST →
// Patch IR, name resolution) → PatchFile::validateAll() with errors mapped back to source spans.
//
// Lowering is order-independent (PATCH_LANGUAGE §3: bindings are a dataflow graph): per patch,
//   1. declarations become nodes (param/in/out/export/unknown/equation) and register their names;
//   2. every binding `name = rhs` registers its name; then its rhs's top node (the "head") is created and named,
//      unless the rhs is just another name, which makes `name` an alias with no IR form;
//   3. aliases are resolved (iteratively, cycles are errors);
//   4. `node.input = rhs` bindings are attached;
//   5. a work list materialises every remaining sub-expression and wires it to the input it feeds.
// Step 5 is iterative (no recursion through names), so long chains of bindings never deepen the stack. The
// syntax phase limits nesting (kMaxNesting) and expression depth (kMaxExprDepth), which bounds every recursion.
//
// Syntax errors are reported per member with recovery (skip to the next line that starts a member); if there are
// any, lowering is skipped. Lowering errors are collected for the whole file; if there are any, validation is
// skipped. So each phase only reports first-order problems.

#include "pq/graph/parse.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

#include "lexer.h"
#include "text_common.h"

namespace pq::graph {

using namespace detail;

namespace {

// ---- diagnostics ----------------------------------------------------------------------------------------------

struct Diags {
  std::vector<Diagnostic> list;
  bool full() const noexcept { return list.size() >= kMaxDiagnostics; }
  void add(uint32_t offset, uint32_t length, std::string message, ErrorCode code = ErrorCode::Parse) {
    if (full()) return;
    Diagnostic d;
    d.message = std::move(message);
    d.offset = offset;
    d.length = length;
    d.code = code;
    list.push_back(std::move(d));
  }
};

// Thrown after a syntax error has been recorded; caught where the parser can resynchronise.
struct SyntaxError {};

// A byte range of the source (for diagnostics about a node).
using Span = SourceSpan;

// ---- AST --------------------------------------------------------------------------------------------------------

enum class AstKind : uint8_t { Number, Bool, Name, NamePort, AtomRef, Unary, Binary, Ternary, Call, Field, Index, Tuple };

struct AstNode {
  AstKind kind = AstKind::Number;
  Unit unit = Unit::None;  // Number
  uint16_t depth = 1;
  AtomId atom = 0;          // Unary/Binary/Ternary/Index/Tuple/AtomRef
  uint32_t tok = 0;         // main token: literal, name, operator, call name, '[' or '('
  uint32_t tok2 = kNone;    // NamePort/Field: the field name token
  uint32_t a = kNone, b = kNone, c = kNone;  // children; Call/Tuple: a = first arg, b = arg count
  double number = 0.0;      // Number (sign folded in), Bool (0/1)
  bool paren = false;       // written in parentheses: `-(3)` negates a literal, `-3` is one
};

struct AstArg {
  uint32_t nameTok = kNone;  // kNone: positional
  uint32_t expr = kNone;
};

// ---- syntax of a file -------------------------------------------------------------------------------------------

enum class MemberKind : uint8_t { Param, Input, Output, Export, Binding, PortBinding, Unknown, Equation };

struct Member {
  MemberKind kind = MemberKind::Binding;
  uint32_t nameTok = kNone;   // declared/bound name (Equation: the '==' token)
  uint32_t fieldTok = kNone;  // PortBinding: the input name
  uint32_t expr = kNone;      // default / value / rhs / init / equation lhs
  uint32_t expr2 = kNone;     // equation rhs
  uint32_t index = kNone;     // Param: index into PatchSyn::params; Unknown/Equation: solve block
  PortType type;              // Input/Output
};

struct SolveSyn {
  uint32_t iterations = 1;
  uint32_t tok = 0;  // 'solve'
};

struct LayoutNodeSyn {
  uint32_t nameTok = 0;
  NodeLayout layout;
};

struct LayoutWireSyn {
  uint32_t fromTok = 0, fromPortTok = 0, toTok = 0, toPortTok = 0;
  std::vector<Point2> via;
};

struct PatchSyn {
  uint32_t nameTok = 0;
  std::vector<Member> members;
  std::vector<ParamDecl> params;
  std::vector<SolveSyn> solves;
  std::vector<LayoutNodeSyn> layoutNodes;
  std::vector<LayoutWireSyn> layoutWires;
  std::vector<LayoutComment> comments;
};

struct ImportSyn {
  std::string path;
  uint32_t tok = 0;
};

// ---- parser -----------------------------------------------------------------------------------------------------

class Parser {
public:
  Parser(std::string_view src, const std::vector<Token>& toks, Diags& diags) : src_(src), toks_(toks), diags_(diags) {}

  std::vector<AstNode> ast;
  std::vector<AstArg> args;

  void parseFile(std::vector<ImportSyn>& imports, std::vector<PatchSyn>& patches) {
    while (peek().kind != Tok::End && !diags_.full()) {
      try {
        if (peek().kind == Tok::KwImport) {
          parseImport(imports);
        } else if (peek().kind == Tok::KwPatch) {
          patches.emplace_back();
          parsePatch(patches.back());
        } else {
          fail(pos_, "expected 'patch' or 'import' at the top level, found " + describe(pos_));
        }
      } catch (SyntaxError) {
        // Skip to the next top-level item.
        advance();
        while (peek().kind != Tok::End && peek().kind != Tok::KwPatch && peek().kind != Tok::KwImport) advance();
      }
    }
  }

  std::string_view text(uint32_t tok) const noexcept { return src_.substr(toks_[tok].offset, toks_[tok].length); }

private:
  // ---- token access ----
  const Token& peek(uint32_t k = 0) const noexcept {
    const size_t i = std::min<size_t>(pos_ + k, toks_.size() - 1);
    return toks_[i];
  }
  Tok kind(uint32_t k = 0) const noexcept { return peek(k).kind; }
  uint32_t advance() noexcept {
    const uint32_t t = pos_;
    if (pos_ + 1 < toks_.size()) ++pos_;
    return t;
  }
  bool accept(Tok k) noexcept {
    if (kind() != k) return false;
    advance();
    return true;
  }
  bool isWord(uint32_t k, std::string_view w) const noexcept {
    return kind(k) == Tok::Ident && text(static_cast<uint32_t>(std::min<size_t>(pos_ + k, toks_.size() - 1))) == w;
  }

  std::string describe(uint32_t tok) const {
    const Token& t = toks_[tok];
    switch (t.kind) {
      case Tok::End: return "end of file";
      case Tok::Ident: return "'" + std::string(text(tok)) + "'";
      case Tok::Number: return "number '" + std::string(text(tok)) + "'";
      case Tok::String: return "a string";
      default: return std::string(tokText(t.kind));
    }
  }

  [[noreturn]] void fail(uint32_t tok, std::string message) {
    const Token& t = toks_[tok];
    diags_.add(t.offset, t.length, std::move(message));
    throw SyntaxError{};
  }
  void report(uint32_t tok, std::string message) {
    const Token& t = toks_[tok];
    diags_.add(t.offset, t.length, std::move(message));
  }
  [[noreturn]] void failAst(uint32_t node, std::string message) { fail(ast[node].tok, std::move(message)); }

  uint32_t expect(Tok k, std::string_view context) {
    if (kind() != k) fail(pos_, "expected " + std::string(tokText(k)) + " " + std::string(context) + ", found " + describe(pos_));
    return advance();
  }

  // A name: an identifier, or `in`/`out` (contextual keywords, QUESTIONS Q7).
  static bool nameTok(Tok k) noexcept { return k == Tok::Ident || k == Tok::KwIn || k == Tok::KwOut; }
  uint32_t expectName(std::string_view what) {
    if (!nameTok(kind())) {
      std::string msg = "expected " + std::string(what) + ", found " + describe(pos_);
      if (isKeywordTok(kind())) msg += " (a keyword cannot be used as a name)";
      fail(pos_, std::move(msg));
    }
    return advance();
  }
  // After '.', any identifier or keyword names a port or field (`track.in`, QUESTIONS Q8).
  uint32_t expectField() {
    if (kind() != Tok::Ident && !isKeywordTok(kind())) fail(pos_, "expected a name after '.', found " + describe(pos_));
    return advance();
  }

  // ---- nesting guard ----
  struct Nest {
    Parser& p;
    Nest(Parser& parser, uint32_t tok) : p(parser) {
      if (++p.nesting_ > kMaxNesting) {
        --p.nesting_;
        p.fail(tok, "expression is nested too deeply (more than " + std::to_string(kMaxNesting) + " levels)");
      }
    }
    ~Nest() { --p.nesting_; }
    Nest(const Nest&) = delete;
    Nest& operator=(const Nest&) = delete;
  };

  uint32_t makeNode(AstNode n) {
    if (n.depth > kMaxExprDepth)
      fail(n.tok, "expression is too deep (more than " + std::to_string(kMaxExprDepth) +
                      " levels); split it with named intermediate bindings");
    ast.push_back(n);
    return static_cast<uint32_t>(ast.size() - 1);
  }
  uint16_t depthOf(uint32_t node) const noexcept { return node == kNone ? 0 : ast[node].depth; }
  static uint16_t deeper(uint16_t d) noexcept { return static_cast<uint16_t>(std::min<uint32_t>(d + 1u, 0xFFFFu)); }

  // ---- top level ----
  void parseImport(std::vector<ImportSyn>& imports) {
    advance();  // import
    const uint32_t s = expect(Tok::String, "(the imported file) after 'import'");
    ImportSyn imp;
    decodeString(text(s), imp.path);
    imp.tok = s;
    if (imp.path.empty()) fail(s, "empty import path");
    imports.push_back(std::move(imp));
    accept(Tok::Semi);
  }

  void parsePatch(PatchSyn& p) {
    advance();  // patch
    if (kind() != Tok::Ident) {
      std::string msg = "expected the patch name after 'patch', found " + describe(pos_);
      if (isKeywordTok(kind())) msg += " (a keyword cannot be used as a name)";
      fail(pos_, std::move(msg));
    }
    p.nameTok = advance();
    const uint32_t open = expect(Tok::LBrace, "after the patch name");
    bool layoutSeen = false;
    for (;;) {
      if (diags_.full()) throw SyntaxError{};
      if (kind() == Tok::RBrace) {
        advance();
        return;
      }
      if (kind() == Tok::End) fail(open, "'{' of patch '" + std::string(text(p.nameTok)) + "' is never closed");
      const uint32_t start = pos_;
      try {
        if (layoutSeen) fail(pos_, "the layout block must be the last item in a patch");
        if (kind() == Tok::KwLayout) {
          parseLayout(p);
          layoutSeen = true;
        } else {
          parseMember(p);
        }
      } catch (SyntaxError) {
        if (kind() == Tok::End) fail(open, "'{' of patch '" + std::string(text(p.nameTok)) + "' is never closed");
        syncMember(start);
      }
    }
  }

  // Skips to the next token that can start a member on a new line, or to the '}' closing the current block. Always
  // makes progress past `start` (the first token of the failed item).
  void syncMember(uint32_t start) {
    int depth = 0;
    if (pos_ == start) advance();
    for (;;) {
      const Token& t = peek();
      if (t.kind == Tok::End) return;
      if (depth == 0 && t.kind == Tok::RBrace) return;
      if (depth == 0 && t.lineStart() && (t.kind == Tok::Ident || isKeywordTok(t.kind))) return;
      if (t.kind == Tok::LBrace) ++depth;
      if (t.kind == Tok::RBrace) --depth;
      advance();
    }
  }

  // ---- members ----
  void parseMember(PatchSyn& p) {
    switch (kind()) {
      case Tok::KwParam: parseParam(p); return;
      case Tok::KwIn:
      case Tok::KwOut:
        if (nameTok(kind(1)) && kind(2) == Tok::Colon) {
          parsePortDecl(p);
          return;
        }
        parseBinding(p);
        return;
      case Tok::KwExport: {
        advance();
        Member m;
        m.kind = MemberKind::Export;
        m.nameTok = expectName("the export name");
        expect(Tok::Assign, "after the export name");
        m.expr = parseExpr();
        p.members.push_back(m);
        return;
      }
      case Tok::KwSolve: parseSolve(p); return;
      case Tok::Ident: parseBinding(p); return;
      default: {
        std::string msg = "expected a declaration (param, in, out, export, solve, layout) or a binding 'name = …', found " +
                          describe(pos_);
        if (kind() == Tok::KwPatch) msg += " (missing '}' before it?)";
        fail(pos_, std::move(msg));
      }
    }
  }

  void parseBinding(PatchSyn& p) {
    Member m;
    m.nameTok = advance();
    m.kind = MemberKind::Binding;
    if (accept(Tok::Dot)) {
      m.kind = MemberKind::PortBinding;
      m.fieldTok = expectField();
    }
    if (kind() != Tok::Assign) {
      if (m.kind == MemberKind::Binding && kind() == Tok::Colon)
        fail(pos_, "expected '=' after '" + std::string(text(m.nameTok)) +
                       "', found ':' (declarations start with 'in', 'out' or 'param')");
      fail(pos_, "expected '=' after '" + std::string(text(m.nameTok)) + "', found " + describe(pos_));
    }
    advance();
    m.expr = parseExpr();
    p.members.push_back(m);
  }

  Kind parseValueKind(bool forParam, Rate& impliedRate, uint32_t& tok) {
    tok = pos_;
    impliedRate = Rate::Infer;
    if (isWord(0, "real")) {
      advance();
      return Kind::Real;
    }
    if (isWord(0, "bool")) {
      advance();
      return Kind::Bool;
    }
    if (isWord(0, "audio")) {
      if (forParam) fail(pos_, "a param is a control value and cannot be 'audio'; declare it 'real'");
      advance();
      impliedRate = Rate::Audio;
      return Kind::Real;
    }
    fail(pos_, "expected a value kind (real, bool or audio), found " + describe(pos_));
  }

  // portType = valueKind [rate] [poly] [width]. The type ends at a line break (the next line starts a member),
  // which keeps `in x : real` followed by `param y = …` or `mono = …` unambiguous.
  PortType parsePortType() {
    PortType t;
    Rate implied = Rate::Infer;
    uint32_t kindTok = 0;
    t.kind = parseValueKind(false, implied, kindTok);
    t.rate = implied;
    int stage = 0;  // 1 after rate, 2 after poly, 3 after width
    auto sameLine = [&] { return !peek().lineStart(); };
    while (sameLine()) {
      int wordStage = 0;
      if (kind() == Tok::KwParam || isWord(0, "const") || isWord(0, "audio")) wordStage = 1;
      else if (isWord(0, "global") || isWord(0, "voice")) wordStage = 2;
      else if (isWord(0, "mono") || isWord(0, "stereo") || isWord(0, "ch")) wordStage = 3;
      else if (isWord(0, "real") || isWord(0, "bool")) fail(pos_, "a port type has one value kind");
      else break;
      if (wordStage <= stage)
        fail(pos_, "port type words must come in the order: kind, rate, poly, width (found " + describe(pos_) + ")");
      if (wordStage == 1) {
        if (t.rate != Rate::Infer) fail(pos_, "'audio' already means a real at audio rate; omit the rate");
        t.rate = kind() == Tok::KwParam ? Rate::Param : isWord(0, "const") ? Rate::Const : Rate::Audio;
        advance();
      } else if (wordStage == 2) {
        t.poly = isWord(0, "global") ? Poly::Global : Poly::Voice;
        advance();
      } else {
        if (isWord(0, "mono")) {
          t.width = kWidthMono;
          advance();
        } else if (isWord(0, "stereo")) {
          t.width = kWidthStereo;
          advance();
        } else {
          advance();  // ch
          expect(Tok::LParen, "after 'ch'");
          const uint32_t n = pos_;
          if (kind() != Tok::Number || !peek().integer()) fail(pos_, "expected a channel count (an integer) in ch(…)");
          advance();
          const double v = toks_[n].number;
          if (v < 1 || v > 64) fail(n, "channel count must be 1..64");
          t.width = static_cast<uint16_t>(v);
          expect(Tok::RParen, "after the channel count");
        }
      }
      stage = wordStage;
    }
    return t;
  }

  void parsePortDecl(PatchSyn& p) {
    const bool isIn = kind() == Tok::KwIn;
    advance();
    Member m;
    m.kind = isIn ? MemberKind::Input : MemberKind::Output;
    m.nameTok = expectName(isIn ? "the input name" : "the output name");
    expect(Tok::Colon, "after the port name");
    m.type = parsePortType();
    if (kind() == Tok::Assign) {
      if (!isIn) fail(pos_, "an 'out' has no default; bind it with '" + std::string(text(m.nameTok)) + " = …'");
      advance();
      m.expr = parseExpr();
    }
    p.members.push_back(m);
  }

  // Folds a constant expression (param default or range end). A plain literal (optionally negated) keeps its
  // unit; arithmetic yields the normalised value without a unit (QUESTIONS Q10).
  bool fold(uint32_t node, Constant& out, uint32_t& bad) const {
    const AstNode& n = ast[node];
    switch (n.kind) {
      case AstKind::Number: out = Constant::real(n.number, n.unit); return true;
      case AstKind::Bool: out = Constant::boolean(n.number != 0.0); return true;
      case AstKind::Unary: {
        Constant x;
        if (!fold(n.a, x, bad)) return false;
        const OperatorAtoms& ops = OperatorAtoms::get();
        if (n.atom == ops.neg && x.kind == Kind::Real) {
          out = Constant::real(-x.value());
          return true;
        }
        if (n.atom == ops.notOp && x.kind == Kind::Bool) {
          out = Constant::boolean(x.literal == 0.0);
          return true;
        }
        bad = node;
        return false;
      }
      case AstKind::Binary: {
        Constant x, y;
        if (!fold(n.a, x, bad) || !fold(n.b, y, bad)) return false;
        if (x.kind != Kind::Real || y.kind != Kind::Real) {
          bad = node;
          return false;
        }
        const double a = x.value(), b = y.value();
        const std::string_view op = atomSignature(n.atom).name;
        double r;
        if (op == "add") r = a + b;
        else if (op == "sub") r = a - b;
        else if (op == "mul") r = a * b;
        else if (op == "div") r = a / b;
        else if (op == "mod") r = a - b * std::floor(a / b);
        else {
          bad = node;
          return false;
        }
        out = Constant::real(r);
        return true;
      }
      default: bad = node; return false;
    }
  }

  Constant constantExpr(uint32_t node, std::string_view what) {
    Constant c;
    uint32_t bad = node;
    if (!fold(node, c, bad))
      failAst(bad, std::string(what) + " must be a constant: a number, true/false, or arithmetic (+ - * / %) on numbers");
    return c;
  }

  // The first literal written in `smp` in a folded constant expression, or kNone.
  uint32_t smpLiteral(uint32_t node) const {
    if (node == kNone) return kNone;
    const AstNode& n = ast[node];
    if (n.kind == AstKind::Number) return n.unit == Unit::smp ? node : kNone;
    if (n.kind != AstKind::Unary && n.kind != AstKind::Binary) return kNone;
    if (const uint32_t a = smpLiteral(n.a); a != kNone) return a;
    return n.kind == AstKind::Binary ? smpLiteral(n.b) : kNone;
  }

  // A param default or range end: a constant, never in samples. A param is a knob value with no unit after
  // normalisation; at a duration input it is read as seconds, so `param t = 100smp` would be a 100 s delay
  // (M1 review L1, QUESTIONS Q35).
  Constant paramConstant(uint32_t node, std::string_view what) {
    Constant c = constantExpr(node, what);
    if (const uint32_t bad = smpLiteral(node); bad != kNone)
      failAst(bad, "a param cannot be in samples (`smp`): params are read as seconds at duration inputs, so this "
                   "would be that many seconds. Use ms or s, or a plain number of samples divided by sr where it is "
                   "used (delay(x, n / sr, …))");
    return c;
  }

  void parseParam(PatchSyn& p) {
    advance();  // param
    Member m;
    m.kind = MemberKind::Param;
    m.nameTok = expectName("the param name");
    ParamDecl d;
    bool kindGiven = false;
    uint32_t kindTok = 0;
    if (accept(Tok::Colon)) {
      Rate implied;
      d.kind = parseValueKind(true, implied, kindTok);
      kindGiven = true;
    }
    expect(Tok::Assign, "after the param name (a param needs a default: 'param x = 0.5')");
    // `[` after the default starts the range, so index syntax is off in the default.
    noIndex_ = true;
    uint32_t def;
    try {
      def = parseExpr();
    } catch (SyntaxError) {
      noIndex_ = false;
      throw;
    }
    noIndex_ = false;
    d.defaultValue = paramConstant(def, "a param default");
    if (!kindGiven) d.kind = d.defaultValue.kind;
    else if (d.kind != d.defaultValue.kind)
      failAst(def, "param '" + std::string(text(m.nameTok)) + "' is " + std::string(toText(d.kind)) +
                       " but its default is " + std::string(toText(d.defaultValue.kind)));
    if (kind() == Tok::LBracket) {
      const uint32_t open = advance();
      const uint32_t lo = parseExpr();
      expect(Tok::DotDot, "between the range ends ([min..max])");
      const uint32_t hi = parseExpr();
      expect(Tok::RBracket, "after the range");
      if (d.kind != Kind::Real) fail(open, "only real params have a range");
      d.hasRange = true;
      d.rangeMin = paramConstant(lo, "a range end");
      d.rangeMax = paramConstant(hi, "a range end");
      if (d.rangeMin.kind != Kind::Real) failAst(lo, "a range end must be a number");
      if (d.rangeMax.kind != Kind::Real) failAst(hi, "a range end must be a number");
    }
    while (kind() == Tok::At) d.attrs.push_back(parseAttr());
    m.index = static_cast<uint32_t>(p.params.size());
    p.params.push_back(std::move(d));
    p.members.push_back(m);
  }

  // attr = "@" Ident [ "(" [ attrArg { "," attrArg } ] ")" ];  attrArg = [ Ident ":" ] literal (QUESTIONS Q10).
  Attr parseAttr() {
    advance();  // @
    Attr a;
    if (kind() != Tok::Ident) fail(pos_, "expected an attribute name after '@', found " + describe(pos_));
    a.name = std::string(text(advance()));
    if (kind() != Tok::LParen || peek().lineStart()) return a;
    advance();
    if (accept(Tok::RParen)) return a;
    for (;;) {
      AttrArg arg;
      if ((kind() == Tok::Ident || isKeywordTok(kind())) && kind(1) == Tok::Colon) {
        if (kind() != Tok::Ident) fail(pos_, "a keyword cannot name an attribute argument");
        arg.name = std::string(text(advance()));
        advance();  // :
      }
      arg.value = parseAttrValue();
      a.args.push_back(std::move(arg));
      if (accept(Tok::RParen)) return a;
      expect(Tok::Comma, "between attribute arguments");
    }
  }

  AttrValue parseAttrValue() {
    const Tok k = kind();
    if (k == Tok::Minus || k == Tok::Number) {
      const bool neg = accept(Tok::Minus);
      if (kind() != Tok::Number) fail(pos_, "expected a number after '-'");
      const Token& t = toks_[advance()];
      return AttrValue::num(neg ? -t.number : t.number, t.unit);
    }
    if (k == Tok::KwTrue || k == Tok::KwFalse) {
      advance();
      return AttrValue::boolean(k == Tok::KwTrue);
    }
    if (k == Tok::String) {
      AttrValue v = AttrValue::string({});
      decodeString(text(advance()), v.text);
      return v;
    }
    if (k == Tok::Ident || isKeywordTok(k)) return AttrValue::ident(std::string(text(advance())));
    fail(pos_, "attribute arguments are literals (a number, true/false, a string or a word), found " + describe(pos_));
  }

  void parseSolve(PatchSyn& p) {
    SolveSyn s;
    s.tok = advance();  // solve
    expect(Tok::LParen, "after 'solve' (solve(iterations: n) { … })");
    expect(Tok::KwIterations, "in solve(iterations: n)");
    expect(Tok::Colon, "after 'iterations'");
    const uint32_t n = pos_;
    if (kind() != Tok::Number || !peek().integer()) fail(pos_, "expected the iteration count (an integer), found " + describe(pos_));
    advance();
    const double it = toks_[n].number;
    if (it < 1 || it > kMaxSolveIterations)
      fail(n, "solve iterations must be 1.." + std::to_string(kMaxSolveIterations) + " (found " +
                  std::string(text(n)) + ")");
    s.iterations = static_cast<uint32_t>(it);
    expect(Tok::RParen, "after the iteration count");
    const uint32_t open = expect(Tok::LBrace, "to start the solve block");
    const auto index = static_cast<uint32_t>(p.solves.size());
    p.solves.push_back(s);
    uint32_t unknowns = 0, equations = 0;
    bool failed = false;
    for (;;) {
      if (diags_.full()) throw SyntaxError{};
      if (kind() == Tok::RBrace) {
        advance();
        break;
      }
      if (kind() == Tok::End) fail(open, "'{' of the solve block is never closed");
      const uint32_t start = pos_;
      // Items are line-separated: see atItemEnd().
      struct LineItems {
        bool& flag;
        explicit LineItems(bool& f) : flag(f) { flag = true; }
        ~LineItems() { flag = false; }
        LineItems(const LineItems&) = delete;
        LineItems& operator=(const LineItems&) = delete;
      } lineItems(lineItems_);
      try {
        if (isWord(0, "unknown") && nameTok(kind(1))) {
          advance();
          Member m;
          m.kind = MemberKind::Unknown;
          m.nameTok = expectName("the unknown's name");
          m.index = index;
          if (accept(Tok::Assign)) m.expr = parseExpr();
          p.members.push_back(m);
          ++unknowns;
        } else {
          const uint32_t e = parseExpr();
          const AstNode& top = ast[e];
          if (top.kind != AstKind::Binary || toks_[top.tok].kind != Tok::Eq)
            failAst(e, "expected 'unknown name' or an equation 'lhs == rhs' in a solve block");
          Member m;
          m.kind = MemberKind::Equation;
          m.nameTok = top.tok;
          m.expr = top.a;
          m.expr2 = top.b;
          m.index = index;
          p.members.push_back(m);
          ++equations;
        }
        if (kind() != Tok::RBrace && kind() != Tok::End && !peek().lineStart())
          fail(pos_, "a solve block has one item per line; found " + describe(pos_) + " after the item");
      } catch (SyntaxError) {
        failed = true;
        if (kind() == Tok::End) fail(open, "'{' of the solve block is never closed");
        syncMember(start);
      }
    }
    if (failed) return;
    if (unknowns == 0) report(s.tok, "a solve block needs at least one 'unknown'");
    else if (unknowns != equations)
      report(s.tok, "a solve block needs as many equations as unknowns (" + std::to_string(unknowns) + " unknowns, " +
                      std::to_string(equations) + " equations)");
  }

  // A layout coordinate: [-] Number without a unit, parsed as float directly (exact round trip of printed floats).
  float layoutNumber() {
    const bool neg = accept(Tok::Minus);
    if (kind() != Tok::Number) fail(pos_, "expected a coordinate (a number), found " + describe(pos_));
    const uint32_t t = advance();
    if (toks_[t].unit != Unit::None) fail(t, "layout coordinates have no unit");
    float v = 0.0f;
    const std::string_view s = text(t);
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) fail(t, "coordinate '" + std::string(s) + "' is out of range");
    return neg ? -v : v;
  }

  Point2 point() {
    expect(Tok::LParen, "to start a point (x, y)");
    Point2 pt;
    pt.x = layoutNumber();
    expect(Tok::Comma, "between the coordinates");
    pt.y = layoutNumber();
    expect(Tok::RParen, "after the point");
    return pt;
  }

  void expectWord(std::string_view w, std::string_view context) {
    if (!isWord(0, w)) fail(pos_, "expected '" + std::string(w) + "' " + std::string(context) + ", found " + describe(pos_));
    advance();
  }

  void parseLayout(PatchSyn& p) {
    advance();  // layout
    const uint32_t open = expect(Tok::LBrace, "after 'layout'");
    for (;;) {
      if (diags_.full()) throw SyntaxError{};
      if (kind() == Tok::RBrace) {
        advance();
        break;
      }
      if (kind() == Tok::End) fail(open, "'{' of the layout block is never closed");
      const uint32_t start = pos_;
      try {
        if (isWord(0, "node")) {
          advance();
          LayoutNodeSyn n;
          n.nameTok = expectName("the node name");
          expectWord("at", "after the node name");
          const Point2 pt = point();
          n.layout.placed = true;
          n.layout.x = pt.x;
          n.layout.y = pt.y;
          if (isWord(0, "rot")) {
            advance();
            const bool neg = accept(Tok::Minus);
            const uint32_t t = pos_;
            if (kind() != Tok::Number || !peek().integer()) fail(pos_, "expected a rotation in degrees (an integer)");
            advance();
            const double r = neg ? -toks_[t].number : toks_[t].number;
            if (r < -32768 || r > 32767) fail(t, "rotation is out of range");
            n.layout.rotation = static_cast<int16_t>(r);
          }
          if (isWord(0, "flip")) {
            advance();
            n.layout.flip = true;
          }
          p.layoutNodes.push_back(n);
        } else if (isWord(0, "wire")) {
          advance();
          LayoutWireSyn w;
          w.fromTok = expectName("the source node of the wire");
          expect(Tok::Dot, "after the source node (wire a.out -> b.in)");
          w.fromPortTok = expectField();
          expect(Tok::Arrow, "between the wire's ends");
          w.toTok = expectName("the target node of the wire");
          expect(Tok::Dot, "after the target node (wire a.out -> b.in)");
          w.toPortTok = expectField();
          if (isWord(0, "via")) {
            advance();
            expect(Tok::LParen, "after 'via' (via ((x, y), …))");
            w.via.push_back(point());
            while (accept(Tok::Comma)) w.via.push_back(point());
            expect(Tok::RParen, "after the routing points");
          }
          p.layoutWires.push_back(std::move(w));
        } else if (isWord(0, "comment")) {
          advance();
          LayoutComment c;
          const uint32_t s = expect(Tok::String, "after 'comment'");
          decodeString(text(s), c.text);
          expectWord("at", "after the comment text");
          c.at = point();
          p.comments.push_back(std::move(c));
        } else {
          fail(pos_, "expected 'node', 'wire' or 'comment' in a layout block, found " + describe(pos_));
        }
      } catch (SyntaxError) {
        if (kind() == Tok::End) fail(open, "'{' of the layout block is never closed");
        syncMember(start);
      }
    }
  }

  // ---- expressions ----
  //
  // Recursion only happens through parseExpr (parenthesised expressions, call and tuple arguments, indices,
  // ternary branches), so the nesting guard there bounds the stack: a few frames per level. Binary operators are
  // parsed iteratively (operator precedence with an explicit stack), so long chains like a + b + … cost no stack.

  // In a solve block, a line break ends an item unless it is inside brackets (an equation may start with '(',
  // '-' or '!', which would otherwise continue the previous line).
  bool atItemEnd() const noexcept { return lineItems_ && brackets_ == 0 && peek().lineStart(); }

  struct Bracket {
    Parser& p;
    explicit Bracket(Parser& parser) : p(parser) { ++p.brackets_; }
    ~Bracket() { --p.brackets_; }
    Bracket(const Bracket&) = delete;
    Bracket& operator=(const Bracket&) = delete;
  };

  template <class T>
  struct Truncate {  // restores a scratch stack to its size on entry, also when a syntax error unwinds
    std::vector<T>& v;
    size_t size;
    explicit Truncate(std::vector<T>& vec) : v(vec), size(vec.size()) {}
    ~Truncate() { v.resize(size); }
    Truncate(const Truncate&) = delete;
    Truncate& operator=(const Truncate&) = delete;
  };

  uint32_t parseExpr() {
    Nest guard(*this, pos_);
    const uint32_t cond = parseBinary();
    if (kind() != Tok::Question || atItemEnd()) return cond;
    const uint32_t q = advance();
    const uint32_t a = parseExpr();
    expect(Tok::Colon, "in 'cond ? a : b'");
    const uint32_t b = parseExpr();
    AstNode n;
    n.kind = AstKind::Ternary;
    n.atom = OperatorAtoms::get().select;
    n.tok = q;
    n.a = cond;
    n.b = a;
    n.c = b;
    n.depth = deeper(std::max({depthOf(cond), depthOf(a), depthOf(b)}));
    return makeNode(n);
  }

  static int binaryOp(Tok k) noexcept {
    for (int i = 0; i < static_cast<int>(std::size(kBinaryOps)); ++i)
      if (kBinaryOps[i].tok == k) return i;
    return -1;
  }

  struct PendingOp {
    int op;
    uint32_t tok;
  };

  void reduce() {
    const PendingOp op = ops_.back();
    ops_.pop_back();
    const uint32_t rhs = vals_.back();
    vals_.pop_back();
    const uint32_t lhs = vals_.back();
    AstNode n;
    n.kind = AstKind::Binary;
    n.atom = OperatorAtoms::get().binary[op.op];
    n.tok = op.tok;
    n.a = lhs;
    n.b = rhs;
    n.depth = deeper(std::max(depthOf(lhs), depthOf(rhs)));
    vals_.back() = makeNode(n);
  }

  uint32_t parseBinary() {
    const Truncate<PendingOp> keepOps(ops_);
    const Truncate<uint32_t> keepVals(vals_);
    const size_t opBase = ops_.size();
    vals_.push_back(parseUnary());
    for (;;) {
      const int index = atItemEnd() ? -1 : binaryOp(kind());
      if (index < 0) break;
      const uint8_t prec = kBinaryOps[index].prec;
      while (ops_.size() > opBase && kBinaryOps[ops_.back().op].prec >= prec) {
        if (prec == kPrecCompare && kBinaryOps[ops_.back().op].prec == kPrecCompare)
          fail(pos_, "comparisons cannot be chained; use parentheses or '&&'");
        reduce();
      }
      ops_.push_back({index, advance()});
      vals_.push_back(parseUnary());
    }
    while (ops_.size() > opBase) reduce();
    return vals_.back();
  }

  uint32_t parseUnary() {
    if (kind() != Tok::Minus && kind() != Tok::Bang) return parsePostfix();
    const uint32_t opTok = advance();
    if (kind() == Tok::Minus || kind() == Tok::Bang)
      fail(pos_, "only one prefix operator is allowed; use parentheses, e.g. -(-x)");
    const uint32_t x = parsePostfix();
    const bool neg = toks_[opTok].kind == Tok::Minus;
    // `-literal` is a negative literal (so a printed negative constant reads back as the same constant).
    if (neg && ast[x].kind == AstKind::Number && !ast[x].paren) {
      ast[x].number = -ast[x].number;
      return x;
    }
    AstNode n;
    n.kind = AstKind::Unary;
    n.atom = neg ? OperatorAtoms::get().neg : OperatorAtoms::get().notOp;
    n.tok = opTok;
    n.a = x;
    n.depth = deeper(depthOf(x));
    return makeNode(n);
  }

  uint32_t parsePostfix() {
    uint32_t base = parsePrimary();
    for (;;) {
      if (kind() == Tok::Dot && !atItemEnd()) {
        advance();
        const uint32_t field = expectField();
        AstNode& b = ast[base];
        if (b.kind == AstKind::Name) {
          // `voice.hz`, `transport.bpm`, `track.in`: namespaced built-in atoms (no inputs).
          const std::string_view ns = text(b.tok), f = text(field);
          char buf[64];
          if (ns.size() + 1 + f.size() <= sizeof buf) {
            std::copy(ns.begin(), ns.end(), buf);
            buf[ns.size()] = '.';
            std::copy(f.begin(), f.end(), buf + ns.size() + 1);
            const auto id = AtomRegistry::builtin().find(std::string_view(buf, ns.size() + 1 + f.size()));
            if (id && atomSignature(*id).inputs.empty()) {
              b.kind = AstKind::AtomRef;
              b.atom = *id;
              b.tok2 = field;
              continue;
            }
          }
          b.kind = AstKind::NamePort;
          b.tok2 = field;
          continue;
        }
        AstNode n;
        n.kind = AstKind::Field;
        n.tok = field;
        n.tok2 = field;
        n.a = base;
        n.depth = deeper(depthOf(base));
        base = makeNode(n);
        continue;
      }
      if (kind() == Tok::LBracket && !noIndex_ && !atItemEnd()) {
        const Bracket bracket(*this);
        const uint32_t open = advance();
        const uint32_t index = parseExpr();
        expect(Tok::RBracket, "after the channel index");
        AstNode n;
        n.kind = AstKind::Index;
        n.atom = OperatorAtoms::get().channel;
        n.tok = open;
        n.a = base;
        n.b = index;
        n.depth = deeper(std::max(depthOf(base), depthOf(index)));
        base = makeNode(n);
        continue;
      }
      return base;
    }
  }

  // Moves the arguments collected on argStack_ since `base` into `args` as one contiguous range.
  uint32_t commitArgs(size_t base) {
    const auto first = static_cast<uint32_t>(args.size());
    args.insert(args.end(), argStack_.begin() + static_cast<std::ptrdiff_t>(base), argStack_.end());
    return first;
  }

  uint32_t parsePrimary() {
    const Tok k = kind();
    if (k == Tok::Number) {
      AstNode n;
      n.kind = AstKind::Number;
      n.tok = advance();
      n.number = toks_[n.tok].number;
      n.unit = toks_[n.tok].unit;
      return makeNode(n);
    }
    if (k == Tok::KwTrue || k == Tok::KwFalse) {
      AstNode n;
      n.kind = AstKind::Bool;
      n.tok = advance();
      n.number = k == Tok::KwTrue ? 1.0 : 0.0;
      return makeNode(n);
    }
    if (nameTok(k)) {
      const uint32_t t = advance();
      if (k == Tok::Ident && kind() == Tok::LParen && !atItemEnd()) return parseCall(t);
      AstNode n;
      n.kind = AstKind::Name;
      n.tok = t;
      return makeNode(n);
    }
    if (k == Tok::LParen) {
      const Bracket bracket(*this);
      const uint32_t open = advance();
      const bool savedNoIndex = noIndex_;
      noIndex_ = false;
      const uint32_t first = parseExpr();
      if (kind() != Tok::Comma) {
        expect(Tok::RParen, "to close '('");
        noIndex_ = savedNoIndex;
        ast[first].paren = true;
        return first;
      }
      // Tuple (a, b, …) → pack.
      const Truncate<AstArg> keep(argStack_);
      const size_t base = argStack_.size();
      argStack_.push_back({kNone, first});
      uint16_t depth = depthOf(first);
      while (accept(Tok::Comma)) {
        const uint32_t e = parseExpr();
        argStack_.push_back({kNone, e});
        depth = std::max(depth, depthOf(e));
      }
      expect(Tok::RParen, "to close the tuple");
      noIndex_ = savedNoIndex;
      AstNode n;
      n.kind = AstKind::Tuple;
      n.atom = OperatorAtoms::get().pack;
      n.tok = open;
      n.b = static_cast<uint32_t>(argStack_.size() - base);
      n.a = commitArgs(base);
      n.depth = deeper(depth);
      return makeNode(n);
    }
    std::string msg = "expected an expression, found " + describe(pos_);
    if (isKeywordTok(k)) msg += " (a keyword cannot be used as a name)";
    fail(pos_, std::move(msg));
  }

  uint32_t parseCall(uint32_t nameT) {
    const Bracket bracket(*this);
    advance();  // (
    const bool savedNoIndex = noIndex_;
    noIndex_ = false;
    // Arguments of nested calls are committed before ours, so ours are collected on a scratch stack first.
    const Truncate<AstArg> keep(argStack_);
    const size_t base = argStack_.size();
    uint16_t depth = 0;
    bool named = false;
    if (kind() != Tok::RParen) {
      for (;;) {
        AstArg a;
        if (nameTok(kind()) && kind(1) == Tok::Colon) {
          a.nameTok = advance();
          advance();  // :
          named = true;
        } else if (named) {
          fail(pos_, "positional arguments must come before named arguments");
        }
        a.expr = parseExpr();
        depth = std::max(depth, depthOf(a.expr));
        argStack_.push_back(a);
        if (accept(Tok::RParen)) break;
        expect(Tok::Comma, "between arguments");
      }
    } else {
      advance();
    }
    noIndex_ = savedNoIndex;
    AstNode n;
    n.kind = AstKind::Call;
    n.tok = nameT;
    n.b = static_cast<uint32_t>(argStack_.size() - base);
    n.a = commitArgs(base);
    n.depth = deeper(depth);
    return makeNode(n);
  }

  std::string_view src_;
  const std::vector<Token>& toks_;
  Diags& diags_;
  uint32_t pos_ = 0;
  uint32_t nesting_ = 0;
  uint32_t brackets_ = 0;
  bool noIndex_ = false;
  bool lineItems_ = false;
  std::vector<PendingOp> ops_;
  std::vector<uint32_t> vals_;
  std::vector<AstArg> argStack_;
};

// ---- lowering ---------------------------------------------------------------------------------------------------

// Callable patches by name (this file's, then externals, then imported; the first of a name wins).
struct PatchTable {
  NameTable index;
  std::vector<const SubcircuitRef*> refs;
  const SubcircuitRef* find(std::string_view name) const noexcept {
    const uint32_t* i = index.find(name);
    return i ? refs[*i] : nullptr;
  }
  void add(const SubcircuitRef& r) {
    if (index.insert(r.patch, static_cast<uint32_t>(refs.size()))) refs.push_back(&r);
  }
};

class Lowerer {
public:
  Lowerer(std::string_view src, const std::vector<Token>& toks, const Parser& parser, const PatchTable& patches,
          Diags& diags)
      : src_(src), toks_(toks), ast_(parser.ast), args_(parser.args), patches_(patches), diags_(diags) {}

  // Builds `out` from `syn`; nodeSpan receives the source span of every node (for validation messages).
  void lower(const PatchSyn& syn, Patch& out, std::vector<Span>& nodeSpan) {
    p_ = &out;
    nodeSpan_ = &nodeSpan;
    syms_.clear();
    work_.clear();
    calls_.clear();
    claimed_.clear();
    names_.reset(syn.members.size());

    for (const SolveSyn& s : syn.solves) p_->addSolve(s.iterations);

    // 1. Declarations.
    for (const Member& m : syn.members) {
      switch (m.kind) {
        case MemberKind::Param: {
          const NodeIndex n = track(p_->addParam(std::string(text(m.nameTok)), syn.params[m.index]), m.nameTok);
          declare(m.nameTok, Sym::Value, {n, 0});
          break;
        }
        case MemberKind::Input: {
          const NodeIndex n = track(p_->addInput(std::string(text(m.nameTok)), m.type), m.nameTok);
          declare(m.nameTok, Sym::Value, {n, 0});
          if (m.expr != kNone) bind(m.expr, {n, 0});
          break;
        }
        case MemberKind::Output: {
          const NodeIndex n = track(p_->addOutput(std::string(text(m.nameTok)), m.type), m.nameTok);
          declare(m.nameTok, Sym::Unreadable, {n, 0});
          break;
        }
        case MemberKind::Export: {
          const NodeIndex n = track(p_->addExport(std::string(text(m.nameTok))), m.nameTok);
          declare(m.nameTok, Sym::Unreadable, {n, 0});
          bind(m.expr, {n, 0});
          break;
        }
        case MemberKind::Unknown: {
          const NodeIndex n = track(p_->addUnknown(std::string(text(m.nameTok)), m.index), m.nameTok);
          declare(m.nameTok, Sym::Value, {n, 0});
          if (m.expr != kNone) bind(m.expr, {n, 0});
          break;
        }
        case MemberKind::Equation: {
          const NodeIndex n = track(p_->addEquation(m.index), m.nameTok);
          bind(m.expr, {n, 0});
          bind(m.expr2, {n, 1});
          break;
        }
        default: break;
      }
    }

    // 2a. Binding names (and `out = …`).
    for (const Member& m : syn.members) {
      if (m.kind != MemberKind::Binding) continue;
      const std::string_view name = text(m.nameTok);
      if (const uint32_t* s = names_.find(name)) {
        Sym& sym = syms_[*s];
        if (sym.st == Sym::Unreadable && p_->node(sym.value.node).kind == NodeKind::Output) {
          if (claimed(PortRef{sym.value.node, 0}))
            errorTok(m.nameTok, "output '" + std::string(name) + "' is bound twice (first at " + where(sym.boundTok) + ")");
          else {
            sym.boundTok = m.nameTok;
            bind(m.expr, {sym.value.node, 0});
          }
          continue;
        }
        errorTok(m.nameTok, cannotBind(sym, name));
        continue;
      }
      if (!checkNewName(m.nameTok)) continue;
      const uint32_t s = declare(m.nameTok, Sym::Pending, {});
      if (s != kNone) syms_[s].rhs = m.expr;
    }
    // 2b. Heads of bindings (alias or a fresh, named node).
    for (uint32_t s = 0; s < syms_.size(); ++s) {
      if (syms_[s].st != Sym::Pending) continue;
      const uint32_t rhs = syms_[s].rhs;
      if (aliasBase(rhs) != kNone) {
        syms_[s].st = Sym::Alias;
        continue;
      }
      const PortRef v = materialize(rhs);
      if (v.node == kNone) {
        syms_[s].st = Sym::Error;
        continue;
      }
      const AstKind k = ast_[rhs].kind;
      const bool selectsPort = k == AstKind::Field || k == AstKind::NamePort;
      p_->setNodeName(v.node, std::string(syms_[s].name), selectsPort ? static_cast<uint16_t>(v.port) : kWholeNode);
      syms_[s].st = Sym::Value;
      syms_[s].value = v;
    }
    // 3. Aliases.
    resolveAliases();
    // 4. `node.input = rhs`.
    for (const Member& m : syn.members)
      if (m.kind == MemberKind::PortBinding) bindPort(m);
    // 5. Everything else.
    while (!work_.empty()) {
      const Work w = work_.back();
      work_.pop_back();
      const PortRef v = materialize(w.ast);
      if (v.node == kNone) continue;
      if (v.port >= p_->node(v.node).numOutputs) {
        errorAst(w.ast, describeValue(w.ast) + " has no output to use as a value");
        continue;
      }
      p_->connect(v, w.to);
    }
    // 6. Every call has its required inputs.
    for (const auto& [node, tok] : calls_)
      for (uint32_t k = 0; k < p_->node(node).numInputs; ++k)
        if (!claimed({node, static_cast<uint16_t>(k)}) && !p_->inputOptional(node, k))
          errorTok(tok, "missing argument '" + portTextName(*p_, node, k, true) + "' in the call to '" +
                            std::string(text(tok)) + "'");
    // 7. Layout.
    lowerLayout(syn);
  }

private:
  struct Sym {
    enum State : uint8_t { Value, Unreadable, Pending, Alias, Resolving, Error };
    std::string_view name;
    State st = Value;
    PortRef value;
    uint32_t tok = 0;          // defining token
    uint32_t rhs = kNone;      // Pending/Alias: the binding's rhs
    uint32_t boundTok = kNone; // outputs: the binding that drives it
  };
  struct Work {
    uint32_t ast;
    PortRef to;
  };

  std::string_view text(uint32_t tok) const noexcept { return src_.substr(toks_[tok].offset, toks_[tok].length); }

  std::string where(uint32_t tok) const {
    if (tok == kNone) return "?";
    uint32_t line = 1, col = 1;
    const uint32_t off = toks_[tok].offset;
    for (uint32_t i = 0; i < off && i < src_.size(); ++i) {
      if (src_[i] == '\n') {
        line++;
        col = 1;
      } else if ((static_cast<unsigned char>(src_[i]) & 0xC0) != 0x80) {
        col++;
      }
    }
    return std::to_string(line) + ":" + std::to_string(col);
  }

  void errorTok(uint32_t tok, std::string msg) { diags_.add(toks_[tok].offset, toks_[tok].length, std::move(msg)); }
  void errorAst(uint32_t node, std::string msg) {
    const AstNode& n = ast_[node];
    // Point at the whole `name.field` for port selections, else at the main token.
    if ((n.kind == AstKind::NamePort || n.kind == AstKind::AtomRef) && n.tok2 != kNone) {
      const uint32_t b = toks_[n.tok].offset, e = toks_[n.tok2].offset + toks_[n.tok2].length;
      diags_.add(b, e - b, std::move(msg));
      return;
    }
    errorTok(n.tok, std::move(msg));
  }

  Span spanOf(uint32_t tok) const noexcept { return {toks_[tok].offset, toks_[tok].length}; }
  NodeIndex track(NodeIndex n, uint32_t tok) { return track(n, spanOf(tok)); }
  NodeIndex track(NodeIndex n, Span span) {
    if (nodeSpan_->size() <= n) nodeSpan_->resize(n + 1);
    (*nodeSpan_)[n] = span;
    if (claimed_.size() < p_->portCount()) claimed_.resize(p_->portCount(), 0);
    return n;
  }

  bool claimed(PortRef in) const { return claimed_[p_->flatInput(in.node, in.port)] != 0; }
  void bind(uint32_t ast, PortRef to) {
    claimed_[p_->flatInput(to.node, to.port)] = 1;
    work_.push_back({ast, to});
  }

  bool checkNewName(uint32_t tok) {
    const std::string_view name = text(tok);
    // `track` and `sidechain` too: a local of that name would make `track.in` ambiguous.
    if (isReservedName(name) || name == "track" || name == "sidechain") {
      errorTok(tok, "'" + std::string(name) +
                        "' is a built-in name (voice.*, transport.*, track.in, sidechain.in, sr) and cannot be redefined");
      return false;
    }
    return true;
  }

  uint32_t declare(uint32_t tok, Sym::State st, PortRef value) {
    const std::string_view name = text(tok);
    if (st != Sym::Pending && !checkNewName(tok)) st = Sym::Error;
    const auto index = static_cast<uint32_t>(syms_.size());
    if (!names_.insert(name, index)) {
      const Sym& first = syms_[*names_.find(name)];
      errorTok(tok, "'" + std::string(name) + "' is already defined (at " + where(first.tok) + ")");
      return kNone;
    }
    Sym s;
    s.name = name;
    s.st = st;
    s.value = value;
    s.tok = tok;
    syms_.push_back(s);
    return index;
  }

  std::string cannotBind(const Sym& sym, std::string_view name) const {
    if (sym.st == Sym::Unreadable)
      return "export '" + std::string(name) + "' is bound in its declaration ('export " + std::string(name) + " = …')";
    if (sym.st == Sym::Value && sym.value.node != kNone) {
      const NodeKind k = p_->node(sym.value.node).kind;
      if (k == NodeKind::Param || k == NodeKind::Input || k == NodeKind::Unknown)
        return "cannot bind '" + std::string(name) + "': it is " +
               (k == NodeKind::Param ? "a param" : k == NodeKind::Input ? "an input" : "a solve unknown") +
               " (declared at " + where(sym.tok) + ")";
    }
    return "'" + std::string(name) + "' is already defined (at " + where(sym.tok) + ")";
  }

  // The name token at the root of an alias-like rhs (`x`, `x.lp`, `x.lp.y`), if that name is a symbol; else kNone.
  uint32_t aliasBase(uint32_t node) const {
    while (ast_[node].kind == AstKind::Field) node = ast_[node].a;
    const AstNode& n = ast_[node];
    if (n.kind != AstKind::Name && n.kind != AstKind::NamePort) return kNone;
    return names_.find(text(n.tok)) ? n.tok : kNone;
  }

  // Applies the port selections of an alias-like rhs (`x.lp`, `x.lp.hp`): each selects an output of the same node,
  // so the outermost wins; every one is checked so a typo anywhere is reported.
  PortRef applyFields(uint32_t node, PortRef base) {
    std::vector<uint32_t> chain;  // Field nodes, outer → inner
    uint32_t root = node;
    while (ast_[root].kind == AstKind::Field) {
      chain.push_back(root);
      root = ast_[root].a;
    }
    PortRef v = base;
    if (ast_[root].kind == AstKind::NamePort) {
      v = selectOutput(base.node, ast_[root].tok2, root);
      if (v.node == kNone) return v;
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      v = selectOutput(base.node, ast_[*it].tok2, *it);
      if (v.node == kNone) return v;
    }
    return v;
  }

  PortRef selectOutput(NodeIndex n, uint32_t fieldTok, uint32_t astNode) {
    const auto port = findPortByText(*p_, n, text(fieldTok), false);
    if (!port) {
      std::string outs;
      for (uint32_t i = 0; i < p_->node(n).numOutputs; ++i) {
        if (!outs.empty()) outs += ", ";
        outs += portTextName(*p_, n, i, false);
      }
      errorAst(astNode, describeNode(n) + " has no output '" + std::string(text(fieldTok)) + "'" +
                            (outs.empty() ? std::string(" (it has no outputs)") : " (outputs: " + outs + ")"));
      return {};
    }
    return PortRef{n, static_cast<uint16_t>(*port)};
  }

  std::string describeNode(NodeIndex n) const {
    const Node& nd = p_->node(n);
    if (!p_->nodeName(n).empty()) return "'" + p_->nodeName(n) + "'";
    if (nd.kind == NodeKind::Atom) return "'" + std::string(atomSignature(nd.atom).name) + "'";
    if (nd.kind == NodeKind::Subcircuit) return "'" + p_->subcircuit(n).patch + "'";
    return "this " + std::string(toText(nd.kind));
  }
  std::string describeValue(uint32_t node) const {
    const AstNode& n = ast_[node];
    if (n.kind == AstKind::Name || n.kind == AstKind::Call) return "'" + std::string(text(n.tok)) + "'";
    if (n.kind == AstKind::AtomRef) return "'" + std::string(atomSignature(n.atom).name) + "'";
    return "this expression";
  }

  void resolveAliases() {
    std::vector<uint32_t> stack;
    for (uint32_t s0 = 0; s0 < syms_.size(); ++s0) {
      if (syms_[s0].st != Sym::Alias) continue;
      syms_[s0].st = Sym::Resolving;
      stack.push_back(s0);
      while (!stack.empty()) {
        const uint32_t t = stack.back();
        const uint32_t baseTok = aliasBase(syms_[t].rhs);
        const uint32_t u = *names_.find(text(baseTok));
        Sym& us = syms_[u];
        if (us.st == Sym::Alias) {
          us.st = Sym::Resolving;
          stack.push_back(u);
          continue;
        }
        if (us.st == Sym::Resolving) {
          std::string cycle;
          for (uint32_t k : stack) cycle += std::string(syms_[k].name) + " = ";
          cycle += std::string(us.name);
          errorTok(syms_[stack.front()].tok, "names refer to each other in a cycle with no node: " + cycle);
          for (uint32_t k : stack) syms_[k].st = Sym::Error;
          stack.clear();
          break;
        }
        Sym& ts = syms_[t];
        if (us.st == Sym::Error) {
          ts.st = Sym::Error;
        } else if (us.st == Sym::Unreadable) {
          errorAst(ts.rhs, unreadable(us));
          ts.st = Sym::Error;
        } else {
          const PortRef v = applyFields(ts.rhs, us.value);
          ts.st = v.node == kNone ? Sym::Error : Sym::Value;
          ts.value = v;
        }
        stack.pop_back();
      }
    }
  }

  std::string unreadable(const Sym& s) const {
    if (p_->node(s.value.node).kind == NodeKind::Output)
      return "'" + std::string(s.name) + "' is an output and cannot be read; bind the value to a name and use that";
    return "export '" + std::string(s.name) + "' cannot be read; use the value it exports";
  }

  void bindPort(const Member& m) {
    const std::string_view name = text(m.nameTok);
    const uint32_t* s = names_.find(name);
    if (!s) {
      errorTok(m.nameTok, "unknown name '" + std::string(name) + "'");
      return;
    }
    const Sym& sym = syms_[*s];
    if (sym.st == Sym::Error) return;
    const NodeIndex n = sym.value.node;
    const NodeKind k = p_->node(n).kind;
    if (sym.st != Sym::Value || (k != NodeKind::Atom && k != NodeKind::Subcircuit && k != NodeKind::Const)) {
      errorTok(m.nameTok, "only inputs of a computed node can be bound with '" + std::string(name) + "." +
                              std::string(text(m.fieldTok)) + " = …'");
      return;
    }
    const auto port = findPortByText(*p_, n, text(m.fieldTok), true);
    if (!port) {
      errorTok(m.fieldTok, describeNode(n) + " has no input '" + std::string(text(m.fieldTok)) + "'");
      return;
    }
    const PortRef to{n, static_cast<uint16_t>(*port)};
    if (claimed(to)) {
      errorTok(m.fieldTok, "input '" + std::string(text(m.fieldTok)) + "' of " + describeNode(n) + " is already connected");
      return;
    }
    bind(m.expr, to);
  }

  // A zero-input built-in atom by plain name (`sr`, `pulse`), used when no local name matches.
  std::optional<AtomId> bareAtom(std::string_view name) const {
    const auto id = AtomRegistry::builtin().find(name);
    if (id && atomSignature(*id).inputs.empty()) return id;
    return std::nullopt;
  }

  PortRef resolveName(uint32_t node) {
    const AstNode& n = ast_[node];
    const std::string_view name = text(n.tok);
    const uint32_t* s = names_.find(name);
    if (!s) {
      if (const auto id = bareAtom(name)) {
        const NodeIndex a = track(p_->addAtom(*id), n.tok);
        if (n.kind == AstKind::NamePort) return selectOutput(a, n.tok2, node);
        return {a, 0};
      }
      if (name == "voice" || name == "transport" || name == "track" || name == "sidechain") {
        if (n.kind == AstKind::NamePort)
          errorAst(node, "'" + std::string(name) + "." + std::string(text(n.tok2)) + "' is not a built-in input");
        else
          errorAst(node, "'" + std::string(name) + "' is a namespace; use one of its inputs, e.g. " +
                             (name == "voice" ? "voice.hz" : name == "transport" ? "transport.bpm" : std::string(name) + ".in"));
        return {};
      }
      errorAst(node, "unknown name '" + std::string(name) + "'");
      return {};
    }
    const Sym& sym = syms_[*s];
    switch (sym.st) {
      case Sym::Value:
        if (n.kind == AstKind::NamePort) return selectOutput(sym.value.node, n.tok2, node);
        return sym.value;
      case Sym::Unreadable: errorAst(node, unreadable(sym)); return {};
      default: return {};  // already reported
    }
  }

  // Creates the node for an expression (its children are queued on the work list) and returns its value.
  PortRef materialize(uint32_t node) {
    const AstNode& n = ast_[node];
    switch (n.kind) {
      case AstKind::Number: return {track(p_->addConst(Constant::real(n.number, n.unit)), n.tok), 0};
      case AstKind::Bool: return {track(p_->addConst(Constant::boolean(n.number != 0.0)), n.tok), 0};
      case AstKind::AtomRef: {
        // `voice.hz`: the span covers the namespace, the dot and the name.
        const uint32_t b = toks_[n.tok].offset, e = toks_[n.tok2].offset + toks_[n.tok2].length;
        return {track(p_->addAtom(n.atom), Span{b, e - b}), 0};
      }
      case AstKind::Name:
      case AstKind::NamePort: return resolveName(node);
      case AstKind::Field: {
        const PortRef base = materialize(n.a);
        if (base.node == kNone) return {};
        return selectOutput(base.node, n.tok2, node);
      }
      case AstKind::Unary: {
        const NodeIndex a = track(p_->addAtom(n.atom), n.tok);
        bind(n.a, {a, 0});
        return {a, 0};
      }
      case AstKind::Binary:
      case AstKind::Index: {
        const NodeIndex a = track(p_->addAtom(n.atom), n.tok);
        bind(n.b, {a, 1});
        bind(n.a, {a, 0});
        return {a, 0};
      }
      case AstKind::Ternary: {
        const NodeIndex a = track(p_->addAtom(n.atom), n.tok);
        bind(n.c, {a, 2});
        bind(n.b, {a, 1});
        bind(n.a, {a, 0});
        return {a, 0};
      }
      case AstKind::Tuple: {
        const NodeIndex a = track(p_->addAtom(n.atom, static_cast<uint16_t>(n.b)), n.tok);
        for (uint32_t i = n.b; i-- > 0;) bind(args_[n.a + i].expr, {a, static_cast<uint16_t>(i)});
        return {a, 0};
      }
      case AstKind::Call: return call(node);
    }
    return {};
  }

  PortRef call(uint32_t node) {
    const AstNode& n = ast_[node];
    const std::string_view name = text(n.tok);
    const uint32_t positional = [&] {
      uint32_t k = 0;
      while (k < n.b && args_[n.a + k].nameTok == kNone) ++k;
      return k;
    }();

    NodeIndex callNode;
    uint32_t repeatedFrom = kNone;  // first repeated (variadic) input; named arguments cannot address those
    if (const SubcircuitRef* ref = patches_.find(name)) {
      if (positional > ref->inputs.size()) {
        errorTok(n.tok, "too many arguments to '" + std::string(name) + "' (" + std::to_string(positional) + " given, it has " +
                            std::to_string(ref->inputs.size()) + " inputs)");
        return {};
      }
      callNode = track(p_->addSubcircuit(*ref), n.tok);
    } else if (const auto id = AtomRegistry::builtin().find(name)) {
      const AtomSignature& sig = atomSignature(*id);
      uint16_t repeat = 0;
      if (sig.has(atom_flags::kVariadicInputs)) {
        const uint32_t fixed = sig.fixedInputs();
        const uint32_t extra = positional > fixed ? positional - fixed : 0;
        if (extra < sig.minRepeat) {
          errorTok(n.tok, "'" + std::string(name) + "' needs at least " + std::to_string(fixed + sig.minRepeat) +
                              " positional arguments");
          return {};
        }
        if (extra > 0xFFF) {
          errorTok(n.tok, "too many arguments to '" + std::string(name) + "'");
          return {};
        }
        repeat = static_cast<uint16_t>(extra);
        repeatedFrom = fixed;
      } else if (positional > sig.inputs.size()) {
        errorTok(n.tok, "too many arguments to '" + std::string(name) + "' (" + std::to_string(positional) + " given, it has " +
                            std::to_string(sig.inputs.size()) + " inputs)");
        return {};
      }
      callNode = track(p_->addAtom(*id, repeat), n.tok);
    } else {
      errorTok(n.tok, "unknown function '" + std::string(name) + "' (not a patch in this file, an imported patch or an atom)");
      return {};
    }

    for (uint32_t i = 0; i < positional; ++i) bind(args_[n.a + i].expr, {callNode, static_cast<uint16_t>(i)});
    for (uint32_t i = positional; i < n.b; ++i) {
      const AstArg& a = args_[n.a + i];
      const std::string_view argName = text(a.nameTok);
      std::optional<uint32_t> port;
      for (uint32_t k = 0; k < p_->node(callNode).numInputs && k < repeatedFrom; ++k)
        if (p_->inputName(callNode, k) == argName) {
          port = k;
          break;
        }
      if (!port) {
        std::string ins;
        for (uint32_t k = 0; k < p_->node(callNode).numInputs && k < repeatedFrom; ++k) {
          if (!ins.empty()) ins += ", ";
          ins += p_->inputName(callNode, k);
        }
        errorTok(a.nameTok, "'" + std::string(name) + "' has no input '" + std::string(argName) + "'" +
                                (ins.empty() ? std::string() : " (inputs: " + ins + ")"));
        continue;
      }
      const PortRef to{callNode, static_cast<uint16_t>(*port)};
      if (claimed(to)) {
        errorTok(a.nameTok, "input '" + std::string(argName) + "' of '" + std::string(name) + "' is given twice");
        continue;
      }
      bind(a.expr, to);
    }
    calls_.push_back({callNode, n.tok});  // required inputs are checked once `node.input = …` bindings are in
    return {callNode, 0};
  }

  std::optional<NodeIndex> layoutNode(uint32_t tok) {
    const uint32_t* s = names_.find(text(tok));
    if (!s) {
      errorTok(tok, "layout: unknown node '" + std::string(text(tok)) + "'");
      return std::nullopt;
    }
    const Sym& sym = syms_[*s];
    if (sym.st == Sym::Error || sym.value.node == kNone) return std::nullopt;
    return sym.value.node;
  }

  void lowerLayout(const PatchSyn& syn) {
    std::vector<uint8_t> placed(p_->nodeCount(), 0);
    for (const LayoutNodeSyn& ln : syn.layoutNodes) {
      const auto n = layoutNode(ln.nameTok);
      if (!n) continue;
      if (placed[*n]) {
        errorTok(ln.nameTok, "layout: node '" + p_->nodeName(*n) + "' is placed twice");
        continue;
      }
      placed[*n] = 1;
      p_->setLayout(*n, ln.layout);
    }
    if (!syn.layoutWires.empty()) {
      std::vector<WireIndex> byInput(p_->portCount(), kNone);
      for (WireIndex w = 0; w < p_->wireCount(); ++w) {
        const Wire& wr = p_->wire(w);
        byInput[p_->flatInput(wr.to.node, wr.to.port)] = w;
      }
      std::vector<uint8_t> routed(p_->wireCount(), 0);
      for (const LayoutWireSyn& lw : syn.layoutWires) {
        const auto from = layoutNode(lw.fromTok);
        const auto to = layoutNode(lw.toTok);
        if (!from || !to) continue;
        const auto out = findPortByText(*p_, *from, text(lw.fromPortTok), false);
        if (!out) {
          errorTok(lw.fromPortTok, "layout: " + describeNode(*from) + " has no output '" + std::string(text(lw.fromPortTok)) + "'");
          continue;
        }
        const auto in = findPortByText(*p_, *to, text(lw.toPortTok), true);
        if (!in) {
          errorTok(lw.toPortTok, "layout: " + describeNode(*to) + " has no input '" + std::string(text(lw.toPortTok)) + "'");
          continue;
        }
        const WireIndex w = byInput[p_->flatInput(*to, *in)];
        if (w == kNone || !(p_->wire(w).from == PortRef{*from, static_cast<uint16_t>(*out)})) {
          errorTok(lw.fromTok, "layout: there is no wire " + std::string(text(lw.fromTok)) + "." +
                                   std::string(text(lw.fromPortTok)) + " -> " + std::string(text(lw.toTok)) + "." +
                                   std::string(text(lw.toPortTok)));
          continue;
        }
        if (routed[w]) {
          errorTok(lw.fromTok, "layout: this wire is routed twice");
          continue;
        }
        routed[w] = 1;
        p_->setWireVia(w, lw.via);
      }
    }
    for (const LayoutComment& c : syn.comments) p_->addComment(c);
  }

  std::string_view src_;
  const std::vector<Token>& toks_;
  const std::vector<AstNode>& ast_;
  const std::vector<AstArg>& args_;
  const PatchTable& patches_;
  Diags& diags_;

  Patch* p_ = nullptr;
  std::vector<Span>* nodeSpan_ = nullptr;
  std::vector<Sym> syms_;
  NameTable names_;
  std::vector<Work> work_;
  std::vector<uint8_t> claimed_;  // per flat port: an input that already has (or will get) a driver
  std::vector<std::pair<NodeIndex, uint32_t>> calls_;  // call nodes and their name tokens
};

// The signature a call sees, computed from the syntax (callees may be defined later in the file). Matches
// subcircuitSignature() of the lowered patch: ins (optional with a default), params, outs, exports.
SubcircuitRef signatureFromSyntax(const PatchSyn& p, std::string_view name, const Parser& parser) {
  SubcircuitRef r;
  r.patch = std::string(name);
  for (const Member& m : p.members)
    if (m.kind == MemberKind::Input) r.inputs.push_back({std::string(parser.text(m.nameTok)), m.type, m.expr != kNone});
  for (const Member& m : p.members)
    if (m.kind == MemberKind::Param)
      r.inputs.push_back({std::string(parser.text(m.nameTok)),
                          portType(p.params[m.index].kind, Rate::Param, Poly::Global, kWidthMono), true});
  for (const Member& m : p.members)
    if (m.kind == MemberKind::Output) r.outputs.push_back({std::string(parser.text(m.nameTok)), m.type, false});
  for (const Member& m : p.members)
    if (m.kind == MemberKind::Export) r.outputs.push_back({std::string(parser.text(m.nameTok)), PortType{}, false});
  return r;
}

// Maps a validation message to a source span: "patch 'X': …" selects the patch; the first "#N)" (Patch::describe)
// selects the node. Falls back to the patch name.
Span validationSpan(const std::string& msg, const PatchFile& file, const std::vector<Span>& patchSpan,
                    const std::vector<std::vector<Span>>& nodeSpan) {
  constexpr std::string_view kPatch = "patch '";
  const size_t p0 = msg.find(kPatch);
  if (p0 == std::string::npos) return patchSpan.empty() ? Span{} : patchSpan.front();
  const size_t nameBegin = p0 + kPatch.size();
  const size_t nameEnd = msg.find('\'', nameBegin);
  if (nameEnd == std::string::npos) return {};
  const std::string_view name(msg.data() + nameBegin, nameEnd - nameBegin);
  size_t pi = 0;
  while (pi < file.patches.size() && file.patches[pi].name() != name) ++pi;
  if (pi == file.patches.size()) return {};
  for (size_t h = msg.find(", #", nameEnd); h != std::string::npos; h = msg.find(", #", h + 1)) {
    uint32_t index = 0;
    const char* b = msg.data() + h + 3;
    const char* e = msg.data() + msg.size();
    const auto [ptr, ec] = std::from_chars(b, e, index);
    if (ec == std::errc{} && ptr < e && *ptr == ')' && index < nodeSpan[pi].size() && nodeSpan[pi][index].offset != kNone)
      return nodeSpan[pi][index];
  }
  return patchSpan[pi];
}

void computeLocations(std::string_view src, std::string_view fileName, std::vector<Diagnostic>& diags) {
  // Diagnostics are few; sort a copy of their offsets and sweep the source once.
  std::vector<std::pair<uint32_t, size_t>> order;
  order.reserve(diags.size());
  for (size_t i = 0; i < diags.size(); ++i) order.emplace_back(diags[i].offset, i);
  std::sort(order.begin(), order.end());
  uint32_t line = 1, col = 1, pos = 0;
  const std::string file(fileName.empty() ? std::string_view("<input>") : fileName);
  for (const auto& [off, i] : order) {
    const uint32_t target = std::min<uint32_t>(off, static_cast<uint32_t>(src.size()));
    for (; pos < target; ++pos) {
      if (src[pos] == '\n') {
        ++line;
        col = 1;
      } else if ((static_cast<unsigned char>(src[pos]) & 0xC0) != 0x80) {
        ++col;
      }
    }
    Diagnostic& d = diags[i];
    d.location.file = file;
    d.location.line = line;
    d.location.col = col;
  }
}

}  // namespace

std::vector<SubcircuitRef> signatures(const PatchFile& file) {
  std::vector<SubcircuitRef> out;
  out.reserve(file.patches.size());
  for (const Patch& p : file.patches) out.push_back(subcircuitSignature(p));
  return out;
}

ParseResult parseDetailed(std::string_view source, std::string_view fileName, const ParseOptions& options) {
  ParseResult result;
  Diags diags;

  std::vector<Token> toks;
  std::vector<LexError> lexErrors;
  lex(source, toks, lexErrors);
  for (LexError& e : lexErrors) diags.add(e.offset, e.length, std::move(e.message));

  Parser parser(source, toks, diags);
  parser.ast.reserve(toks.size() / 2 + 8);
  std::vector<ImportSyn> imports;
  std::vector<PatchSyn> patches;
  parser.parseFile(imports, patches);

  auto finish = [&]() -> ParseResult {
    computeLocations(source, fileName, diags.list);
    result.diagnostics = std::move(diags.list);
    return std::move(result);
  };
  if (!diags.list.empty()) return finish();

  for (const ImportSyn& imp : imports) result.file.imports.push_back(imp.path);

  // Callable patches: this file's (by syntax, so order does not matter), then externals, then imported files.
  std::vector<SubcircuitRef> local;
  local.reserve(patches.size());
  {
    NameTable seen;
    seen.reset(patches.size());
    for (uint32_t i = 0; i < patches.size(); ++i) {
      const std::string_view name = parser.text(patches[i].nameTok);
      const Token& t = toks[patches[i].nameTok];
      if (!seen.insert(name, i)) {
        diags.add(t.offset, t.length, "patch '" + std::string(name) + "' is defined twice");
        continue;
      }
      if (AtomRegistry::builtin().find(name))
        diags.add(t.offset, t.length, "patch '" + std::string(name) + "' has the name of a built-in atom");
      if (isReservedName(name)) diags.add(t.offset, t.length, "'" + std::string(name) + "' is a built-in name");
      local.push_back(signatureFromSyntax(patches[i], name, parser));
    }
  }
  std::vector<SubcircuitRef> imported;
  if (options.imports) {
    for (const ImportSyn& imp : imports) {
      Result<std::vector<SubcircuitRef>> r = options.imports->load(imp.path, fileName);
      const Token& t = toks[imp.tok];
      if (!r) {
        diags.add(t.offset, t.length, "cannot import \"" + imp.path + "\": " + r.error().message);
        continue;
      }
      for (SubcircuitRef& s : r.value()) imported.push_back(std::move(s));
    }
  }
  PatchTable table;
  table.index.reset(local.size() + options.externals.size() + imported.size());
  for (const SubcircuitRef& r : local) table.add(r);
  for (const SubcircuitRef& r : options.externals) table.add(r);
  for (const SubcircuitRef& r : imported) table.add(r);
  if (!diags.list.empty()) return finish();

  // Lowering.
  Lowerer lowerer(source, toks, parser, table, diags);
  std::vector<std::vector<Span>> nodeSpan(patches.size());
  std::vector<Span> patchSpan;
  result.file.patches.reserve(patches.size());
  for (size_t i = 0; i < patches.size(); ++i) {
    result.file.patches.emplace_back(std::string(parser.text(patches[i].nameTok)));
    patchSpan.push_back({toks[patches[i].nameTok].offset, toks[patches[i].nameTok].length});
    lowerer.lower(patches[i], result.file.patches.back(), nodeSpan[i]);
    if (diags.full()) break;
  }
  if (!diags.list.empty()) return finish();

  if (options.validate) {
    for (Error& e : result.file.validateAll()) {
      const Span span = validationSpan(e.message, result.file, patchSpan, nodeSpan);
      diags.add(span.offset == kNone ? 0 : span.offset, span.length, std::move(e.message),
                e.code == ErrorCode::Type ? ErrorCode::Type : ErrorCode::Parse);
    }
  }
  // Node spans for later phases (inference reports at the node, T1.3).
  for (size_t i = 0; i < nodeSpan.size(); ++i) nodeSpan[i].resize(result.file.patches[i].nodeCount());
  result.nodeSpans = std::move(nodeSpan);
  return finish();
}

std::string formatDiagnostic(std::string_view source, const Diagnostic& d) {
  std::string s = d.location.file.empty() ? std::string("<input>") : d.location.file;
  s += ':';
  s += std::to_string(d.location.line);
  s += ':';
  s += std::to_string(d.location.col);
  s += ": error: ";
  s += d.message;
  s += '\n';
  // The source line containing the offset.
  const size_t off = std::min<size_t>(d.offset, source.size());
  size_t begin = off;
  while (begin > 0 && source[begin - 1] != '\n') --begin;
  size_t end = off;
  while (end < source.size() && source[end] != '\n') ++end;
  std::string_view line = source.substr(begin, end - begin);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  s += line;
  s += '\n';
  // Caret: tabs are kept so the caret lines up; one column per code point.
  for (size_t i = begin; i < off && i - begin < line.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(source[i]);
    if ((c & 0xC0) == 0x80) continue;
    s += c == '\t' ? '\t' : ' ';
  }
  s += '^';
  const size_t spanEnd = std::min<size_t>(off + d.length, begin + line.size());
  for (size_t i = off + 1; i < spanEnd; ++i)
    if ((static_cast<unsigned char>(source[i]) & 0xC0) != 0x80) s += '~';
  return s;
}

Diagnostic makeDiagnostic(std::string_view source, std::string_view fileName, SourceSpan span, std::string message,
                          ErrorCode code) {
  std::vector<Diagnostic> one(1);
  one[0].message = std::move(message);
  one[0].code = code;
  one[0].offset = span.offset == kNone ? 0 : std::min<uint32_t>(span.offset, static_cast<uint32_t>(source.size()));
  one[0].length = span.offset == kNone ? 0 : span.length;
  computeLocations(source, fileName, one);
  return std::move(one[0]);
}

Result<PatchFile> parse(std::string_view source, std::string_view fileName, const ParseOptions& options) {
  ParseResult r = parseDetailed(source, fileName, options);
  if (r.ok()) return std::move(r.file);
  Error e;
  e.code = r.diagnostics.front().code;
  e.location = r.diagnostics.front().location;
  for (size_t i = 0; i < r.diagnostics.size(); ++i) {
    if (i) e.message += '\n';
    e.message += formatDiagnostic(source, r.diagnostics[i]);
  }
  return e;
}

}  // namespace pq::graph
