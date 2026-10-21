#include "pq/ui/actions.h"

#include <string>

namespace pq::ui {

Result<ActionIndex> ActionRegistry::add(std::string id, Handler handler, Enabled enabled, ActionOptions options) {
  if (id.empty()) return Error{ErrorCode::InvalidArgument, "action id is empty", {}};
  if (!handler) return Error{ErrorCode::InvalidArgument, "action '" + id + "' has no handler", {}};
  if (find(id) != kNoAction) return Error{ErrorCode::InvalidArgument, "action '" + id + "' is already registered", {}};
  actions_.push_back({std::move(id), std::move(handler), std::move(enabled), options});
  return static_cast<ActionIndex>(actions_.size() - 1);
}

Result<ActionIndex> ActionRegistry::set(std::string id, Handler handler, Enabled enabled, ActionOptions options) {
  const ActionIndex existing = find(id);
  if (existing == kNoAction) return add(std::move(id), std::move(handler), std::move(enabled), options);
  if (!handler) return Error{ErrorCode::InvalidArgument, "action '" + id + "' has no handler", {}};
  Action& a = actions_[existing];
  a.handler = std::move(handler);
  a.enabledFn = std::move(enabled);
  a.options = options;
  return existing;
}

ActionIndex ActionRegistry::find(std::string_view id) const noexcept {
  for (size_t i = 0; i < actions_.size(); ++i)
    if (actions_[i].id == id) return static_cast<ActionIndex>(i);
  return kNoAction;
}

bool ActionRegistry::enabled(ActionIndex a) const {
  const Action& act = actions_[a];
  return !act.enabledFn || act.enabledFn();
}

bool ActionRegistry::invoke(ActionIndex a) const {
  if (a >= actions_.size() || !enabled(a)) return false;
  actions_[a].handler();
  return true;
}

Keymap::Keymap() { scopes_.push_back({"global", {}}); }

ScopeId Keymap::scope(std::string_view name) {
  if (const ScopeId s = findScope(name); s != kNoScope) return s;
  scopes_.push_back({std::string(name), {}});
  return static_cast<ScopeId>(scopes_.size() - 1);
}

ScopeId Keymap::findScope(std::string_view name) const noexcept {
  for (size_t i = 0; i < scopes_.size(); ++i)
    if (scopes_[i].name == name) return static_cast<ScopeId>(i);
  return kNoScope;
}

Status Keymap::bind(ScopeId scope, Chord chord, std::string actionId) {
  if (scope >= scopes_.size()) return Error{ErrorCode::InvalidArgument, "unknown keymap scope", {}};
  if (isModifierKey(chord.key) || chord.key == key::kUnknown)
    return Error{ErrorCode::InvalidArgument, "chord '" + formatChord(chord) + "' has no non-modifier key", {}};
  chord.mods = static_cast<Mods>(chord.mods & mod::kChordMask);
  for (const Binding& b : bindings_)
    if (b.scope == scope && b.chord == chord)
      return Error{ErrorCode::InvalidArgument,
                   "chord '" + formatChord(chord) + "' is bound twice in scope '" + scopes_[scope].name + "' ('" +
                       b.action + "', '" + actionId + "')",
                   {}};
  bindings_.push_back({scope, chord, std::move(actionId)});
  return Ok{};
}

Status Keymap::bind(std::string_view scopeName, std::string_view chordText, std::string_view actionId) {
  auto chord = parseChord(chordText);
  if (!chord) return std::move(chord).error();
  return bind(scope(scopeName), *chord, std::string(actionId));
}

std::string_view Keymap::lookup(ScopeId scope, Chord chord) const noexcept {
  for (const Binding& b : bindings_)
    if (b.scope == scope && b.chord == chord) return b.action;
  return {};
}

}  // namespace pq::ui
