#pragma once

// UiTree's drag session state (T5.7; private to pq_ui: ui_tree.cpp and drag.cpp).

#include "pq/ui/drag.h"
#include "pq/ui/widget.h"

namespace pq::ui {

struct UiTree::DragState {
  DragPayload payload;
  Vec2 pos{0, 0};
  Mods mods = mod::kNone;
  Widget* target = nullptr;  // the widget whose dragOver accepted last (null: none)
  DropFeedback feedback;
  IRect ghost{};             // where the ghost was last laid out (damaged on every move)
  bool scrolling = false;    // a widget under the pointer auto-scrolls: frames keep coming
  double lastTick = -1.0;    // update() time of the last auto-scroll step
};

}  // namespace pq::ui
