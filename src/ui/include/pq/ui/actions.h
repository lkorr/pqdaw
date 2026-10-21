#pragma once

// Actions and keymaps (T3.1). An action is a named command ("transport.play", "edit.duplicate") with a
// handler and an optional enabled predicate. A keymap is data: per scope, chord → action id. Scopes are
// named areas ("global", "arrangement", "pianoroll", later "macro"); the UiTree decides which are active
// (pq/ui/widget.h, UiTree::dispatchKey) and resolves keymap ids to registry indices once, so dispatch never
// hashes or compares strings.
//
// Registration and binding allocate (startup / keymap load); lookups and invoke() do not (the handlers
// themselves must not allocate either if the caller needs that).

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/ui/input.h"

namespace pq::ui {

using ActionIndex = uint32_t;
inline constexpr ActionIndex kNoAction = UINT32_MAX;

struct ActionOptions {
  bool repeatable = false;  // fires on key auto-repeat too (e.g. nudge note up); toggles should not
};

class ActionRegistry {
public:
  using Handler = std::function<void()>;
  using Enabled = std::function<bool()>;

  // Registers an action. Fails (InvalidArgument) on an empty or duplicate id or a null handler.
  Result<ActionIndex> add(std::string id, Handler handler, Enabled enabled = {}, ActionOptions options = {});
  // Registers the action, or replaces the handler, predicate and options of an already registered one (its index
  // stays the same, so resolved keymaps keep working). For views that can be rebuilt over the same registry: the
  // newest instance takes the action over, and a destroyed one parks it with a disabled predicate (T3.8).
  // Fails like add() on an empty id or a null handler.
  Result<ActionIndex> set(std::string id, Handler handler, Enabled enabled = {}, ActionOptions options = {});
  ActionIndex find(std::string_view id) const noexcept;  // kNoAction when unknown
  size_t size() const noexcept { return actions_.size(); }
  std::string_view id(ActionIndex a) const noexcept { return actions_[a].id; }
  const ActionOptions& options(ActionIndex a) const noexcept { return actions_[a].options; }
  // True when the action has no predicate or its predicate returns true.
  bool enabled(ActionIndex a) const;
  // Runs the handler if enabled. Returns whether it ran.
  bool invoke(ActionIndex a) const;

private:
  struct Action {
    std::string id;
    Handler handler;
    Enabled enabledFn;
    ActionOptions options;
  };
  std::vector<Action> actions_;
};

using ScopeId = uint16_t;
inline constexpr ScopeId kGlobalScope = 0;  // always active, consulted last
inline constexpr ScopeId kNoScope = UINT16_MAX;

// How a scope treats keys it has no binding for. Leaves room for macro mode (SPEC §6.2): the macro scope
// takes over plain keys (swallowUnbound) while chords with Ctrl or Alt pass through to the Ableton bindings
// below it (passthroughMods = Ctrl|Alt), like the AHK `*` wildcard.
struct ScopeRules {
  Mods passthroughMods = mod::kNone;  // a key with any of these modifiers skips this scope entirely
  bool swallowUnbound = false;        // an unbound key (not passed through) stops routing here
};

class Keymap {
public:
  Keymap();  // creates the "global" scope (kGlobalScope)

  // Interns a scope name (creates it on first use).
  ScopeId scope(std::string_view name);
  ScopeId findScope(std::string_view name) const noexcept;  // kNoScope when unknown
  std::string_view scopeName(ScopeId s) const noexcept { return scopes_[s].name; }
  size_t scopeCount() const noexcept { return scopes_.size(); }
  void setRules(ScopeId s, const ScopeRules& rules) { scopes_[s].rules = rules; }
  const ScopeRules& rules(ScopeId s) const noexcept { return scopes_[s].rules; }

  // Binds chord → action id in a scope. Fails on a chord that is already bound in that scope, on a modifier
  // key used as the chord's key, and (string overload) on an unparsable chord.
  Status bind(ScopeId scope, Chord chord, std::string actionId);
  Status bind(std::string_view scope, std::string_view chord, std::string_view actionId);

  struct Binding {
    ScopeId scope;
    Chord chord;
    std::string action;
  };
  std::span<const Binding> bindings() const noexcept { return bindings_; }
  // The action id bound to `chord` in `scope`, or empty.
  std::string_view lookup(ScopeId scope, Chord chord) const noexcept;

private:
  struct Scope {
    std::string name;
    ScopeRules rules;
  };
  std::vector<Scope> scopes_;
  std::vector<Binding> bindings_;
};

}  // namespace pq::ui
