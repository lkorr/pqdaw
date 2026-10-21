#pragma once

// The T0.8 / T0.9 test scenes (registered in scenes.cpp).

#include "pq/ui/draw_list.h"
#include "pq/ui/scenes.h"

namespace pq::ui::detail {

void textScene(DrawList& dl, const SceneTextures& tex, double time);
void linesScene(DrawList& dl, const SceneTextures& tex, double time);

}  // namespace pq::ui::detail
