#pragma once

// GU.8 (docs/plans/GU.md "Panels and the panel window": "Named style schemas (save / apply a set)"): STYLE SCHEMAS —
// named sets of an element kind's style fields ("Minimal" knob, "Classic" knob …). The panel window's inspector saves
// the selected element's style as one and applies one to the selected elements (one undo step: PanelWindow::
// editSelected; elements of another kind are left as they are).
//
//   what a schema holds   the element's fields its kind's registry schema declares as Number, Choice or Token (shape,
//                         sizes, colours as theme tokens, readout position). Text fields (a label, a tabs element's
//                         items, a button's text) are content, not style: never saved, never overwritten.
//   built-ins             a few per generic kind (knob: Classic, Minimal, Pie; slider: Classic, Minimal), always
//                         listed first; a user schema of a built-in's name is refused (save renames it "<name> 2").
//   user schemas          per kind by name (a save under an existing user name replaces it); kept in one JSON file
//                         when a file is set (the app: <User Library>\Styles\schemas.json, written atomically on every
//                         save), else for the session only (tests).
// The document never sees a schema: applying one writes ordinary style fields (SetGroupPanel), so a panel carries its
// look without the schema it came from.
//
// UI thread only. Not RT code.

#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/panel.h"

namespace pq::views::panel {

struct StyleSchema {
  std::string kind;  // the element kind it is for ("knob")
  std::string name;  // "Minimal"
  std::vector<doc::StyleField> fields;  // sorted by name, unique
  bool builtIn = false;
};

// The fields of `e` a schema of its kind carries (above).
std::vector<doc::StyleField> schemaFields(const doc::PanelElement& e);
// `e` with schema `s`'s fields set (the others kept). False (unchanged): `e` is another kind.
bool applyStyleSchema(const StyleSchema& s, doc::PanelElement& e);

class StyleSchemas {
public:
  // The process's schemas (the views share one set).
  static StyleSchemas& shared();
  // Kind `kind`'s schemas: the built-ins, then the user's by name.
  std::vector<const StyleSchema*> forKind(std::string_view kind) const;
  const StyleSchema* find(std::string_view kind, std::string_view name) const;
  // Saves `e`'s style as a user schema `name` of its kind (trimmed; "" → "Style"); returns the name it got.
  Result<std::string> save(const doc::PanelElement& e, std::string_view name);
  // Where user schemas are kept: read now (missing = none; an unreadable file: an error, nothing loaded), written on
  // every save. "" = none (session only).
  Status setFile(std::string pathUtf8);
  // Removes every user schema (tests).
  void clearUser();

private:
  StyleSchemas();
  Status write() const;
  std::vector<StyleSchema> builtIn_, user_;
  std::string file_;
};

}  // namespace pq::views::panel
