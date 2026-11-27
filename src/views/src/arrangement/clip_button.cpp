// SX.2: the clip-menu button on a selected audio clip's title strip (internal.h clipMenuButton: its geometry and hit
// zone; a press opens the radial clip menu, pq/views/audio_clip/clip_menu.h).

#include <algorithm>
#include <cmath>

#include "lane_paint.h"

namespace pq::views::arrangement {

using namespace pq::ui;

// A disc in the title's text colour with four dots in the clip colour around its centre (a ring of bubbles: the menu
// it opens); brighter while hovered. Only the selected clip draws it: a handful of vertices per frame.
void paintClipMenuButton(LanePaint& lp, const ClipCols& cc, float top, float titleB, Rgba colour, bool hovered) {
  const float scale = lp.ctx.scale.factor;
  const ClipMenuButton b =
      clipMenuButton(cc.cx0, cc.cx1, static_cast<float>(lp.r.x), static_cast<float>(lp.r.right()), top, titleB, scale);
  if (!b.show || b.c.x + b.r < cc.vx0 || b.c.x - b.r > cc.vx1) return;
  DrawList& dl = lp.ctx.dl;
  const Rgba ink = lp.m.clipTextColour(lp.th, colour);
  dl.addCircleFilled(b.c, b.r, withAlpha(ink, hovered ? 240 : 185));
  const float d = 0.48f * b.r, dot = std::max(1.0f, 0.2f * b.r);
  const Vec2 at[] = {{b.c.x, b.c.y - d}, {b.c.x + d, b.c.y}, {b.c.x, b.c.y + d}, {b.c.x - d, b.c.y}};
  for (const Vec2& p : at) dl.addCircleFilled(p, dot, colour);
}

float clipTitleRight(const LanePaint& lp, const ClipCols& cc, float top, float titleB, bool selected) {
  const float right = std::min(cc.cx1, static_cast<float>(lp.r.right()));
  if (!selected) return right;
  const ClipMenuButton b =
      clipMenuButton(cc.cx0, cc.cx1, static_cast<float>(lp.r.x), static_cast<float>(lp.r.right()), top, titleB, lp.ctx.scale.factor);
  return b.show ? b.c.x - b.r : right;
}

}  // namespace pq::views::arrangement
