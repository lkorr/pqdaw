// The circuit canvas (T4.2): view transform, level of detail, symbol tiles, wire drawing, culling, hit testing,
// selection and input. See pq/views/circuit/circuit_canvas.h.

#include "pq/views/circuit/circuit_canvas.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>

#include "../common/drag.h"
#include "../common/paint.h"
#include "pq/graph/atoms.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/lines.h"
#include "pq/ui/raster.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/tile_atlas.h"
#include "pq/views/circuit/cables.h"

namespace pq::views::circuit {

using graph::Point2;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr uint32_t kTileBuildTexelsPerFrame = 1u << 20;

// The scene node a wire's end moves with (TX.drag-grow: SceneWire::fromNode / toNode, else its pin's); UINT32_MAX: none.
uint32_t wireEnd(const CircuitScene& sc, const SceneWire& sw, bool to) noexcept {
  const uint32_t n = to ? sw.toNode : sw.fromNode;
  if (n != UINT32_MAX) return n;
  const uint32_t p = to ? sw.toPin : sw.fromPin;
  return p < sc.pins.size() ? sc.pins[p].node : UINT32_MAX;
}

constexpr float kInactiveAlpha = 0.3f;  // T4.15: inactive parts shown with Show Inactive

// One body's two coverage tiles at one scale.
struct BodyTile {
  float scale = 0.0f;  // device px per world unit it was rasterised at
  float margin = 0.0f; // device px around the body box
  ui::TileAtlas::Tile fill, stroke;
  bool failed = false;
};

int32_t snapTop(float centre, float t) noexcept { return static_cast<int32_t>(std::floor(centre - t * 0.5f + 0.5f)); }
float snapCentre(float centre, float t) noexcept { return static_cast<float>(snapTop(centre, t)) + t * 0.5f; }

}  // namespace

struct CircuitCanvas::Impl final : FaceHost {
  CircuitCanvas& self;
  std::shared_ptr<const CircuitScene> scene;
  View view;
  bool fitPending = false;
  bool fitReadable = false;  // M15: the pending / next fit is an open (readable), not F (the whole patch)
  bool stubsNearOnly = false;  // T5.x-circuit-chain
  double pillMinZoom = 0.0;    // T5.x-circuit-chain
  bool fitLeft = false;        // TMM.13-fix2: setFitAlignLeft
  Stats stats;

  Selection selection;
  std::vector<uint8_t> nodeSel, wireSel, wireLit;
  CanvasHit hover;

  // input
  enum class DragMode : uint8_t { None, Pan, Band, Click };
  DragMode mode = DragMode::None;
  drag::Tracker tracker;
  Vec2 panLast{0, 0};
  Selection bandBase;  // the selection before a Shift band
  bool bandAdd = false;

  // zoom settle and tiles
  bool zooming = false;
  uint32_t settleTimer = 0;
  bool tilesWanted = false;
  std::unordered_map<uint64_t, BodyTile> tiles;  // by SymbolSpec::bodyKey
  ui::DrawList scratch, scratchOther;
  std::vector<uint8_t> raster;

  // Far level: world-anchored chunks (chunk (i, j) covers device-world px [i·C, (i+1)·C) × [j·C, (j+1)·C) at scale s).
  static constexpr float kChunk = 256.0f;
  static constexpr uint32_t kChunksPerFrame = 8;
  static constexpr int64_t kChunkBuildMicros = 300;  // build budget per frame (at least one chunk)
  struct ChunkKey {
    uint32_t scale;  // bits of the float scale
    int32_t i, j;
    bool operator==(const ChunkKey&) const = default;
  };
  struct ChunkHash {
    size_t operator()(const ChunkKey& k) const noexcept {
      return std::hash<uint64_t>()((static_cast<uint64_t>(k.scale) << 32) ^ (static_cast<uint64_t>(static_cast<uint32_t>(k.i)) << 16) ^
                                   static_cast<uint32_t>(k.j));
    }
  };
  struct FarChunk {
    ui::TileAtlas::Tile wires, nodes;
    bool hasWires = false, hasNodes = false, failed = false;
    bool visited = false;  // built (an empty chunk has no tiles)
  };
  std::unordered_map<ChunkKey, FarChunk, ChunkHash> chunks;
  float chunkScale = 0.0f;  // the scale whose chunks covered the whole view last (the stretch source)
  int32_t farI0 = 0, farI1 = -1, farJ0 = 0, farJ1 = -1;  // chunks the last Far paint showed
  float farScale = 0.0f;
  Rgba farWireCol = 0, farNodeCol = 0;
  float farThin = 1.0f;

  static uint32_t bitsOf(float f) noexcept {
    uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
  }

  // Far-level geometry of the world rect into `dlW` (wires) and `dlN` (nodes), world → device = o + w·s.
  void drawFarRegion(ui::DrawList& dlW, ui::DrawList& dlN, double ox, double oy, float sc, float wx0, float wy0, float wx1,
                     float wy1, float thin, Rgba wireCol, Rgba nodeCol) {
    collectVisible(wx0, wy0, wx1, wy1);
    const CircuitScene& s = *scene;
    uint32_t lastCable = UINT32_MAX;
    for (const uint32_t sg : visSegs) {
      const SceneSegment& seg = s.segments[sg];
      if (!showWire(seg.wire)) continue;  // TX.block-drag: a lifted block's wires are drawn with it
      if (s.wires[seg.wire].cable) {  // TX.cables: its rest shape, thin and antialiased (once per cable)
        if (seg.wire != lastCable) farCable(dlW, seg.wire, ox, oy, sc, thin, wireCol);
        lastCable = seg.wire;
        continue;
      }
      const Point2 a = s.points[seg.point], b = s.points[seg.point + 1];
      // Sub-pixel jogs disappear under their neighbours' 1-px lines at this zoom: skip them.
      if (std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y)) * sc < 0.75f) continue;
      const float ax = static_cast<float>(ox + a.x * sc), ay = static_cast<float>(oy + a.y * sc);
      const float bx = static_cast<float>(ox + b.x * sc), by = static_cast<float>(oy + b.y * sc);
      if (a.y == b.y) dlW.addHLine(std::min(ax, bx) - thin * 0.5f, std::max(ax, bx) + thin * 0.5f, ay, wireCol, thin);
      else dlW.addVLine(ax, std::min(ay, by) - thin * 0.5f, std::max(ay, by) + thin * 0.5f, wireCol, thin);
      ++stats.segmentsPainted;
    }
    for (const uint32_t v : visNodes) {
      if (faced(v) || !showNode(v)) continue;  // TMM.4: faces are drawn over the chunks at every level
      const SceneNode& n = s.nodes[v];
      const ui::Rect rr{static_cast<float>(std::round(ox + n.x * sc)), static_cast<float>(std::round(oy + n.y * sc)),
                        static_cast<float>(std::round(ox + (n.x + n.w) * sc)), static_cast<float>(std::round(oy + (n.y + n.h) * sc))};
      if (rr.x1 <= rr.x0 || rr.y1 <= rr.y0) continue;
      dlN.addRectStroke(rr, nodeCol, 1.0f);
      ++stats.nodesPainted;
    }
  }

  // TX.cables: a cable's rest shape at the far level, world → device = o + w·s (a thin antialiased curve).
  void farCable(ui::DrawList& dl, uint32_t w, double ox, double oy, float sc, float thin, Rgba col) {
    const SceneWire& sw = scene->wires[w];
    farPts.clear();
    for (uint32_t i = 0; i < sw.pointCount; ++i) {
      const Point2 q = scene->points[sw.firstPoint + i];
      farPts.push_back({static_cast<float>(ox + q.x * sc), static_cast<float>(oy + q.y * sc)});
    }
    ui::smoothPolyline(dl, farPts, col, std::max(0.35f, thin - 0.6f));
    ++stats.segmentsPainted;
  }
  std::vector<Vec2> farPts;

  // A wire drawn over the Far picture (selection, hover).
  void farWire(ui::DrawList& dl, uint32_t w, double ox, double oy, float sc, float thin, Rgba col) {
    if (!scene || w >= scene->wires.size()) return;
    const SceneWire& sw = scene->wires[w];
    if (sw.cable) {
      farCable(dl, w, ox, oy, sc, thin + 0.5f, col);
      return;
    }
    for (uint32_t i = 0; i + 1 < sw.pointCount; ++i) {
      const Point2 a = scene->points[sw.firstPoint + i], b = scene->points[sw.firstPoint + i + 1];
      const float ax = static_cast<float>(ox + a.x * sc), ay = static_cast<float>(oy + a.y * sc);
      const float bx = static_cast<float>(ox + b.x * sc), by = static_cast<float>(oy + b.y * sc);
      if (a.y == b.y) dl.addHLine(std::min(ax, bx) - thin * 0.5f, std::max(ax, bx) + thin * 0.5f, ay, col, thin);
      else dl.addVLine(ax, std::min(ay, by) - thin * 0.5f, std::max(ay, by) + thin * 0.5f, col, thin);
    }
  }

  bool chunkReady(const ui::TileAtlas& atlas, const FarChunk& c) const noexcept {
    return c.visited && !c.failed && (!c.hasWires || atlas.valid(c.wires)) && (!c.hasNodes || atlas.valid(c.nodes));
  }

  void chunkQuads(ui::DrawList& dl, ui::TileAtlas& atlas, const FarChunk& c, float x, float y, float k) {
    const float inv = atlas.uvScale();
    auto quad = [&](const ui::TileAtlas::Tile& t, Rgba col) {
      atlas.touch(t.page);
      dl.addImage(ui::TileAtlas::texture(t.page), {x, y, x + static_cast<float>(t.w) * k, y + static_cast<float>(t.h) * k},
                  {static_cast<float>(t.x) * inv, static_cast<float>(t.y) * inv},
                  {static_cast<float>(t.x + t.w) * inv, static_cast<float>(t.y + t.h) * inv}, col);
    };
    if (c.hasWires) quad(c.wires, farWireCol);
    if (c.hasNodes) quad(c.nodes, farNodeCol);
  }

  // The Far level: chunks from tiles, stretched from the previous scale, or drawn directly. Returns whether a chunk of
  // this scale is missing (onFrame builds it).
  bool paintFar(ui::DrawList& dl, const ui::Theme& theme, const ui::IRect& clip, ui::TileAtlas* atlas, float thin) {
    const ui::IRect r = self.rect();
    const float sc = s();
    const double ox = std::round(r.x - view.x * sc), oy = std::round(r.y - view.y * sc);
    farWireCol = theme.circuit.wireParam;
    farNodeCol = theme.circuit.symbolStroke;
    farThin = thin;
    farScale = sc;
    const float C = kChunk;
    farI0 = static_cast<int32_t>(std::floor((clip.x - ox) / C));
    farI1 = static_cast<int32_t>(std::floor((clip.right() - 1 - ox) / C));
    farJ0 = static_cast<int32_t>(std::floor((clip.y - oy) / C));
    farJ1 = static_cast<int32_t>(std::floor((clip.bottom() - 1 - oy) / C));
    bool missing = false;
    const uint32_t key = bitsOf(sc);
    for (int32_t j = farJ0; j <= farJ1; ++j)
      for (int32_t i = farI0; i <= farI1; ++i) {
        const float x = static_cast<float>(ox + i * C), y = static_cast<float>(oy + j * C);
        // TX.block-drag: a chunk the lifted blocks or their wires were in is drawn directly without them while they are
        // lifted (its tile keeps them: it is right again once the drag ends without a new scene).
        const bool liftedHere = paintPass == 0 && liftTouches(static_cast<float>((x - ox) / sc), static_cast<float>((y - oy) / sc),
                                                         static_cast<float>((x + C - ox) / sc), static_cast<float>((y + C - oy) / sc));
        if (atlas && !liftedHere) {
          auto it = chunks.find({key, i, j});
          if (it != chunks.end() && chunkReady(*atlas, it->second)) {
            chunkQuads(dl, *atlas, it->second, x, y, 1.0f);
            ++stats.chunksFromTiles;
            continue;
          }
          const bool failed = it != chunks.end() && it->second.visited && it->second.failed;  // no room: drawn directly
          missing = missing || !failed;
          if (chunkScale > 0.0f && chunkScale != sc && stretched(dl, *atlas, ox, oy, sc, x, y)) {
            ++stats.chunksStretched;
            continue;
          }
        }
        // Directly, clipped to the chunk.
        dl.pushClip({x, y, x + C, y + C});
        drawFarRegion(dl, dl, ox, oy, sc, static_cast<float>((x - ox) / sc) - 1.0f, static_cast<float>((y - oy) / sc) - 1.0f,
                      static_cast<float>((x + C - ox) / sc) + 1.0f, static_cast<float>((y + C - oy) / sc) + 1.0f, thin, farWireCol,
                      farNodeCol);
        dl.popClip();
        ++stats.chunksDirect;
      }
    // Build ahead: the ring of chunks around the view, so a pan finds its next column ready.
    if (atlas && !missing)
      for (int32_t j = farJ0 - 1; j <= farJ1 + 1 && !missing; ++j)
        for (int32_t i = farI0 - 1; i <= farI1 + 1; ++i) {
          if (j >= farJ0 && j <= farJ1 && i >= farI0 && i <= farI1) continue;
          auto it = chunks.find({key, i, j});
          if (it == chunks.end() || !(chunkReady(*atlas, it->second) || (it->second.visited && it->second.failed))) {
            missing = true;
            break;
          }
        }
    return missing;
  }

  // Chunk (device rect at x, y) from the chunks of chunkScale, stretched; false when one of them is not ready.
  bool stretched(ui::DrawList& dl, ui::TileAtlas& atlas, double ox, double oy, float sc, float x, float y) {
    const float C = kChunk, k = sc / chunkScale, Ck = C * k;
    const int32_t p0 = static_cast<int32_t>(std::floor((x - ox) / Ck)), p1 = static_cast<int32_t>(std::floor((x + C - 1 - ox) / Ck));
    const int32_t q0 = static_cast<int32_t>(std::floor((y - oy) / Ck)), q1 = static_cast<int32_t>(std::floor((y + C - 1 - oy) / Ck));
    if ((p1 - p0 + 1) * (q1 - q0 + 1) > 64) return false;
    const uint32_t key = bitsOf(chunkScale);
    for (int32_t q = q0; q <= q1; ++q)
      for (int32_t p = p0; p <= p1; ++p) {
        auto it = chunks.find({key, p, q});
        if (it == chunks.end() || !chunkReady(atlas, it->second)) return false;
      }
    dl.pushClip({x, y, x + C, y + C});
    for (int32_t q = q0; q <= q1; ++q)
      for (int32_t p = p0; p <= p1; ++p)
        chunkQuads(dl, atlas, chunks.find({key, p, q})->second, static_cast<float>(ox + p * Ck), static_cast<float>(oy + q * Ck), k);
    dl.popClip();
    return true;
  }

  // Builds missing chunks of the last Far paint (≤ kChunksPerFrame). Returns true if it built any.
  bool buildFarChunks(ui::TileAtlas& atlas) {
    const float sc = farScale, C = kChunk;
    const uint32_t key = bitsOf(sc);
    uint32_t built = 0;
    bool more = false, visibleMissing = false;
    uint32_t builtVisible = 0;
    const auto t0 = std::chrono::steady_clock::now();
    // The visible chunks first, then the ring around them; within a time budget per frame (at least one chunk).
    for (int pass = 0; pass < 2 && !more; ++pass)
    for (int32_t j = farJ0 - pass; j <= farJ1 + pass && !more; ++j)
      for (int32_t i = farI0 - pass; i <= farI1 + pass; ++i) {
        if (pass == 1 && j >= farJ0 && j <= farJ1 && i >= farI0 && i <= farI1) continue;
        FarChunk& c = chunks[{key, i, j}];
        if (chunkReady(atlas, c) || (c.visited && c.failed)) continue;
        if (built > 0 && (built >= kChunksPerFrame ||
                          std::chrono::steady_clock::now() - t0 > std::chrono::microseconds(kChunkBuildMicros))) {
          more = true;
          visibleMissing = visibleMissing || pass == 0;
          break;
        }
        c = FarChunk{};
        const uint32_t tw = static_cast<uint32_t>(C), th = static_cast<uint32_t>(C);
        const double lox = -static_cast<double>(i) * C, loy = -static_cast<double>(j) * C;
        const float wx0 = static_cast<float>(i * C / sc) - 1.0f, wy0 = static_cast<float>(j * C / sc) - 1.0f;
        const float wx1 = static_cast<float>((i + 1) * C / sc) + 1.0f, wy1 = static_cast<float>((j + 1) * C / sc) + 1.0f;
        scratch.reset(tw, th);
        scratchOther.reset(tw, th);
        drawFarRegion(scratch, scratchOther, lox, loy, sc, wx0, wy0, wx1, wy1, farThin, ui::rgba(255, 255, 255), ui::rgba(255, 255, 255));
        for (int layer = 0; layer < 2; ++layer) {
          const ui::DrawList& src = layer == 0 ? scratch : scratchOther;
          if (src.empty()) continue;
          ui::TileAtlas::Tile& t = layer == 0 ? c.wires : c.nodes;
          if (!atlas.allocate(tw, th, t)) {
            c.failed = true;
            ++stats.tileFailures;
            break;
          }
          uint8_t* dst = atlas.write(t);
          const uint32_t pitch = ui::TileAtlas::rowPitch(t);
          ui::rasterizeCoverage(src, dst + pitch + 1, tw, th, pitch, {0.0f, 0.0f});
          (layer == 0 ? c.hasWires : c.hasNodes) = true;
        }
        c.visited = true;
        ++built;
        builtVisible += pass == 0 ? 1u : 0u;
        ++stats.tilesBuilt;
      }
    if (!visibleMissing) {  // the view is covered at this scale: it is the stretch source from now on, older scales go
      chunkScale = sc;
      for (auto it = chunks.begin(); it != chunks.end();)
        it = it->first.scale != key ? chunks.erase(it) : std::next(it);
    }
    tilesWanted = tilesWanted || more;
    return builtVisible > 0;  // the ring is not on screen: no repaint for it
  }

  // culling scratch
  std::vector<uint32_t> nodeStamp, segStamp;
  uint32_t stamp = 0;
  std::vector<uint32_t> visNodes, visSegs;
  std::vector<Vec2> pts, offA, offB;

  // info text
  mutable std::string infoTitle, infoBody;

  // ---- TMM.4: meta-module faces ----
  struct Hosted {
    uint32_t node = 0;   // scene node
    uint32_t entry = 0;  // index into scene->faces
    std::unique_ptr<Face> face;
    std::vector<FaceJack> jacks;
    uint32_t owner = 0;  // TMM.7: the instance (SceneFace owner, path): a new scene keeps its face
    std::string path;
    bool bound = false;
    bool panel = false;  // TX.panel-faces: a device's panel (panelMaker), else a meta face
    float scale = 1.0f;  // TX.resize: its node's SceneNode::scale (face-local units = world units / scale)
    uint64_t tileSalt = 0;  // TX.resize: a scaled face's tiles are its own (0: shared by its type at scale 1)
    MetaFace* meta() const noexcept { return panel ? nullptr : static_cast<MetaFace*>(face.get()); }
  };
  std::vector<Hosted> faces;
  std::vector<int32_t> faceOf;  // per scene node: index into faces, or -1
  std::vector<uint32_t> controlWires;  // TX.mod-all: the scene wires that end on a face's control (SceneWire::control)
  std::vector<uint32_t> metaIdx, panelIdx;  // TX.panel-faces: faces' indices by kind (faceAt / panelAt)
  uint32_t pumpTimer = 0;     // TX.panel-faces: the faces' own clock (Face::nextWake)
  double pumpDue = std::numeric_limits<double>::infinity();
  bool pumpWanted = false;    // a face asked for an update (faceNeedsUpdate): the next frame runs it
  std::unordered_map<uint64_t, ui::LayerTile> faceTiles;  // static layers by (type, layer), at one scale each
  bool facesShown = true;
  int32_t faceCapture = -1;  // the face a press went to (its drag runs until Up)
  int32_t faceHover = -1;
  uint32_t faceTimer = 0;
  std::vector<ui::Rect> faceRects;
  mutable std::string faceTitle, faceBody;

  int32_t faceFocus = -1;    // TMM.7: the face the last press went to (it gets keys first: a keyframe's Delete)

  bool faced(uint32_t v) const noexcept { return facesShown && v < faceOf.size() && faceOf[v] >= 0; }
  // A static layer's tile key: the type and layer, and the face's per-instance key (TMM.7: 0 = shared by the type).
  // TX.resize: a scaled face's tiles are rasterised at its own px per unit, so they are its own (Hosted::tileSalt: by
  // instance, not by scale — while its scale changes, the tile it had is stretched, as for a zoom, until it settles).
  // TMM.13-fix1 L15: the fields mixed, not packed (the type's pointer << 3 ^ layer collided for layer >= 8).
  static uint64_t faceKey(const Face& f, uint32_t layer, uint64_t salt) {
    const uint64_t base = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(f.metaType())) * 0xD6E8FEB86659FD93ull) ^
                          ((static_cast<uint64_t>(layer) + 1) * 0xC2B2AE3D27D4EB4Full) ^ (salt * 0x165667B19E3779F9ull);
    const uint64_t inst = f.staticKey(layer);
    return inst == 0 ? base & ~(1ull << 63) : (base ^ (inst * 0x9E3779B97F4A7C15ull)) | 1ull << 63;
  }
  // Drops tiles no hosted face uses any more (instance keys change with their data) once the map has grown.
  void pruneFaceTiles() {
    if (faceTiles.size() <= 32 + 4 * faces.size()) return;
    std::unordered_map<uint64_t, ui::LayerTile> keep;
    for (const Hosted& h : faces)
      for (uint32_t layer = 0; layer < h.face->staticLayerCount(); ++layer) {
        const uint64_t k = faceKey(*h.face, layer, h.tileSalt);
        auto it = faceTiles.find(k);
        if (it != faceTiles.end()) keep.emplace(k, it->second);
      }
    faceTiles.swap(keep);
  }

  // TMM.7: a new scene keeps the faces of the instances it still shows (same type, owner and path): their bindings,
  // their own state (a selected keyframe, voice fades) and any drag in progress survive a rebuild — which a face's own
  // edit can cause while its handler runs (a setting is structural: the host rebuilds synchronously). Faces that go
  // are retired, not destroyed, until the next paint, so a handler that caused the rebuild returns into a live object.
  std::vector<std::unique_ptr<Face>> retired;
  int32_t indexOf(const Face* f) const noexcept {
    for (size_t i = 0; i < faces.size(); ++i)
      if (faces[i].face.get() == f) return static_cast<int32_t>(i);
    return -1;
  }

  bool rescaled = false;  // TX.resize: the last buildFaces kept a face whose node's scale changed

  void buildFaces() {
    rescaled = false;
    std::vector<Hosted> old;
    old.swap(faces);
    auto ptrAt = [&](int32_t i) -> const Face* {
      return i >= 0 && static_cast<size_t>(i) < old.size() ? old[static_cast<size_t>(i)].face.get() : nullptr;
    };
    const Face *capture = ptrAt(faceCapture), *hoverFace = ptrAt(faceHover), *focus = ptrAt(faceFocus);
    faceCapture = faceHover = faceFocus = -1;
    faceOf.assign(scene ? scene->nodes.size() : 0, -1);
    controlWires.clear();
    for (uint32_t w = 0; scene && w < scene->wires.size(); ++w)
      if (scene->wires[w].control != UINT32_MAX) controlWires.push_back(w);
    metaIdx.clear();
    panelIdx.clear();
    for (uint32_t e = 0; scene && e < scene->faces.size(); ++e) {
      const SceneFace& f = scene->faces[e];
      if (f.node >= scene->nodes.size() || (f.type == nullptr && !f.panel)) continue;
      std::unique_ptr<Face> face;
      bool reused = false;
      float prevScale = 1.0f;
      for (Hosted& o : old)
        if (o.face && o.panel == f.panel && (f.panel || o.face->metaType() == f.type) && o.owner == f.owner && o.path == f.path) {
          face = std::move(o.face);
          reused = true;
          prevScale = o.scale;
          break;
        }
      if (!face) {
        if (f.panel) face = self.panelMaker ? self.panelMaker(f) : nullptr;  // TX.panel-faces
        else face = makeFace(*f.type);
      }
      if (!face) continue;
      face->attach(this);
      Hosted h;
      h.node = f.node;
      h.entry = e;
      h.owner = f.owner;
      h.path = f.path;
      h.bound = reused;
      h.panel = f.panel;
      h.face = std::move(face);
      const SceneNode& n = scene->nodes[f.node];
      h.scale = n.scale > 0.0f ? n.scale : 1.0f;
      if (h.scale != 1.0f)
        h.tileSalt = (static_cast<uint64_t>(h.owner) * 0x9E3779B97F4A7C15ull ^ std::hash<std::string>()(h.path) * 0xC2B2AE3D27D4EB4Full) | 8ull;
      // A kept face whose block was rescaled (an S-drag step): its tiles and a panel's layout wait until the scale
      // settles, as for a zoom (stretched meanwhile).
      if (reused && h.scale != prevScale) rescaled = true;
      for (uint32_t k = 0; k < n.pinCount; ++k) {
        const ScenePin& pin = scene->pins[n.firstPin + k];
        FaceJack j;
        j.at = {(pin.x - n.x) / h.scale, (pin.y - n.y) / h.scale};
        j.input = pin.input;
        j.connected = pin.connected;
        j.visible = pin.visible && !pin.orbit;  // TX.orbit-ports: the canvas draws an orbiting port (off the face)

        if (pin.label != UINT32_MAX) j.name = scene->strings[pin.label];
        h.jacks.push_back(j);
      }
      faceOf[f.node] = static_cast<int32_t>(faces.size());
      (h.panel ? panelIdx : metaIdx).push_back(static_cast<uint32_t>(faces.size()));
      faces.push_back(std::move(h));
    }
    for (Hosted& o : old)
      if (o.face) retired.push_back(std::move(o.face));
    // TX.panel-faces: a retired face is kept alive (a handler of its may be running) but leaves the host.
    for (const std::unique_ptr<Face>& f : retired) f->attach(nullptr);
    faceCapture = indexOf(capture);
    faceHover = indexOf(hoverFace);
    faceFocus = indexOf(focus);
    bindFaces(false);
    syncTextInput();
  }

  // Binds the faces through the host's binder (all: the host's state moved; else only the faces new to this scene).
  void bindFaces(bool all) {
    for (Hosted& h : faces) {
      if ((h.bound && !all) || h.panel) continue;  // TX.panel-faces: a panel is bound by its maker
      CircuitCanvas::FaceBinding b;
      if (self.faceBinder) b = self.faceBinder(scene->faces[h.entry]);
      h.meta()->bind(std::move(b.target), b.voices);
      if (h.meta()->wantsAnalysis()) h.meta()->setAnalysis(std::move(b.analysis), [this] { self.wakeFaces(); });  // TX.spectrum
      h.bound = true;
    }
  }

  // The face whose block is under a device point (-1: none). TX.orbit-ports: a face's floating panel first (over all).
  int32_t faceUnder(Vec2 d) const noexcept {
    if (!facesShown || !scene || !self.rect().contains(d)) return -1;
    if (const int32_t p = popupUnder(d); p >= 0) return p;
    const Point2 w = self.toWorld(d);
    // TX.block-drag: a lifted face is where it is drawn (over the others: first).
    // TX.drag-grow: then the shifted ones (drawn over the scene, under the lifted), then the rest.
    for (const int lift : {1, 2, 0}) {
      if (lift > 0 && !anyLift()) continue;
      for (size_t i = faces.size(); i-- > 0;) {
        const uint8_t k = layerOf(faces[i].node);
        if ((lift == 1 ? k != 1 : lift == 2 ? k < kShiftLayer0 : k != 0)) continue;
        if (faces[i].scale < kFaceMiniatureScale) continue;  // TX.block-drag: a miniature has no controls (it is the block)
        const SceneNode& n = scene->nodes[faces[i].node];
        const Point2 o = shiftOf(faces[i].node);
        const float x = w.x - o.x, y = w.y - o.y;
        if (!n.hidden && x >= n.x && x <= n.x + n.w && y >= n.y && y <= n.y + n.h) return static_cast<int32_t>(i);
      }
    }
    return -1;
  }

  FacePointer pointerOf(int32_t i, const ui::PointerEvent& e, FacePointer::Kind kind) const {
    const Hosted& h = faces[static_cast<size_t>(i)];
    const Point2 w = self.toWorld(e.pos);
    FacePointer fp;
    fp.kind = kind;
    fp.local = localOf(h, w);
    fp.device = e.pos;
    fp.pxPerUnit = s() * h.scale;
    fp.uiScale = uiScale();
    fp.mods = e.mods;
    fp.clicks = e.clickCount;
    fp.wheel = e.wheel;
    if (e.button != ui::PointerButton::None) fp.button = e.button;
    return fp;
  }

  // TX.resize: a world point in face `h`'s own units (its node may be scaled).
  ui::Vec2 localOf(const Hosted& h, Point2 w) const noexcept {
    const SceneNode& n = scene->nodes[h.node];
    const Point2 o = shiftOf(h.node);  // TX.block-drag
    return {(w.x - o.x - n.x) / h.scale, (w.y - o.y - n.y) / h.scale};
  }

  bool faceVisible(const Hosted& h) const noexcept {
    SceneNode n = scene->nodes[h.node];
    n.x += shiftOf(h.node).x, n.y += shiftOf(h.node).y;  // TX.block-drag
    const ui::IRect r = self.rect();
    return !n.hidden && devX(n.x + n.w) >= static_cast<float>(r.x) && devX(n.x) <= static_cast<float>(r.right()) &&
           devY(n.y + n.h) >= static_cast<float>(r.y) && devY(n.y) <= static_cast<float>(r.bottom());
  }

  // At most 60 Hz while a visible face's reader is live: new voice records damage that face's live rects only.
  void pollFaces() {
    if (!facesShown || !scene) return;
    bool live = false;
    ui::UiTree* tr = self.tree();
    const ui::IRect cr = self.rect();
    for (Hosted& h : faces) {
      VoiceTapReader* rd = h.face->voices();
      if (!faceVisible(h)) continue;
      const bool ticked = h.face->frameTick();  // DT.1: what follows the transport without voices (a compare)
      live = live || h.face->ticking();
      if (rd == nullptr && !ticked) continue;
      if ((ticked | (rd != nullptr && h.face->pollVoices())) && tr != nullptr) {
        SceneNode n = scene->nodes[h.node];
        n.x += shiftOf(h.node).x, n.y += shiftOf(h.node).y;  // TX.block-drag
        faceRects.clear();
        h.face->liveRects(faceRects);
        const float k = h.scale;
        for (const ui::Rect& lr : faceRects) {
          const ui::Rect d{devX(n.x + lr.x0 * k) - 2.0f, devY(n.y + lr.y0 * k) - 2.0f, devX(n.x + lr.x1 * k) + 2.0f,
                           devY(n.y + lr.y1 * k) + 2.0f};
          const ui::Rect c{std::max(d.x0, static_cast<float>(cr.x)), std::max(d.y0, static_cast<float>(cr.y)),
                           std::min(d.x1, static_cast<float>(cr.right())), std::min(d.y1, static_cast<float>(cr.bottom()))};
          tr->damage().add(c);
        }
      }
      live = live || (rd != nullptr && (rd->live() || h.face->animating()));
    }
    if (live && faceTimer == 0 && tr != nullptr) faceTimer = self.requestTimer(kFaceFrameSeconds);
  }

  // Draws every visible face: static layers from tiles (or directly), then jacks and the live layer.
  void paintFaces(ui::PaintContext& ctx, const ui::IRect& clip, ui::TileAtlas* atlas, bool& missing) {
    if (!facesShown || faces.empty()) return;
    const ui::Theme& th = *ctx.res.theme;
    bool wantTimer = false;
    for (Hosted& h : faces) {
      const SceneNode& n = scene->nodes[h.node];
      if (n.hidden || !showNode(h.node)) continue;  // TX.block-drag: a lifted face in the lifted pass
      const float sc = s() * h.scale;  // TX.resize: a scaled node's face at its own px per unit
      const float x0 = devX(n.x), y0 = devY(n.y), x1 = devX(n.x + n.w), y1 = devY(n.y + n.h);
      if (x1 < static_cast<float>(clip.x) - 2.0f || x0 > static_cast<float>(clip.right()) + 2.0f ||
          y1 < static_cast<float>(clip.y) - 2.0f || y0 > static_cast<float>(clip.bottom()) + 2.0f)
        continue;
      Face& f = *h.face;
      const Vec2 o{std::round(x0), std::round(y0)};
      if (f.needsPrepare(sc)) missing = true;  // TX.panel-faces: a panel's layout at this scale (built in onFrame)
      const uint32_t layers = f.staticAt(sc) ? f.staticLayerCount() : 0;  // TX.panel-faces: a panel's only far out
      for (uint32_t layer = 0; layer < layers; ++layer) {
        const Rgba col = f.staticLayerColour(th, layer);
        bool drawn = false;
        // TMM.7: a per-instance layer of a face being dragged changes with every step (a knob re-bakes it): drawn
        // directly until the drag ends, no tile built per step.
        const bool volatileLayer = f.dragging() && f.staticKey(layer) != 0;
        if (atlas && !volatileLayer) {
          auto it = faceTiles.find(faceKey(f, layer, h.tileSalt));
          if (it != faceTiles.end() && ui::layerReady(*atlas, it->second)) {
            ui::drawLayer(ctx.dl, *atlas, it->second, o, sc, col);
            drawn = true;
            if (it->second.scale == sc) {
              ++stats.faceLayersFromTiles;
            } else {
              ++stats.faceLayersStretched;
              missing = true;
            }
          } else if (!(it != faceTiles.end() && it->second.failed && it->second.scale == sc)) {
            missing = true;
          }
        }
        if (!drawn) {
          ui::FacePainter p(ctx.dl, {o, sc});
          p.setTint(col);
          f.paintStatic(p, layer);
          ++stats.faceLayersDirect;
        }
      }
      FaceEnv env;
      env.theme = &th;
      env.uiScale = ctx.scale.factor;
      env.selected = h.node < nodeSel.size() && nodeSel[h.node] != 0;
      env.hovered = hover.kind == CanvasHit::Kind::Node && hover.index == h.node;
      env.jacks = h.jacks;
      ui::FacePainter p(ctx.dl, {o, sc});
      p.setText(ctx.res.text);
      p.setImages(ctx.res.images);  // TX.spectrogram: a face with its own texture
      f.paintJacks(p, env);
      f.paintLive(p, env);
      if (h.panel) {
        ++stats.panelsPainted;  // TX.panel-faces
      } else {
        stats.facePrimitives += p.primitives();
        ++stats.facesPainted;
      }
      wantTimer = wantTimer || f.ticking() || (f.voices() != nullptr && (f.voices()->live() || f.animating()));
    }
    if (wantTimer && faceTimer == 0) faceTimer = self.requestTimer(kFaceFrameSeconds);
  }

  // Builds the visible faces' missing static-layer tiles at the current scale. True when it built any.
  bool buildFaceTiles(ui::TileAtlas& atlas) {
    if (!facesShown) return false;
    bool built = false;
    for (const Hosted& h : faces) {
      if (!faceVisible(h)) continue;
      const Face& f = *h.face;
      const float sc = s() * h.scale;  // TX.resize
      const uint32_t layers = f.staticAt(sc) ? f.staticLayerCount() : 0;  // TX.panel-faces
      for (uint32_t layer = 0; layer < layers; ++layer) {
        if (f.dragging() && f.staticKey(layer) != 0) continue;
        ui::LayerTile& t = faceTiles[faceKey(f, layer, h.tileSalt)];
        if (t.scale == sc && (ui::layerReady(atlas, t) || t.failed)) continue;
        const ui::Vec2 ws = f.worldSize();
        ui::LayerTile fresh;
        if (ui::recordLayer(atlas, scratch, ws.x, ws.y, sc, [&](ui::FacePainter& p) { f.paintStatic(p, layer); }, fresh))
          ++stats.tilesBuilt;
        else
          ++stats.tileFailures;
        t = fresh;
        built = true;
      }
    }
    if (built) pruneFaceTiles();
    return built;
  }

  // ---- TX.panel-faces: the faces' own clock and scale ----
  ui::Widget* hostWidget() noexcept override { return &self; }
  void faceNeedsUpdate(Face& /*f*/) noexcept override {
    if (pumpWanted) return;
    pumpWanted = true;
    self.requestFrame();
  }
  // The face's device rect, damaged (clipped to the canvas).
  void damageFace(const Hosted& h) {
    ui::UiTree* tr = self.tree();
    if (tr == nullptr || !scene) return;
    SceneNode n = scene->nodes[h.node];
    n.x += shiftOf(h.node).x, n.y += shiftOf(h.node).y;  // TX.block-drag
    const ui::IRect cr = self.rect();
    const ui::Rect d{std::max(devX(n.x) - 4.0f, static_cast<float>(cr.x)), std::max(devY(n.y) - 4.0f, static_cast<float>(cr.y)),
                     std::min(devX(n.x + n.w) + 4.0f, static_cast<float>(cr.right())),
                     std::min(devY(n.y + n.h) + 4.0f, static_cast<float>(cr.bottom()))};
    if (d.x1 > d.x0 && d.y1 > d.y0) tr->damage().add(d);
  }
  // Runs every face's due work (Face::update) at the host tree's clock, damages the faces that changed, and arms the
  // next wake-up: a frame when one is due now, else one timer at the earliest Face::nextWake. Idle faces cost nothing.
  void pumpFaces() {
    pumpWanted = false;
    ui::UiTree* tr = self.tree();
    if (tr == nullptr) return;
    const double now = tr->now();
    double due = std::numeric_limits<double>::infinity();
    for (Hosted& h : faces) {
      if (h.face->update(now) && facesShown && !scene->nodes[h.node].hidden) damageFace(h);
      due = std::min(due, h.face->nextWake());
    }
    if (due <= now) {
      pumpWanted = true;
      self.requestFrame();
    } else if (due < pumpDue || pumpTimer == 0) {
      if (pumpTimer != 0) self.cancelTimer(pumpTimer);
      pumpTimer = 0;
      pumpDue = due;
      if (due < std::numeric_limits<double>::infinity()) pumpTimer = self.requestTimer(due - now);
    }
  }
  // The zoom settled: visible faces rebuild what depends on the scale (a panel's layout). True when any did.
  bool prepareFaces() {
    if (!facesShown || !scene) return false;
    bool any = false;
    for (Hosted& h : faces) {
      const float sc = s() * h.scale;  // TX.resize
      if (faceVisible(h) && h.face->needsPrepare(sc) && h.face->prepare(sc)) any = true;
    }
    return any;
  }
  // The focused face's text acceptance mirrored on the canvas (the host tree sends text only to a text input).
  void syncTextInput() {
    const bool want = faceFocus >= 0 && static_cast<size_t>(faceFocus) < faces.size() && faces[static_cast<size_t>(faceFocus)].face->wantsText();
    self.setAcceptsTextInput(want);
  }
  // The key focus leaves face `i` (a press elsewhere, a new scene without it).
  void blurFace(int32_t i) {
    if (i < 0 || static_cast<size_t>(i) >= faces.size()) return;
    Face& f = *faces[static_cast<size_t>(i)].face;
    const bool popup = f.popupRect().has_value();
    f.focusLost();
    if (popup && !f.popupRect()) self.markDirty();  // TX.orbit-ports: its floating panel closed (a press elsewhere)
  }

  // ---- TX.cables: hanging cables ----
  struct CableRef {
    uint32_t wire = 0;
    uint32_t fromNode = UINT32_MAX, toNode = UINT32_MAX;  // the scene nodes of its ends (drag offsets, faces)
    Point2 restA, restB;  // the scene's ends (a control's centre at build time for a cable onto a control)
    float glow = 0.0f;    // 0 … 1, smoothed
  };
  std::vector<CableRef> cables;  // sim index = cables index
  std::vector<int32_t> cableOf;  // per scene wire: its cable (−1: none)
  CableSim sim;
  double simLast = -1.0;              // the frame time of the last advance (−1: none since it woke)
  // TX.block-drag: blocks lifted by a host's drag (setNodeDrag) — drawn, hit and plugged at their place + dragOffset,
  // over the rest of the scene, while the scene still has them where they were. TX.drag-grow: and nodes shifted by it
  // (setNodeShift), each shift layer at its own offset.
  std::vector<uint32_t> dragNodes;    // sorted: setNodeDrag's nodes
  Point2 dragOffset{0.0f, 0.0f};
  std::vector<uint32_t> shiftNodes;   // sorted: setNodeShift's nodes
  std::vector<uint8_t> shiftLayer;    // per shiftNodes entry: its layer (0 …)
  std::vector<Point2> shiftOffsets;   // per shift layer
  static constexpr uint8_t kMixedLift = 0xFF;
  std::vector<uint8_t> liftNode;      // per scene node: its layer (0 in place, 1 lifted, kShiftLayer0 + k shifted; empty: none)
  std::vector<uint8_t> liftWire;      // per scene wire: 0 in place, the layer of both ends, kMixedLift: ends in different layers
  std::vector<uint32_t> liftSegs;     // the segments of the (non-cable) wires with both ends in one moved layer
  std::vector<uint32_t> liftHalf;     // the non-cable wires with ends in different layers (drawn as a Z between their ends)
  std::vector<ui::Rect> liftArea;     // world rects the moved parts cover in the scene (far chunks there: drawn directly)
  std::vector<uint32_t> passNodes;    // paint scratch: one layer's nodes
  // The paint pass: −1 everything (nothing moved), 0 the scene without the moved parts, k ≥ 1 the nodes of layer k only.
  int8_t paintPass = -1;
  bool anyLift() const noexcept { return !dragNodes.empty() || !shiftNodes.empty(); }
  uint8_t layerOf(uint32_t v) const noexcept { return v < liftNode.size() ? liftNode[v] : 0; }
  bool lifted(uint32_t v) const noexcept { return layerOf(v) != 0; }  // moved by the drag (lifted or shifted)
  bool showNode(uint32_t v) const noexcept { return paintPass < 0 || layerOf(v) == static_cast<uint8_t>(paintPass); }
  bool showWire(uint32_t w) const noexcept {
    if (paintPass < 0 || liftWire.empty()) return paintPass <= 0;
    return liftWire[w] == static_cast<uint8_t>(paintPass);
  }
  Point2 layerOffset(uint8_t k) const noexcept {
    if (k == 0) return {0.0f, 0.0f};
    if (k == 1) return dragOffset;
    const size_t j = static_cast<size_t>(k - kShiftLayer0);
    return j < shiftOffsets.size() ? shiftOffsets[j] : Point2{0.0f, 0.0f};
  }
  Point2 shiftOf(uint32_t v) const noexcept { return layerOffset(layerOf(v)); }
  // The nodes of layer k (passNodes; 1: dragNodes).
  std::span<const uint32_t> nodesOfLayer(uint8_t k) {
    if (k == 1) return dragNodes;
    passNodes.clear();
    for (size_t i = 0; i < shiftNodes.size(); ++i)
      if (static_cast<uint8_t>(shiftLayer[i] + kShiftLayer0) == k) passNodes.push_back(shiftNodes[i]);
    return passNodes;
  }
  void clearLift() {
    dragNodes.clear();
    dragOffset = {0.0f, 0.0f};
    shiftNodes.clear();
    shiftLayer.clear();
    shiftOffsets.clear();
    liftNode.clear();
    liftWire.clear();
    liftSegs.clear();
    liftHalf.clear();
    liftArea.clear();
  }
  // The masks and areas for dragNodes and shiftNodes (new sets; the offsets alone change nothing here).
  void buildLift() {
    liftNode.assign(scene ? scene->nodes.size() : 0, 0);
    liftWire.assign(scene ? scene->wires.size() : 0, 0);
    liftSegs.clear();
    liftHalf.clear();
    liftArea.clear();
    if (!scene) return;
    const CircuitScene& sc = *scene;
    for (size_t i = 0; i < shiftNodes.size(); ++i)
      if (shiftNodes[i] < liftNode.size()) liftNode[shiftNodes[i]] = static_cast<uint8_t>(kShiftLayer0 + shiftLayer[i]);
    for (const uint32_t v : dragNodes)
      if (v < liftNode.size()) {
        liftNode[v] = 1;
        const SceneNode& n = sc.nodes[v];
        liftArea.push_back({n.x - 16.0f, n.y - 16.0f, n.x + n.w + 16.0f, n.y + n.h + 16.0f});
      }
    // A shift layer (many nodes: the bands under a growing one): one rect around it.
    for (uint8_t k = 0; k < shiftOffsets.size(); ++k) {
      bool any = false;
      ui::Rect r{};
      for (size_t i = 0; i < shiftNodes.size(); ++i) {
        if (shiftLayer[i] != k || shiftNodes[i] >= sc.nodes.size()) continue;
        const SceneNode& n = sc.nodes[shiftNodes[i]];
        const ui::Rect q{n.x - 16.0f, n.y - 16.0f, n.x + n.w + 16.0f, n.y + n.h + 16.0f};
        r = any ? ui::Rect{std::min(r.x0, q.x0), std::min(r.y0, q.y0), std::max(r.x1, q.x1), std::max(r.y1, q.y1)} : q;
        any = true;
      }
      if (any) liftArea.push_back(r);
    }
    for (uint32_t w = 0; w < sc.wires.size(); ++w) {
      const SceneWire& sw = sc.wires[w];
      const uint8_t a = layerOf(wireEnd(sc, sw, false));
      const uint8_t b = layerOf(wireEnd(sc, sw, true));
      const uint8_t k = a == b ? a : kMixedLift;
      liftWire[w] = k;
      if (k == 0 || sw.hidden) continue;
      liftArea.push_back({sw.x0 - 8.0f, sw.y0 - 8.0f, sw.x1 + 8.0f, sw.y1 + 8.0f});
      if (!sw.cable && k == kMixedLift && sw.pointCount >= 2) liftHalf.push_back(w);
    }
    for (uint32_t i = 0; i < sc.segments.size(); ++i) {
      const uint8_t k = liftWire[sc.segments[i].wire];
      if (k != 0 && k != kMixedLift && !sc.wires[sc.segments[i].wire].cable) liftSegs.push_back(i);
    }
  }
  bool liftTouches(float x0, float y0, float x1, float y1) const noexcept {
    for (const ui::Rect& r : liftArea)
      if (r.x0 <= x1 && r.x1 >= x0 && r.y0 <= y1 && r.y1 >= y0) return true;
    return false;
  }
  uint32_t glowTimer = 0;
  bool glowWasLive = false;           // the last poll saw the sources live (one more poll after they stop)
  std::vector<uint32_t> glowWires;    // poll scratch
  std::vector<float> glowLevels;
  std::vector<uint32_t> movedScratch;
  // Hit-test grid over the cables' current bounds (rebuilt when the sim moved).
  static constexpr float kCableCell = 128.0f;
  mutable uint64_t cgVersion = ~0ull;
  mutable const CircuitScene* cgScene = nullptr;
  mutable float cgX0 = 0.0f, cgY0 = 0.0f;
  mutable uint32_t cgCols = 0, cgRows = 0;
  mutable std::vector<uint32_t> cgBegin, cgItems;
  // Paint scratch: the visible cables' device polylines (one flat buffer).
  struct CableDraw {
    uint32_t cable = 0, first = 0, count = 0;
    float w = 0.0f, plugA = 0.0f, plugB = 0.0f;  // TX.block-drag: tube and plug widths (device px) by its ends' scales
    Rgba col = 0;
    float alpha = 1.0f;
    float flip = 1.0f;  // the normals' sign: the highlight on the cable's upper side
    float glow = 0.0f;  // 0 … 1 (the loose cable: none)
    bool poly = false, sel = false, hov = false;
  };
  std::vector<CableDraw> cdraw;
  std::vector<Vec2> cpts, cnrm;
  std::vector<float> carc, cfade;


  // Where cable c's ends are now: the scene's, a control's live anchor (its rim, toward the cable's source), the drag.
  void cableEnds(const CableRef& c, Point2& a, Point2& b) const {
    a = c.restA;
    b = c.restB;
    const SceneWire& sw = scene->wires[c.wire];
    // TX.orbit-ports: an end on an orbiting port is where the port is now (its slide, its block's drag offset).
    const int32_t oa = orbitAt(sw.fromPin), ob = orbitAt(sw.toPin);
    if (oa >= 0) a = orbitPos(orbits[static_cast<size_t>(oa)], true);
    if (ob >= 0) b = orbitPos(orbits[static_cast<size_t>(ob)], true);
    if (sw.control != UINT32_MAX && c.toNode != UINT32_MAX && faced(c.toNode)) {
      const Hosted& h = faces[static_cast<size_t>(faceOf[c.toNode])];
      if (const std::optional<ControlAnchor> an = h.face->controlAnchor(scene->strings[sw.control])) {
        const SceneNode& n = scene->nodes[c.toNode];
        const Point2 centre{n.x + an->at.x * h.scale, n.y + an->at.y * h.scale};
        const float dx = a.x - centre.x, dy = a.y - centre.y, len = std::sqrt(dx * dx + dy * dy);
        const float r = an->radius * h.scale;
        b = len > r + 1e-3f ? Point2{centre.x + dx / len * r, centre.y + dy / len * r} : centre;
      }
    }
    if (anyLift()) {
      if (oa < 0 && lifted(c.fromNode)) a = {a.x + shiftOf(c.fromNode).x, a.y + shiftOf(c.fromNode).y};
      if (ob < 0 && lifted(c.toNode)) b = {b.x + shiftOf(c.toNode).x, b.y + shiftOf(c.toNode).y};
    }
  }

  // ---- TX.orbit-ports: orbiting main ports (pq/views/circuit/orbit.h) ----
  // Every scene pin with ScenePin::orbit: its angle now (`cur`, drawn and plugged there) eases toward `target` (the
  // scene's settled angle, or the live solve while blocks are lifted). Only the ports whose cable ends moved are solved
  // again; frames run only while a port slides.
  struct OrbitRef {
    uint32_t pin = 0, node = 0;
    float cur = 0.0f, target = 0.0f;
    bool pinned = false;
    bool held = false;      // the host holds it at `heldAngle` (a port dragged along the edge: setPortAngle)
    float heldAngle = 0.0f;
    uint32_t firstEnd = 0, endCount = 0;  // into orbitEnds: the cables (cables[] index) that end on it
  };
  std::vector<OrbitRef> orbits;
  std::vector<int32_t> orbitOf;      // per scene pin: index into orbits, or -1
  std::vector<uint32_t> orbitEnds;   // per orbit, its cables
  std::vector<uint32_t> orbitLive;   // TX.block-drag: the nodes whose ports a lift moves (solved per move), sorted
  std::vector<Point2> orbitScratch, orbitFixed;
  std::vector<std::pair<uint64_t, float>> orbitWas;  // syncOrbits scratch
  double orbitLast = -1.0;
  bool orbitAnimated = true;         // follows setCablesAnimated (tests / goldens: at once)
  uint64_t orbitSolves = 0;
  std::shared_ptr<const CircuitScene> scene0;  // the scene the orbits were built for (their keys across a new one)

  int32_t orbitAt(uint32_t pin) const noexcept { return pin < orbitOf.size() ? orbitOf[pin] : -1; }
  // Where orbit o is drawn now (shifted: by its block's lift offset).
  Point2 orbitPos(const OrbitRef& o, bool shifted) const noexcept {
    const SceneNode& n = scene->nodes[o.node];
    const Point2 sh = shifted ? shiftOf(o.node) : Point2{0.0f, 0.0f};
    return orbitPoint(n.outline.shifted(sh.x, sh.y), o.cur, nodeScale(o.node));
  }
  // A scene pin where it is now (an orbiting one: on its outline; unshifted).
  Point2 pinXY(uint32_t p) const noexcept {
    const int32_t o = orbitAt(p);
    if (o >= 0) return orbitPos(orbits[static_cast<size_t>(o)], false);
    return {scene->pins[p].x, scene->pins[p].y};
  }
  bool orbitsMoving() const noexcept {
    for (const OrbitRef& o : orbits)
      if (o.cur != o.target) return true;
    return false;
  }

  // The scene's orbiting pins (a kept port, by its key, slides on from where it was drawn). Before syncCables.
  void syncOrbits() {
    orbitWas.clear();
    for (const OrbitRef& o : orbits)
      if (scene0 && o.pin < scene0->pins.size()) orbitWas.emplace_back(scene0->pins[o.pin].orbitKey, o.cur);
    std::sort(orbitWas.begin(), orbitWas.end());
    orbits.clear();
    orbitEnds.clear();
    orbitLive.clear();
    orbitOf.assign(scene ? scene->pins.size() : 0, -1);
    for (uint32_t p = 0; scene && p < scene->pins.size(); ++p) {
      const ScenePin& pin = scene->pins[p];
      if (!pin.orbit || pin.node >= scene->nodes.size() || !scene->nodes[pin.node].outline.valid() || scene->nodes[pin.node].hidden) continue;
      OrbitRef o;
      o.pin = p;
      o.node = pin.node;
      o.pinned = pin.pinned;
      o.target = o.cur = pin.angle;
      if (orbitAnimated && pin.orbitKey != 0) {
        const auto it = std::lower_bound(orbitWas.begin(), orbitWas.end(), pin.orbitKey,
                                         [](const std::pair<uint64_t, float>& a, uint64_t k) { return a.first < k; });
        if (it != orbitWas.end() && it->first == pin.orbitKey) o.cur = it->second;
      }
      orbitOf[p] = static_cast<int32_t>(orbits.size());
      orbits.push_back(o);
    }
    scene0 = scene;
    orbitLast = -1.0;
  }
  // After syncCables: each orbit's cables.
  void linkOrbits() {
    orbitEnds.clear();
    for (OrbitRef& o : orbits) o.firstEnd = o.endCount = 0;
    for (uint32_t i = 0; i < cables.size(); ++i) {
      const SceneWire& sw = scene->wires[cables[i].wire];
      for (const uint32_t p : {sw.fromPin, sw.toPin})
        if (const int32_t o = orbitAt(p); o >= 0) ++orbits[static_cast<size_t>(o)].firstEnd;  // (a count first)
    }
    uint32_t at = 0;
    for (OrbitRef& o : orbits) {
      const uint32_t n = o.firstEnd;
      o.firstEnd = at;
      at += n;
    }
    orbitEnds.assign(at, 0);
    for (uint32_t i = 0; i < cables.size(); ++i) {
      const SceneWire& sw = scene->wires[cables[i].wire];
      for (const uint32_t p : {sw.fromPin, sw.toPin})
        if (const int32_t o = orbitAt(p); o >= 0) {
          OrbitRef& r = orbits[static_cast<size_t>(o)];
          orbitEnds[r.firstEnd + r.endCount++] = i;
        }
    }
    if (orbitsMoving()) self.requestFrame();
  }
  // Solves node v's orbiting ports again (the layout's rule, where the blocks are drawn now: lifted ones at their
  // offset): their targets. Returns whether one changed.
  bool solveNode(uint32_t v) {
    const SceneNode& n = scene->nodes[v];
    int32_t idx[2] = {-1, -1};  // input, output
    for (uint32_t k = 0; k < n.pinCount; ++k)
      if (const int32_t o = orbitAt(n.firstPin + k); o >= 0) idx[scene->pins[n.firstPin + k].input ? 0 : 1] = o;
    if (idx[0] < 0 && idx[1] < 0) return false;
    ++orbitSolves;
    const Point2 sh = shiftOf(v);
    const Outline ol = n.outline.shifted(sh.x, sh.y);
    orbitScratch.clear();
    orbitFixed.clear();
    size_t split[3] = {0, 0, 0};
    for (int k = 0; k < 2; ++k) {
      if (idx[k] >= 0) {
        const OrbitRef& o = orbits[static_cast<size_t>(idx[k])];
        for (uint32_t e = 0; e < o.endCount; ++e) {
          const CableRef& c = cables[orbitEnds[o.firstEnd + e]];
          const SceneWire& sw = scene->wires[c.wire];
          const bool fromHere = sw.fromPin == o.pin;
          const uint32_t other = fromHere ? sw.toPin : sw.fromPin;
          const uint32_t on = fromHere ? c.toNode : c.fromNode;
          const Point2 s2 = shiftOf(on);
          Point2 t = fromHere ? c.restB : c.restA;
          // The other end orbits too: toward its block's centre (as the layout does: no iteration between them).
          if (orbitAt(other) >= 0 && on < scene->nodes.size()) t = scene->nodes[on].outline.centre();
          orbitScratch.push_back({t.x + s2.x, t.y + s2.y});
        }
      }
      split[k + 1] = orbitScratch.size();
    }
    for (uint32_t k = 0; k < n.pinCount; ++k) {
      const ScenePin& pin = scene->pins[n.firstPin + k];
      if (pin.orbit || !pin.visible) continue;
      orbitFixed.push_back({pin.x + sh.x, pin.y + sh.y});
    }
    const std::span<const Point2> targets[2] = {std::span<const Point2>(orbitScratch.data() + split[0], split[1] - split[0]),
                                                std::span<const Point2>(orbitScratch.data() + split[1], split[2] - split[1])};
    bool on[2] = {false, false}, pinned[2] = {false, false};
    float angles[2] = {0.0f, 0.0f};
    for (int k = 0; k < 2; ++k) {
      on[k] = idx[k] >= 0;
      if (!on[k]) continue;
      const OrbitRef& o = orbits[static_cast<size_t>(idx[k])];
      pinned[k] = o.pinned || o.held;
      angles[k] = o.held ? o.heldAngle : scene->pins[o.pin].angle;
    }
    solveOrbits(ol, nodeScale(v), on, pinned, targets, orbitFixed, angles);
    bool changed = false;
    for (int k = 0; k < 2; ++k) {
      if (!on[k]) continue;
      OrbitRef& o = orbits[static_cast<size_t>(idx[k])];
      if (o.target == angles[k]) continue;
      o.target = angles[k];
      changed = true;
    }
    return changed;
  }
  // The nodes a lift moves the ports of: the lifted blocks' and those with a cable to one (orbitLive).
  void buildOrbitLive() {
    orbitLive.clear();
    if (orbits.empty() || !anyLift()) return;
    for (const OrbitRef& o : orbits) {
      bool touched = lifted(o.node);
      for (uint32_t e = 0; !touched && e < o.endCount; ++e) {
        const CableRef& c = cables[orbitEnds[o.firstEnd + e]];
        touched = lifted(c.fromNode) || lifted(c.toNode);
      }
      if (touched) orbitLive.push_back(o.node);
    }
    std::sort(orbitLive.begin(), orbitLive.end());
    orbitLive.erase(std::unique(orbitLive.begin(), orbitLive.end()), orbitLive.end());
  }
  // The ports of `nodes` toward their (new) targets: animated (frames run), or at once (orbitAnimated off). The caller
  // refreshes the cables' ends.
  void retarget(std::span<const uint32_t> nodes) {
    bool any = false;
    for (const uint32_t v : nodes) any = solveNode(v) || any;
    if (!any) return;
    if (orbitAnimated) {
      self.requestFrame();
      return;
    }
    for (OrbitRef& o : orbits)
      if (o.cur != o.target) {
        damagePort(o);
        o.cur = o.target;
        damagePort(o);
      }
  }
  // Damages a port's drawn rect (with its glow) where it is now.
  void damagePort(const OrbitRef& o) {
    ui::UiTree* tr = self.tree();
    if (tr == nullptr || !scene) return;
    const Point2 p = orbitPos(o, true);
    const float r = (kOrbitPortR * 2.4f) * nodeScale(o.node) * s() + 3.0f * uiScale();
    const ui::IRect cr = self.rect();
    const ui::Rect d{std::max(devX(p.x) - r, static_cast<float>(cr.x)), std::max(devY(p.y) - r, static_cast<float>(cr.y)),
                     std::min(devX(p.x) + r, static_cast<float>(cr.right())), std::min(devY(p.y) + r, static_cast<float>(cr.bottom()))};
    if (d.x1 > d.x0 && d.y1 > d.y0) tr->damage().add(d);
  }
  // Advances the slides to frame time `now` (onFrame): the ports that moved, their cables' ends (they swing).
  void stepOrbits(double now) {
    if (!orbitsMoving()) {
      orbitLast = -1.0;
      return;
    }
    const double dt = orbitLast < 0.0 ? 1.0 / 60.0 : std::clamp(now - orbitLast, 0.0, 0.1);
    orbitLast = now;
    const float k = static_cast<float>(1.0 - std::exp(-dt / kOrbitEaseSeconds));
    for (OrbitRef& o : orbits) {
      if (o.cur == o.target) continue;
      damagePort(o);
      const float d = angleDelta(o.cur, o.target);
      o.cur = std::fabs(d) <= kOrbitSnap || lod() == Lod::Far ? o.target : wrapAngle(o.cur + d * k);
      if (std::fabs(angleDelta(o.cur, o.target)) <= kOrbitSnap) o.cur = o.target;
      damagePort(o);
    }
    refreshOrbitCables();
    if (orbitsMoving()) self.requestFrame();
    else orbitLast = -1.0;
  }
  // The cables on orbiting ports to their ports' places now (a cable whose ends did not move is skipped).
  void refreshOrbitCables() {
    bool moved = false;
    for (const OrbitRef& o : orbits)
      for (uint32_t e = 0; e < o.endCount; ++e) {
        const uint32_t i = orbitEnds[o.firstEnd + e];
        Point2 a, b;
        cableEnds(cables[i], a, b);
        if (std::max({std::fabs(a.x - sim.endA(i).x), std::fabs(a.y - sim.endA(i).y), std::fabs(b.x - sim.endB(i).x),
                      std::fabs(b.y - sim.endB(i).y)}) < 1e-3f)
          continue;
        sim.setEnds(i, a, b, sim.animated());
        damageCable(i);
        moved = true;
      }
    if (moved) {
      cgVersion = ~0ull;
      cableMoved();
    }
  }


  // The scene's cables into the sim (kept connections keep their motion and swing to new ends).
  void syncCables() {
    std::vector<std::pair<uint64_t, float>> glowOf;  // the glow carries over by connection
    for (uint32_t i = 0; i < cables.size() && i < sim.size(); ++i)
      if (cables[i].glow > 0.0f) glowOf.emplace_back(simKeys.size() > i ? simKeys[i] : 0, cables[i].glow);
    cables.clear();
    simKeys.clear();
    cableOf.assign(scene ? scene->wires.size() : 0, -1);
    std::vector<CableSim::Spec> specs;
    for (uint32_t w = 0; scene && w < scene->wires.size(); ++w) {
      const SceneWire& sw = scene->wires[w];
      if (!sw.cable || sw.hidden || sw.pointCount < 2) continue;
      CableRef c;
      c.wire = w;
      c.restA = scene->points[sw.firstPoint];
      c.restB = scene->points[sw.firstPoint + sw.pointCount - 1];
      c.fromNode = wireEnd(*scene, sw, false);
      c.toNode = wireEnd(*scene, sw, true);
      CableSim::Spec sp;
      sp.key = sw.cableKey != 0 ? sw.cableKey : (0xC0FFEEull << 32 | w);
      for (const auto& [k, g] : glowOf)
        if (k == sp.key) c.glow = g;
      cableOf[w] = static_cast<int32_t>(cables.size());
      cableEnds(c, sp.a, sp.b);
      specs.push_back(sp);
      simKeys.push_back(sp.key);
      cables.push_back(c);
    }
    sim.sync(specs);
    cgVersion = ~0ull;
    if (looseOn) adoptLoose();
    cableMoved();
  }
  std::vector<uint64_t> simKeys;  // per cable: its key

  // ---- TX.node-menu-fix1: the loose cable (a new connection being drawn out) ----
  CableSim loose;                 // one cable (key 1) while looseOn
  bool looseOn = false, looseDropped = false, looseSnap = false;
  WireStyle looseStyle{};
  double looseLast = -1.0;        // the frame time of its last advance
  std::vector<uint64_t> looseBase;  // sorted: the cables there were when it came out of its port (not "new")
  void damageLoose() {
    if (!looseOn || loose.size() == 0) return;
    const CableSim::Box &a = loose.previousBounds(0), &b = loose.bounds(0);
    const float extra = 2.0f / std::max(s(), 1e-3f);  // the snap halo
    damageCableBox({std::min(a.x0, b.x0) - extra, std::min(a.y0, b.y0) - extra, std::max(a.x1, b.x1) + extra, std::max(a.y1, b.y1) + extra});
  }
  void clearLoose() {
    if (!looseOn) return;
    damageLoose();
    looseOn = looseDropped = looseSnap = false;
    loose.sync({});
    looseLast = -1.0;
  }
  // A new scene: a NEW cable (none of looseBase: a spawn's cable may first appear where its block was laid out, and
  // reach the drop point with the next scene, which places the block there) whose one end is where the loose cable is plugged in (its port) and whose other end is
  // nearest the loose cable's free end takes the loose cable's motion (the dropped cable keeps swinging, no pop), and
  // the loose cable goes. Within kLooseAdoptReach world units of the free end (a spawned node's input is about a
  // block's width from where the cable was dropped); none: it stays (on the pointer, or dropped: it goes next frame).
  static constexpr float kLooseAdoptReach = 160.0f, kLoosePortReach = 24.0f;
  void adoptLoose() {
    if (!looseOn || loose.size() == 0 || cables.empty()) return;
    const std::vector<uint64_t>& sortedOld = looseBase;
    const Point2 port = loose.endA(0), free = loose.endB(0);
    uint32_t best = UINT32_MAX;
    bool bestRev = false;
    float bestD = kLooseAdoptReach + kLoosePortReach;
    for (uint32_t i = 0; i < cables.size(); ++i) {
      if (std::binary_search(sortedOld.begin(), sortedOld.end(), simKeys[i])) continue;  // not new
      const Point2 a = sim.endA(i), b = sim.endB(i);
      for (int rev = 0; rev < 2; ++rev) {
        const Point2 at = rev ? b : a, other = rev ? a : b;
        // From the loose cable's port (a control's rim moves with the cable's direction: a little reach).
        const float pd = std::hypot(at.x - port.x, at.y - port.y);
        if (pd > kLoosePortReach) continue;
        const float d = pd + std::hypot(other.x - free.x, other.y - free.y);
        if (d < bestD) best = i, bestRev = rev != 0, bestD = d;
      }
    }
    if (best == UINT32_MAX) return;
    sim.adopt(best, loose, 0, bestRev);
    damageCable(best);
    clearLoose();
    cgVersion = ~0ull;
  }
  // Advances the loose cable (onFrame); a dropped one that nothing adopted goes.
  void stepLoose(double now) {
    if (!looseOn) return;
    if (looseDropped) {
      clearLoose();
      return;
    }
    if (!loose.awake()) {
      looseLast = -1.0;
      return;
    }
    const double dt = looseLast < 0.0 ? kCableStep : now - looseLast;
    looseLast = now;
    if (loose.advance(dt)) damageLoose();
    if (loose.awake()) self.requestFrame();
    else looseLast = -1.0;
  }

  // Ends may have moved (the sim woke): run frames (far: settled at once).
  void cableMoved() {
    if (!sim.awake()) return;
    if (lod() == Lod::Far) {
      sim.settle();
      return;
    }
    self.requestFrame();
  }

  // Tube width in device px at the current zoom.
  float cableWidth() const noexcept { return std::clamp(6.0f * s(), 1.5f * uiScale(), 16.0f * uiScale()); }

  // A cable's damage rect for world box b (device px), padded by its shadow, plugs and glow.
  void damageCableBox(const CableSim::Box& b) {
    ui::UiTree* tr = self.tree();
    if (tr == nullptr) return;
    const float pad = 4.5f * cableWidth() + 6.0f;
    const ui::IRect cr = self.rect();
    const ui::Rect d{std::max(devX(b.x0) - pad, static_cast<float>(cr.x)), std::max(devY(b.y0) - pad, static_cast<float>(cr.y)),
                     std::min(devX(b.x1) + pad, static_cast<float>(cr.right())), std::min(devY(b.y1) + pad, static_cast<float>(cr.bottom()))};
    if (d.x1 > d.x0 && d.y1 > d.y0) tr->damage().add(d);
  }
  void damageCable(uint32_t i) {
    const CableSim::Box &a = sim.previousBounds(i), &b = sim.bounds(i);
    damageCableBox({std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)});
  }

  // Moves the ends of the cables whose ends changed (a drag offset, an anchor); damages and wakes them. `animate`
  // false: onto their new rest shapes at once.
  void refreshCableEnds(bool animate) {
    bool any = false;
    for (uint32_t i = 0; i < cables.size(); ++i) {
      Point2 a, b;
      cableEnds(cables[i], a, b);
      if (std::max({std::fabs(a.x - sim.endA(i).x), std::fabs(a.y - sim.endA(i).y), std::fabs(b.x - sim.endB(i).x),
                    std::fabs(b.y - sim.endB(i).y)}) < 1e-3f)
        continue;
      sim.setEnds(i, a, b, animate);
      damageCable(i);
      any = true;
    }
    if (any) {
      cgVersion = ~0ull;
      cableMoved();
    }
  }

  // Advances the motion to frame time `now` (onFrame); damages what moved.
  void stepCables(double now) {
    if (!sim.awake()) {
      simLast = -1.0;
      return;
    }
    if (lod() == Lod::Far) {  // far: no motion (the chunks show the rest shapes)
      for (uint32_t i = 0; i < cables.size(); ++i)
        if (sim.awake(i)) damageCable(i);
      sim.settle();
      simLast = -1.0;
      return;
    }
    const double dt = simLast < 0.0 ? kCableStep : now - simLast;
    simLast = now;
    movedScratch.clear();
    for (uint32_t i = 0; i < cables.size(); ++i)
      if (sim.awake(i)) movedScratch.push_back(i);
    if (sim.advance(dt))
      for (const uint32_t i : movedScratch) damageCable(i);
    if (sim.awake()) self.requestFrame();
    else simLast = -1.0;
  }

  // ---- glow ----
  bool shownInTree() const noexcept {
    for (const ui::Widget* w = &self; w != nullptr; w = w->parent())
      if (!w->visible()) return false;
    return self.tree() != nullptr && !self.rect().empty();
  }
  // ≤ 30 Hz while the sources are live (and once after): the visible cables' levels, smoothed; glowing cables repaint
  // (their light flows), silent ones do not; nothing visible to light: no poll.
  void pollCables() {
    ++stats.cableGlowPolls;
    const bool live = self.cableSignalLive ? self.cableSignalLive() : false;
    const bool visible = shownInTree() && lod() != Lod::Far && scene != nullptr;
    glowWires.clear();
    if (visible && self.cableLevels) {
      const ui::IRect r = self.rect();
      const Point2 w0 = self.toWorld({static_cast<float>(r.x), static_cast<float>(r.y)});
      const Point2 w1 = self.toWorld({static_cast<float>(r.right()), static_cast<float>(r.bottom())});
      const float pad = 8.0f / std::max(s(), 1e-3f);
      for (uint32_t i = 0; i < cables.size(); ++i) {
        const CableSim::Box& b = sim.bounds(i);
        if (b.x1 >= w0.x - pad && b.x0 <= w1.x + pad && b.y1 >= w0.y - pad && b.y0 <= w1.y + pad) glowWires.push_back(cables[i].wire);
      }
    }
    glowLevels.assign(glowWires.size(), 0.0f);
    if (!glowWires.empty() && (live || glowWasLive)) self.cableLevels(glowWires, glowLevels);
    bool glowing = false;
    size_t k = 0;
    for (uint32_t i = 0; i < cables.size(); ++i) {
      CableRef& c = cables[i];
      float target = 0.0f;
      if (k < glowWires.size() && glowWires[k] == c.wire) target = std::clamp(glowLevels[k++], 0.0f, 1.0f);
      const float before = c.glow;
      c.glow = target >= c.glow ? target : c.glow * 0.78f;  // fast attack, ≈ 130 ms release
      if (c.glow < 0.01f) c.glow = 0.0f;
      if (c.glow > 0.0f || before > 0.0f) damageCable(i);
      glowing = glowing || c.glow > 0.0f;
    }
    const bool again = visible && (live || glowing);
    glowWasLive = live;
    if (again && glowTimer == 0) glowTimer = self.requestTimer(kCableGlowSeconds);
  }

  // ---- hit testing ----
  void buildCableGrid() const {
    if (cgVersion == sim.version() && cgScene == scene.get()) return;
    cgVersion = sim.version();
    cgScene = scene.get();
    cgBegin.clear();
    cgItems.clear();
    cgCols = cgRows = 0;
    if (cables.empty()) return;
    CableSim::Box all = sim.bounds(0);
    for (uint32_t i = 1; i < cables.size(); ++i) {
      const CableSim::Box& b = sim.bounds(i);
      all.x0 = std::min(all.x0, b.x0), all.y0 = std::min(all.y0, b.y0), all.x1 = std::max(all.x1, b.x1), all.y1 = std::max(all.y1, b.y1);
    }
    cgX0 = all.x0 - 16.0f;
    cgY0 = all.y0 - 16.0f;
    cgCols = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil((all.x1 + 16.0f - cgX0) / kCableCell)));
    cgRows = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil((all.y1 + 16.0f - cgY0) / kCableCell)));
    const uint32_t cells = cgCols * cgRows;
    cgBegin.assign(cells + 1, 0);
    auto range = [&](const CableSim::Box& b, uint32_t& c0, uint32_t& r0, uint32_t& c1, uint32_t& r1) {
      auto cl = [](float v, uint32_t n) { return static_cast<uint32_t>(std::clamp(v, 0.0f, static_cast<float>(n - 1))); };
      c0 = cl((b.x0 - 8.0f - cgX0) / kCableCell, cgCols), c1 = cl((b.x1 + 8.0f - cgX0) / kCableCell, cgCols);
      r0 = cl((b.y0 - 8.0f - cgY0) / kCableCell, cgRows), r1 = cl((b.y1 + 8.0f - cgY0) / kCableCell, cgRows);
    };
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<uint32_t> cursor;
      if (pass == 1) {
        for (uint32_t c = 0; c < cells; ++c) cgBegin[c + 1] += cgBegin[c];
        cgItems.resize(cgBegin[cells]);
        cursor.assign(cgBegin.begin(), cgBegin.end() - 1);
      }
      for (uint32_t i = 0; i < cables.size(); ++i) {
        uint32_t c0, r0, c1, r1;
        range(sim.bounds(i), c0, r0, c1, r1);
        for (uint32_t r = r0; r <= r1; ++r)
          for (uint32_t c = c0; c <= c1; ++c) {
            const uint32_t cell = r * cgCols + c;
            if (pass == 0) ++cgBegin[cell + 1];
            else cgItems[cursor[cell]++] = i;
          }
      }
    }
  }

  // The cable nearest world point w within `reach` (world units, < 8) of its rope — or, endsOnly, of one of its ends —
  // as its scene wire; UINT32_MAX: none. `dist` receives the distance.
  uint32_t cableAt(Point2 w, float reach, bool endsOnly, float& dist) const {
    dist = reach;
    buildCableGrid();
    if (cgCols == 0) return UINT32_MAX;
    const auto cx = static_cast<int64_t>(std::floor((w.x - cgX0) / kCableCell));
    const auto cy = static_cast<int64_t>(std::floor((w.y - cgY0) / kCableCell));
    if (cx < 0 || cy < 0 || cx >= static_cast<int64_t>(cgCols) || cy >= static_cast<int64_t>(cgRows)) return UINT32_MAX;
    const uint32_t cell = static_cast<uint32_t>(cy) * cgCols + static_cast<uint32_t>(cx);
    uint32_t best = UINT32_MAX;
    for (uint32_t k = cgBegin[cell]; k < cgBegin[cell + 1]; ++k) {
      const uint32_t i = cgItems[k];
      const CableSim::Box& b = sim.bounds(i);
      if (w.x < b.x0 - reach || w.x > b.x1 + reach || w.y < b.y0 - reach || w.y > b.y1 + reach) continue;
      const std::span<const Point2> p = sim.points(i);
      if (endsOnly) {
        for (const Point2 e : {p.front(), p.back()}) {
          const float d = std::hypot(w.x - e.x, w.y - e.y);
          if (d <= dist) dist = d, best = cables[i].wire;
        }
        continue;
      }
      for (size_t j = 0; j + 1 < p.size(); ++j) {
        const float ex = p[j + 1].x - p[j].x, ey = p[j + 1].y - p[j].y;
        const float len2 = ex * ex + ey * ey;
        const float t = len2 > 0.0f ? std::clamp(((w.x - p[j].x) * ex + (w.y - p[j].y) * ey) / len2, 0.0f, 1.0f) : 0.0f;
        const float d = std::hypot(w.x - (p[j].x + ex * t), w.y - (p[j].y + ey * t));
        if (d <= dist) dist = d, best = cables[i].wire;
      }
    }
    return best;
  }

  // Whether device point d is on a face's control (a press there goes to it; cables there yield but for their ends).
  bool onFaceControl(Vec2 d) const {
    const int32_t i = faceUnder(d);
    if (i < 0 || lifted(faces[static_cast<size_t>(i)].node)) return false;  // TX.block-drag: a lifted face is being moved
    const Hosted& h = faces[static_cast<size_t>(i)];
    return h.face->hitTest(localOf(h, self.toWorld(d)), s() * h.scale) >= 0;
  }
  // The hit radii (world units): the rope (4 px, or the tube's half width and a pixel), a plug.
  float cableReach() const noexcept { return std::max(4.0f * uiScale(), 0.5f * cableWidth() + uiScale()) / s(); }
  float plugReach() const noexcept { return std::max(6.0f * uiScale(), 1.3f * cableWidth()) / s(); }
  // A cable's end (its plug) under device point d: the scene wire, UINT32_MAX: none.
  uint32_t cableEndAt(Vec2 d) const {
    if (cables.empty() || lod() == Lod::Far || !self.rect().contains(d)) return UINT32_MAX;
    float dist = 0.0f;
    return cableAt(self.toWorld(d), plugReach(), true, dist);
  }

  // `which`: −1 every cable, 0 those not touching a lifted block, 1 those that do (and the loose cable).
  void paintCables(ui::PaintContext& ctx, Point2 w0, Point2 w1, int8_t which = -1);
  // TX.orbit-ports: the orbiting ports of the pass's nodes (showNode) — an input a hollow ring, an output a filled disc
  // with a bright core and a soft glow, the same size (the block's scale), in their cables' colour.
  void paintPorts(ui::PaintContext& ctx, Point2 w0, Point2 w1);
  // TX.orbit-ports: the faces' floating panels (Face::popupRect), last.
  void paintPopups(ui::PaintContext& ctx);
  // The face whose floating panel is under device point d (-1: none).
  int32_t popupUnder(Vec2 d) const noexcept {
    if (!facesShown || !scene || !self.rect().contains(d)) return -1;
    const Point2 w = self.toWorld(d);
    for (size_t i = faces.size(); i-- > 0;) {
      const std::optional<ui::Rect> r = faces[i].face->popupRect();
      if (!r || scene->nodes[faces[i].node].hidden) continue;
      const ui::Vec2 l = localOf(faces[i], w);
      if (l.x >= r->x0 && l.x <= r->x1 && l.y >= r->y0 && l.y <= r->y1) return static_cast<int32_t>(i);
    }
    return -1;
  }
  // TX.block-drag: a node's scale for the cables' plugs on it (1: none).
  float nodeScale(uint32_t v) const noexcept {
    return scene && v < scene->nodes.size() && scene->nodes[v].scale > 0.0f ? scene->nodes[v].scale : 1.0f;
  }

  explicit Impl(CircuitCanvas& c) : self(c) {}

  float uiScale() const noexcept { return self.tree() ? self.tree()->scale() : 1.0f; }
  float s() const noexcept { return static_cast<float>(view.zoom) * uiScale(); }
  float devX(float wx) const noexcept {
    return static_cast<float>(self.rect().x) + static_cast<float>((static_cast<double>(wx) - view.x) * view.zoom) * uiScale();
  }
  float devY(float wy) const noexcept {
    return static_cast<float>(self.rect().y) + static_cast<float>((static_cast<double>(wy) - view.y) * view.zoom) * uiScale();
  }
  Lod lod() const noexcept {
    if (view.zoom < kFarZoom) return Lod::Far;
    return view.zoom < kNearZoom ? Lod::Mid : Lod::Near;
  }

  void resetSelectionFlags() {
    const size_t nn = scene ? scene->nodes.size() : 0, nw = scene ? scene->wires.size() : 0;
    nodeSel.assign(nn, 0);
    wireSel.assign(nw, 0);
    if (wireLit.size() != nw) wireLit.assign(nw, 0);
    for (uint32_t v : selection.nodes)
      if (v < nn) nodeSel[v] = 1;
    for (uint32_t w : selection.wires)
      if (w < nw) wireSel[w] = 1;
    nodeStamp.assign(nn, 0);
    segStamp.assign(scene ? scene->segments.size() : 0, 0);
  }

  void selectionChanged() {
    resetSelectionFlags();
    self.markDirty();
    if (self.onSelectionChanged) self.onSelectionChanged();
  }

  void fit() {
    const ui::IRect& r = self.rect();
    if (!scene || r.empty()) {
      fitPending = true;
      return;
    }
    fitPending = false;
    const double margin = 24.0;
    const double bw = static_cast<double>(scene->x1 - scene->x0) + 2 * margin, bh = static_cast<double>(scene->y1 - scene->y0) + 2 * margin;
    const double lw = r.w / static_cast<double>(uiScale()), lh = r.h / static_cast<double>(uiScale());
    double z = std::min(lw / std::max(bw, 1.0), lh / std::max(bh, 1.0));
    z = std::clamp(z, kMinZoom, 2.0);
    view.x = static_cast<double>(scene->x0 + scene->x1) * 0.5 - lw / z * 0.5;
    view.y = static_cast<double>(scene->y0 + scene->y1) * 0.5 - lh / z * 0.5;
    if (fitLeft) view.x = std::min(view.x, static_cast<double>(scene->x0) - margin);
    // M4 review M15: a patch opened (not F) never opens below the zoom where names show: too large to fit, it opens
    // at the Near zoom on its outputs (the right edge, vertically on the output terminals), as a schematic is read.
    if (fitReadable && z < kNearZoom) {
      z = kNearZoom;
      float oy0 = scene->y1, oy1 = scene->y0;
      for (const SceneNode& n : scene->nodes)
        if (scene->specs[n.spec].symbol == Symbol::Output || scene->specs[n.spec].symbol == Symbol::Export) {
          oy0 = std::min(oy0, n.y);
          oy1 = std::max(oy1, n.y + n.h);
        }
      const double cy = oy1 >= oy0 ? 0.5 * static_cast<double>(oy0 + oy1) : 0.5 * static_cast<double>(scene->y0 + scene->y1);
      view.x = static_cast<double>(scene->x1) + margin - lw / z;
      if (bh * z <= lh) view.y = static_cast<double>(scene->y0 + scene->y1) * 0.5 - lh / z * 0.5;  // fits: centred
      else
        view.y = std::clamp(cy - lh / z * 0.5, static_cast<double>(scene->y0) - margin,
                            std::max(static_cast<double>(scene->y0) - margin, static_cast<double>(scene->y1) + margin - lh / z));
    }
    fitReadable = false;
    view.zoom = z;
    self.markDirty();
  }

  void zoomed() {
    zooming = true;
    if (settleTimer) self.cancelTimer(settleTimer);
    settleTimer = self.requestTimer(kZoomSettleSeconds);
    self.markDirty();
  }

  // ---- visibility -------------------------------------------------------------------------------------------------
  // The nodes and segments whose boxes meet the world rect, each once.
  void collectVisible(float wx0, float wy0, float wx1, float wy1) {
    visNodes.clear();
    visSegs.clear();
    const CircuitScene& sc = *scene;
    const SpatialGrid& g = sc.grid;
    uint32_t c0, r0, c1, r1;
    g.cellRange(wx0, wy0, wx1, wy1, c0, r0, c1, r1);
    const uint64_t cells = static_cast<uint64_t>(c1 - c0 + 1) * (r1 - r0 + 1);
    auto nodeIn = [&](uint32_t i) {
      const SceneNode& n = sc.nodes[i];
      return n.x <= wx1 && n.x + n.w >= wx0 && n.y - 12.0f <= wy1 && n.y + n.h >= wy0;
    };
    auto segIn = [&](uint32_t i) {
      const Point2 a = sc.points[sc.segments[i].point], b = sc.points[sc.segments[i].point + 1];
      return std::min(a.x, b.x) <= wx1 && std::max(a.x, b.x) >= wx0 && std::min(a.y, b.y) <= wy1 && std::max(a.y, b.y) >= wy0;
    };
    if (cells * 2 >= static_cast<uint64_t>(g.cols) * g.rows) {  // most of the scene: a linear pass, no dedupe
      for (uint32_t i = 0; i < sc.nodes.size(); ++i)
        if (!sc.nodes[i].hidden && nodeIn(i)) visNodes.push_back(i);
      for (uint32_t i = 0; i < sc.segments.size(); ++i)
        if (segIn(i)) visSegs.push_back(i);
      return;
    }
    if (++stamp == 0) {
      std::fill(nodeStamp.begin(), nodeStamp.end(), 0);
      std::fill(segStamp.begin(), segStamp.end(), 0);
      stamp = 1;
    }
    for (uint32_t r = r0; r <= r1; ++r)
      for (uint32_t c = c0; c <= c1; ++c) {
        const uint32_t cell = r * g.cols + c;
        for (uint32_t k = g.nodeBegin[cell]; k < g.nodeBegin[cell + 1]; ++k) {
          const uint32_t i = g.nodeItems[k];
          if (nodeStamp[i] == stamp) continue;
          nodeStamp[i] = stamp;
          if (nodeIn(i)) visNodes.push_back(i);
        }
        for (uint32_t k = g.segBegin[cell]; k < g.segBegin[cell + 1]; ++k) {
          const uint32_t i = g.segItems[k];
          if (segStamp[i] == stamp) continue;
          segStamp[i] = stamp;
          if (segIn(i)) visSegs.push_back(i);
        }
      }
    // Paint order independent of the cell walk (partial repaints equal full ones).
    std::sort(visNodes.begin(), visNodes.end());
    std::sort(visSegs.begin(), visSegs.end());
  }

  // ---- tiles ------------------------------------------------------------------------------------------------------
  float tileMargin(float scale) const noexcept { return std::ceil(strokeWidth(scale) * 0.5f + ui::kSmoothFeather + 1.0f); }

  // Rasterises both layers of `spec`'s body at `scale` into new tiles. False when the atlas has no room.
  bool buildTile(ui::TileAtlas& atlas, const SymbolSpec& spec, float scale, BodyTile& out) {
    const float m = tileMargin(scale);
    const uint32_t tw = static_cast<uint32_t>(std::ceil(spec.w * scale + 2.0f * m));
    const uint32_t th = static_cast<uint32_t>(std::ceil(spec.h * scale + 2.0f * m));
    if (tw > atlas.maxTileSize() || th > atlas.maxTileSize()) return false;
    BodyTile t;
    t.scale = scale;
    t.margin = m;
    for (int layer = 0; layer < 2; ++layer) {
      ui::TileAtlas::Tile& tile = layer == 0 ? t.fill : t.stroke;
      if (!atlas.allocate(tw, th, tile)) return false;
      scratch.reset(tw, th);
      paintBody(scratch, spec, {m, m}, scale, layer == 0 ? Layer::Fill : Layer::Stroke, ui::rgba(255, 255, 255));
      uint8_t* dst = atlas.write(tile);
      if (!dst) return false;
      const uint32_t pitch = ui::TileAtlas::rowPitch(tile);
      ui::rasterizeCoverage(scratch, dst + pitch + 1, tw, th, pitch, {0.0f, 0.0f});
    }
    out = t;
    ++stats.tilesBuilt;
    return true;
  }

  // Builds the missing tiles of the visible bodies within the frame budget. Returns true if it built any.
  bool buildVisibleTiles(ui::TileAtlas& atlas) {
    const float sc = s();
    uint64_t texels = 0;
    bool built = false;
    bool more = false;
    for (const uint32_t v : visNodes) {
      const SymbolSpec& spec = scene->specs[scene->nodes[v].spec];
      BodyTile& t = tiles[spec.bodyKey];
      const bool current = t.scale == sc && atlas.valid(t.fill) && atlas.valid(t.stroke);
      if (current || (t.failed && t.scale == sc)) continue;
      if (texels > kTileBuildTexelsPerFrame) {
        more = true;
        break;
      }
      BodyTile fresh;
      if (buildTile(atlas, spec, sc, fresh)) {
        t = fresh;
        texels += static_cast<uint64_t>(fresh.fill.w) * fresh.fill.h * 2;
        built = true;
      } else {
        t.failed = true;
        t.scale = sc;
        ++stats.tileFailures;
      }
    }
    tilesWanted = more;
    return built;
  }
};

CircuitCanvas::CircuitCanvas(std::string name) : Widget(std::move(name)), impl_(std::make_unique<Impl>(*this)) {
  setFocusable(true);
  setClipChildren(true);
  setLayout(ui::LayoutKind::Custom);  // arrange(): a fit asked for before the first layout happens there
}

void CircuitCanvas::arrange(ui::LayoutPass& /*pass*/) {
  if (impl_->fitPending && !rect().empty()) impl_->fit();
}

CircuitCanvas::~CircuitCanvas() = default;

void CircuitCanvas::setScene(std::shared_ptr<const CircuitScene> scene, bool fit, bool keepTiles) {
  impl_->scene = std::move(scene);
  impl_->clearLift();  // TX.block-drag: the lifted nodes were the old scene's (the host lifts them again in the new one)
  impl_->selection = {};
  impl_->hover = {};
  if (!keepTiles) impl_->tiles.clear();
  impl_->chunks.clear();
  impl_->chunkScale = 0.0f;
  impl_->wireLit.clear();
  impl_->resetSelectionFlags();
  impl_->buildFaces();  // TMM.4
  impl_->syncOrbits();  // TX.orbit-ports: before the cables (their ends on orbiting ports)
  impl_->syncCables();  // TX.cables: after the faces (a cable onto a control ends on its live anchor)
  impl_->linkOrbits();
  if (impl_->rescaled) impl_->zoomed();  // TX.resize: a rescaled face settles like a zoom (its tiles, a panel's layout)
  if (fit) {
    impl_->fitReadable = true;
    impl_->fit();
  }
  markDirty();
}

const std::shared_ptr<const CircuitScene>& CircuitCanvas::scene() const noexcept { return impl_->scene; }
const CircuitCanvas::View& CircuitCanvas::view() const noexcept { return impl_->view; }

void CircuitCanvas::setView(const View& v) {
  View n = v;
  n.zoom = std::clamp(n.zoom, kMinZoom, kMaxZoom);
  if (n == impl_->view) return;
  const bool zoomChanged = n.zoom != impl_->view.zoom;
  impl_->view = n;
  impl_->fitPending = false;
  if (zoomChanged) impl_->zoomed();
  else markDirty();
}

void CircuitCanvas::fitToRead() {
  const double z = impl_->view.zoom;
  impl_->fitReadable = true;
  impl_->fit();
  if (impl_->view.zoom != z) impl_->zoomed();
}

void CircuitCanvas::fitAll() {
  const double z = impl_->view.zoom;
  impl_->fitReadable = false;
  impl_->fit();
  if (impl_->view.zoom != z) impl_->zoomed();
}

void CircuitCanvas::zoomAt(Vec2 device, double factor) {
  View v = impl_->view;
  const double z = std::clamp(v.zoom * factor, kMinZoom, kMaxZoom);
  if (z == v.zoom) return;
  const double sc = impl_->uiScale();
  const double lx = (device.x - rect().x) / sc, ly = (device.y - rect().y) / sc;  // logical px from the corner
  const double wx = v.x + lx / v.zoom, wy = v.y + ly / v.zoom;
  v.zoom = z;
  v.x = wx - lx / z;
  v.y = wy - ly / z;
  setView(v);
}

void CircuitCanvas::panBy(float dx, float dy) {
  View v = impl_->view;
  const double k = 1.0 / (v.zoom * impl_->uiScale());
  v.x -= dx * k;
  v.y -= dy * k;
  setView(v);
}

float CircuitCanvas::deviceScale() const noexcept { return impl_->s(); }
Vec2 CircuitCanvas::toDevice(Point2 w) const noexcept { return {impl_->devX(w.x), impl_->devY(w.y)}; }
Point2 CircuitCanvas::toWorld(Vec2 d) const noexcept {
  const double k = 1.0 / (impl_->view.zoom * impl_->uiScale());
  return {static_cast<float>(impl_->view.x + (d.x - rect().x) * k), static_cast<float>(impl_->view.y + (d.y - rect().y) * k)};
}
CircuitCanvas::Lod CircuitCanvas::lod() const noexcept { return impl_->lod(); }
const CircuitCanvas::Selection& CircuitCanvas::selection() const noexcept { return impl_->selection; }
const CircuitCanvas::Stats& CircuitCanvas::stats() const noexcept { return impl_->stats; }
bool CircuitCanvas::zooming() const noexcept { return impl_->zooming; }
bool CircuitCanvas::tilesPending() const noexcept { return impl_->tilesWanted; }
void CircuitCanvas::dropTiles() {
  impl_->tiles.clear();
  impl_->faceTiles.clear();
  impl_->chunks.clear();
  impl_->chunkScale = 0.0f;
  markDirty();
}

bool CircuitCanvas::nodeSelected(uint32_t v) const noexcept { return v < impl_->nodeSel.size() && impl_->nodeSel[v]; }
bool CircuitCanvas::wireSelected(uint32_t w) const noexcept { return w < impl_->wireSel.size() && impl_->wireSel[w]; }

void CircuitCanvas::select(std::span<const uint32_t> nodes, std::span<const uint32_t> wires, bool add) {
  Selection s = add ? impl_->selection : Selection{};
  s.nodes.insert(s.nodes.end(), nodes.begin(), nodes.end());
  s.wires.insert(s.wires.end(), wires.begin(), wires.end());
  for (auto* v : {&s.nodes, &s.wires}) {
    std::sort(v->begin(), v->end());
    v->erase(std::unique(v->begin(), v->end()), v->end());
  }
  if (s.nodes == impl_->selection.nodes && s.wires == impl_->selection.wires) return;
  impl_->selection = std::move(s);
  impl_->selectionChanged();
}

void CircuitCanvas::clearSelection() {
  if (impl_->selection.nodes.empty() && impl_->selection.wires.empty()) return;
  impl_->selection = {};
  impl_->selectionChanged();
}

void CircuitCanvas::setPinStubsNearOnly(bool on) {
  if (impl_->stubsNearOnly == on) return;
  impl_->stubsNearOnly = on;
  markDirty();
}
bool CircuitCanvas::pinStubsNearOnly() const noexcept { return impl_->stubsNearOnly; }
void CircuitCanvas::setFitAlignLeft(bool on) noexcept { impl_->fitLeft = on; }

void CircuitCanvas::setTerminalPillMinZoom(double zoom) {
  if (impl_->pillMinZoom == zoom) return;
  impl_->pillMinZoom = zoom;
  markDirty();
}

void CircuitCanvas::setWireLit(uint32_t wire, bool lit) {
  if (wire >= impl_->wireLit.size() || (impl_->wireLit[wire] != 0) == lit) return;
  impl_->wireLit[wire] = lit ? 1 : 0;
  markDirty();
}

CanvasHit CircuitCanvas::hitTest(Vec2 d) const noexcept {
  const Impl& m = *impl_;
  if (!m.scene || !rect().contains(d)) return {};
  const CircuitScene& sc = *m.scene;
  const float s = m.s();
  const Point2 w = toWorld(d);
  const float pinR = 5.0f * m.uiScale() / s, wireR = 4.0f * m.uiScale() / s;
  uint32_t c0, r0, c1, r1;
  sc.grid.cellRange(w.x - pinR, w.y - pinR, w.x + pinR, w.y + pinR, c0, r0, c1, r1);
  CanvasHit best;
  float bestPin = pinR;
  // TX.block-drag: lifted blocks first (drawn over the rest), where they are drawn: the point moved back by their offset.
  // TX.drag-grow: then the shifted nodes, each where its layer has it.
  for (const std::span<const uint32_t> set : {std::span<const uint32_t>(m.dragNodes), std::span<const uint32_t>(m.shiftNodes)}) {
    for (const uint32_t v : set) {
      const SceneNode& n = sc.nodes[v];
      if (n.hidden) continue;
      const Point2 l{w.x - m.shiftOf(v).x, w.y - m.shiftOf(v).y};
      for (uint32_t p = 0; p < n.pinCount; ++p) {
        const ScenePin& pin = sc.pins[n.firstPin + p];
        if (!pin.visible) continue;
        const Point2 at = m.pinXY(n.firstPin + p);  // TX.orbit-ports: an orbiting port where it is now
        const float dd = std::max(std::fabs(at.x - l.x), std::fabs(at.y - l.y));
        if (dd <= bestPin) {
          bestPin = dd;
          best = {CanvasHit::Kind::Pin, n.firstPin + p};
        }
      }
    }
    if (best.kind != CanvasHit::Kind::None) return best;
  }
  // TX.orbit-ports: an orbiting port sits outside its block (up to a port's width at the block's largest scale): the
  // nodes of the cells that far around.
  if (!m.orbits.empty()) {
    const float reach = pinR + 2.0f * (kOrbitPortR + kOrbitGap) * 3.0f;
    sc.grid.cellRange(w.x - reach, w.y - reach, w.x + reach, w.y + reach, c0, r0, c1, r1);
  }
  for (uint32_t r = r0; r <= r1; ++r)
    for (uint32_t c = c0; c <= c1; ++c) {
      const uint32_t cell = r * sc.grid.cols + c;
      for (uint32_t k = sc.grid.nodeBegin[cell]; k < sc.grid.nodeBegin[cell + 1]; ++k) {
        if (m.lifted(sc.grid.nodeItems[k])) continue;
        const SceneNode& n = sc.nodes[sc.grid.nodeItems[k]];
        for (uint32_t p = 0; p < n.pinCount; ++p) {
          const ScenePin& pin = sc.pins[n.firstPin + p];
          if (!pin.visible) continue;
          const Point2 at = m.pinXY(n.firstPin + p);  // TX.orbit-ports
          const float dd = std::max(std::fabs(at.x - w.x), std::fabs(at.y - w.y));
          if (dd <= bestPin) {
            bestPin = dd;
            best = {CanvasHit::Kind::Pin, n.firstPin + p};
          }
        }
      }
    }
  if (best.kind != CanvasHit::Kind::None) return best;
  // TX.cables: drawn over the blocks, so before them: the simulated rope within 4 px (or the tube's half width); over a
  // face's control only a cable's plug (a press there goes to the control).
  if (!m.cables.empty()) {
    const bool control = m.onFaceControl(d);
    float dist = 0.0f;
    if (const uint32_t cw = m.cableAt(w, control ? m.plugReach() : m.cableReach(), control, dist); cw != UINT32_MAX)
      return {CanvasHit::Kind::Wire, cw};
  }
  // TX.block-drag: then the lifted blocks' bodies (a cable under one still hits: a splice finds the cable it is over).
  for (const std::span<const uint32_t> set : {std::span<const uint32_t>(m.dragNodes), std::span<const uint32_t>(m.shiftNodes)})
    for (size_t i = set.size(); i-- > 0;) {
      const SceneNode& n = sc.nodes[set[i]];
      const float lx = w.x - m.shiftOf(set[i]).x, ly = w.y - m.shiftOf(set[i]).y;
      if (!n.hidden && lx >= n.x && lx <= n.x + n.w && ly >= n.y && ly <= n.y + n.h) return {CanvasHit::Kind::Node, set[i]};
    }
  sc.grid.cellRange(w.x, w.y, w.x, w.y, c0, r0, c1, r1);
  const uint32_t cell = r0 * sc.grid.cols + c0;
  for (uint32_t k = sc.grid.nodeBegin[cell + 1]; k-- > sc.grid.nodeBegin[cell];) {
    const uint32_t i = sc.grid.nodeItems[k];
    if (m.lifted(i)) continue;
    const SceneNode& n = sc.nodes[i];
    if (w.x >= n.x && w.x <= n.x + n.w && w.y >= n.y && w.y <= n.y + n.h) return {CanvasHit::Kind::Node, i};
  }
  sc.grid.cellRange(w.x - wireR, w.y - wireR, w.x + wireR, w.y + wireR, c0, r0, c1, r1);
  float bestWire = wireR;
  for (uint32_t r = r0; r <= r1; ++r)
    for (uint32_t c = c0; c <= c1; ++c) {
      const uint32_t cl = r * sc.grid.cols + c;
      for (uint32_t k = sc.grid.segBegin[cl]; k < sc.grid.segBegin[cl + 1]; ++k) {
        const SceneSegment& sg = sc.segments[sc.grid.segItems[k]];
        if (sc.wires[sg.wire].cable) continue;  // TX.cables: hit above, by the rope as it hangs now
        if (!m.liftWire.empty() && m.liftWire[sg.wire] != 0) continue;  // TX.block-drag: re-routed by the drop
        const Point2 a = sc.points[sg.point], b = sc.points[sg.point + 1];
        const float dx = std::max({std::min(a.x, b.x) - w.x, 0.0f, w.x - std::max(a.x, b.x)});
        const float dy = std::max({std::min(a.y, b.y) - w.y, 0.0f, w.y - std::max(a.y, b.y)});
        const float dd = std::max(dx, dy);
        if (dd <= bestWire) {
          bestWire = dd;
          best = {CanvasHit::Kind::Wire, sg.wire};
        }
      }
    }
  return best;
}

// ---- paint ----------------------------------------------------------------------------------------------------------

namespace {

// Crisp orthogonal line from a to b (device px), extended by half its thickness at both ends so corners close.
void crispSegment(ui::DrawList& dl, Vec2 a, Vec2 b, Rgba col, float t) {
  const float h = t * 0.5f;
  if (a.y == b.y) dl.addHLine(std::min(a.x, b.x) - h, std::max(a.x, b.x) + h, a.y, col, t);
  else dl.addVLine(a.x, std::min(a.y, b.y) - h, std::max(a.y, b.y) + h, col, t);
}

// Dashed crisp orthogonal line; `phase` (device px into the pattern) continues across segments.
void dashedSegment(ui::DrawList& dl, Vec2 a, Vec2 b, Rgba col, float t, float on, float off, float& phase) {
  const bool horiz = a.y == b.y;
  const float len = horiz ? std::fabs(b.x - a.x) : std::fabs(b.y - a.y);
  const float dir = horiz ? (b.x >= a.x ? 1.0f : -1.0f) : (b.y >= a.y ? 1.0f : -1.0f);
  const float period = on + off;
  float pos = 0.0f;
  while (pos < len) {
    const float inPeriod = std::fmod(phase, period);
    const bool drawing = inPeriod < on;
    const float run = std::min(len - pos, drawing ? on - inPeriod : period - inPeriod);
    if (drawing && run > 0.0f) {
      const float p0 = pos, p1 = pos + run;
      if (horiz) dl.addHLine(a.x + dir * p0, a.x + dir * p1, a.y, col, t);
      else dl.addVLine(a.x, a.y + dir * p0, a.y + dir * p1, col, t);
    }
    pos += run;
    phase += run;
  }
}

// Offsets an orthogonal polyline by d to its left (mitred corners).
void offsetPolyline(const std::vector<Vec2>& in, float d, std::vector<Vec2>& out) {
  out.resize(in.size());
  auto normal = [](Vec2 a, Vec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float l = std::max(std::fabs(dx), std::fabs(dy));
    return l > 0 ? Vec2{dy / l, -dx / l} : Vec2{0, 0};
  };
  for (size_t i = 0; i < in.size(); ++i) {
    const Vec2 np = i > 0 ? normal(in[i - 1], in[i]) : normal(in[0], in[1]);
    const Vec2 nn = i + 1 < in.size() ? normal(in[i], in[i + 1]) : np;
    Vec2 o = (np.x == nn.x && np.y == nn.y) ? np : Vec2{np.x + nn.x, np.y + nn.y};
    out[i] = {in[i].x + d * o.x, in[i].y + d * o.y};
  }
}

}  // namespace

// ---- TX.cables: drawing ---------------------------------------------------------------------------------------------

namespace {

// A strip along a device polyline: lane k at offset off[k] (device px, along each point's normal, then moved by
// `shift`) in colour col[k], each point's alpha scaled by fade[i] when given. Shared vertices: (lanes − 1) quads per
// segment, one draw command with the canvas's other geometry.
void ribbon(ui::DrawList& dl, const Vec2* p, const Vec2* nrm, uint32_t n, const float* off, const Rgba* col, uint32_t lanes,
            const float* fade = nullptr, Vec2 shift = {0.0f, 0.0f}) {
  if (n < 2 || lanes < 2) return;
  const ui::DrawList::PrimSpan sp = dl.primReserveSpan((n - 1) * (lanes - 1) * 6, n * lanes);
  const Vec2 uv = dl.whiteUv();
  ui::Vertex* v = sp.v;
  for (uint32_t i = 0; i < n; ++i) {
    const float bx = p[i].x + shift.x, by = p[i].y + shift.y;
    for (uint32_t k = 0; k < lanes; ++k, ++v) {
      v->pos = {bx + nrm[i].x * off[k], by + nrm[i].y * off[k]};
      v->uv = uv;
      v->col = fade != nullptr ? ui::smoothAlphaMul(col[k], fade[i]) : col[k];
    }
  }
  uint32_t* ix = sp.i;
  for (uint32_t i = 0; i + 1 < n; ++i)
    for (uint32_t k = 0; k + 1 < lanes; ++k) {
      const uint32_t a = sp.base + i * lanes + k, b = a + 1, c = a + lanes, d = c + 1;
      ix[0] = a, ix[1] = b, ix[2] = d, ix[3] = a, ix[4] = d, ix[5] = c;
      ix += 6;
    }
}

// The tube's cross-section (7 lanes, from its upper edge down): a feathered dark rim, a specular highlight above the
// middle, the body, a darker lower side, the feathered lower rim. Lit from above, as the shadow falls below.
constexpr uint32_t kTubeLanes = 7;
void tubeProfile(Rgba col, float hw, float feather, float alpha, float* off, Rgba* cols) {
  const Rgba rim = ui::scaleRgb(col, 0.5f), low = ui::scaleRgb(col, 0.34f);
  const Rgba hi = ui::lerp(col, ui::rgba(255, 255, 255), 0.62f), light = ui::lerp(col, ui::rgba(255, 255, 255), 0.2f);
  const float o[kTubeLanes] = {hw + feather, hw, 0.6f * hw, 0.3f * hw, -0.3f * hw, -hw, -hw - feather};
  const Rgba c[kTubeLanes] = {ui::withAlpha(rim, 0), rim, hi, light, col, low, ui::withAlpha(low, 0)};
  for (uint32_t k = 0; k < kTubeLanes; ++k) {
    off[k] = o[k];
    cols[k] = ui::mulAlpha(c[k], alpha);
  }
}

// A plug on a cable's end: a socket ring at the port and a sleeve with a coloured collar along the cable (device px; `dir`
// from the port into the cable, unit).
// `k` (≤ 1) shortens the plug on a short cable, so its rope still shows between the two.
void cablePlug(ui::DrawList& dl, Vec2 at, Vec2 dir, float w, Rgba col, float alpha, float k) {
  if (w < 3.0f) {
    dl.addCircleFilled(at, 0.8f * w, ui::mulAlpha(ui::scaleRgb(col, 0.7f), alpha));
    return;
  }
  Vec2 nrm{dir.y, -dir.x};
  if (nrm.y > 0.0f || (nrm.y == 0.0f && nrm.x < 0.0f)) nrm = {-nrm.x, -nrm.y};  // the upper side lit
  const Rgba metal = ui::rgba(22, 24, 28), ring = ui::rgba(150, 156, 168, 210);
  dl.addCircleFilled(at, 0.92f * w, ui::mulAlpha(metal, alpha));
  dl.addCircleStroke(at, 0.92f * w, ui::mulAlpha(ring, alpha), std::max(1.0f, 0.16f * w));
  const Vec2 nn[2] = {nrm, nrm};
  float off[kTubeLanes];
  Rgba cols[kTubeLanes];
  const float fe = std::min(1.0f, 0.25f * w);
  const float l1 = 1.95f * w * k, l2 = 2.4f * w * k;
  const Vec2 sleeve[2] = {{at.x + dir.x * 0.1f * w, at.y + dir.y * 0.1f * w}, {at.x + dir.x * l1, at.y + dir.y * l1}};
  tubeProfile(ui::lerp(ui::rgba(58, 62, 70), col, 0.22f), 0.72f * w, fe, alpha, off, cols);
  ribbon(dl, sleeve, nn, 2, off, cols, kTubeLanes);
  const Vec2 collar[2] = {sleeve[1], {at.x + dir.x * l2, at.y + dir.y * l2}};
  tubeProfile(col, 0.6f * w, fe, alpha, off, cols);
  ribbon(dl, collar, nn, 2, off, cols, kTubeLanes);
}

// The direction from a polyline's first point into it, about `reach` px along (unit; +x when degenerate).
Vec2 endDirection(const Vec2* p, uint32_t n, float reach) {
  for (uint32_t k = 1; k < n; ++k) {
    const float dx = p[k].x - p[0].x, dy = p[k].y - p[0].y, d = std::sqrt(dx * dx + dy * dy);
    if (d >= reach || k + 1 == n) return d > 1e-4f ? Vec2{dx / d, dy / d} : Vec2{1.0f, 0.0f};
  }
  return {1.0f, 0.0f};
}

}  // namespace

void CircuitCanvas::Impl::paintCables(ui::PaintContext& ctx, Point2 w0, Point2 w1, int8_t which) {
  Stats& st = stats;
  // Far: the cables are in the cached chunks; only the loose cable (TX.node-menu-fix1) is drawn here (TX.block-drag: and
  // the lifted blocks' cables, thin, as they hang now).
  const bool farLod = lod() == Lod::Far;
  const bool withLoose = looseOn && which != 0;
  auto picked = [&](const CableRef& c) {
    return which < 0 || liftWire.empty() ? which <= 0 : (liftWire[c.wire] != 0) == (which == 1);
  };
  if (farLod && which == 1 && !cables.empty()) {
    for (uint32_t i = 0; i < cables.size(); ++i) {
      if (!picked(cables[i])) continue;
      farPts.clear();
      for (const Point2 q : sim.points(i)) farPts.push_back({devX(q.x), devY(q.y)});
      ui::smoothPolyline(ctx.dl, farPts, farWireCol, std::max(0.35f, farThin - 0.6f));
    }
  }
  if ((cables.empty() || farLod) && !withLoose) return;
  const ui::Theme& th = *ctx.res.theme;
  const auto& C = th.circuit;
  const CircuitScene& sc = *scene;
  ui::DrawList& dl = ctx.dl;
  const float us = ctx.scale.factor;
  const float w = cableWidth();
  // A control's anchor moves with its face (a slider's handle, a panel laid out again after a zoom): the cable's end
  // follows it at once, onto its rest shape (a layout is no push: nothing swings). Its damage reaches the next frame
  // (this one is clipped to its own).
  bool anchorsMoved = false;
  for (uint32_t i = 0; !farLod && which != 1 && i < cables.size(); ++i) {
    const CableRef& c = cables[i];
    if (sc.wires[c.wire].control == UINT32_MAX) continue;
    Point2 a, b;
    cableEnds(c, a, b);
    if (std::max({std::fabs(a.x - sim.endA(i).x), std::fabs(a.y - sim.endA(i).y), std::fabs(b.x - sim.endB(i).x),
                  std::fabs(b.y - sim.endB(i).y)}) < 1e-3f)
      continue;
    sim.setEnds(i, a, b, false);
    damageCable(i);
    anchorsMoved = true;
  }
  if (anchorsMoved) {
    cgVersion = ~0ull;
    self.requestFrame();
  }
  // Cables over the face whose control the pointer is on fade (the control reads; a press there goes to it).
  bool fadeOn = false;
  ui::Rect fade{};
  if (faceHover >= 0 && static_cast<size_t>(faceHover) < faces.size() && faces[static_cast<size_t>(faceHover)].face->hoveredPart() >= 0) {
    const SceneNode& n = sc.nodes[faces[static_cast<size_t>(faceHover)].node];
    fade = {n.x, n.y, n.x + n.w, n.y + n.h};
    fadeOn = true;
  }
  const float pad = (4.5f * w + 6.0f) / s();
  cdraw.clear();
  cpts.clear();
  carc.clear();
  Vec2 dev[kCableMaxPoints];
  // A rope's device polyline: Catmull-Rom through its points, ≈ 12 px chords (≤ 3 per segment; the rope bends gently):
  // smooth at every zoom, few vertices.
  auto spline = [&](std::span<const Point2> P) {
    const uint32_t n = static_cast<uint32_t>(P.size());
    for (uint32_t k = 0; k < n; ++k) dev[k] = {devX(P[k].x), devY(P[k].y)};
    for (uint32_t k = 0; k + 1 < n; ++k) {
      const Vec2 p0 = dev[k > 0 ? k - 1 : 0], p1 = dev[k], p2 = dev[k + 1], p3 = dev[k + 2 < n ? k + 2 : n - 1];
      const float len = std::hypot(p2.x - p1.x, p2.y - p1.y);
      const uint32_t steps = std::clamp(static_cast<uint32_t>(std::ceil(len / 12.0f)), 1u, 3u);
      for (uint32_t t = 0; t < steps; ++t) cpts.push_back(ui::catmullRom(p0, p1, p2, p3, static_cast<float>(t) / static_cast<float>(steps)));
    }
    cpts.push_back(dev[n - 1]);
  };
  auto colourOf = [&](const WireStyle& ws, bool lit) {
    if (ws.boolean) return lit ? C.wireBoolLit : C.wireBool;
    if (ws.rate == WireRate::Const) return C.wireConst;
    if (ws.notes) return C.wireNotes;
    if (ws.modulation) return C.wireMod;
    return ws.rate == WireRate::Param ? C.wireParam : C.wireAudio;
  };
  // TX.block-drag: the tube by the smaller of its ends' scales (≤ 1), each plug by its own block's (a tiny block's
  // plugs are tiny, a large one's up to twice the zoom's).
  auto tubeOf = [&](float k) { return std::max(std::min(1.0f, us), w * std::clamp(k, 0.1f, 1.0f)); };
  auto plugOf = [&](float k) { return std::max(std::min(1.0f, us), w * std::clamp(k, 0.1f, 2.0f)); };
  for (uint32_t i = 0; !farLod && i < cables.size(); ++i) {
    const CableSim::Box& bx = sim.bounds(i);
    if (bx.x1 < w0.x - pad || bx.x0 > w1.x + pad || bx.y1 < w0.y - pad || bx.y0 > w1.y + pad) continue;
    const CableRef& c = cables[i];
    if (!picked(c)) continue;
    const SceneWire& sw = sc.wires[c.wire];
    const std::span<const Point2> P = sim.points(i);
    CableDraw d;
    d.cable = i;
    d.first = static_cast<uint32_t>(cpts.size());
    spline(P);
    d.count = static_cast<uint32_t>(cpts.size()) - d.first;
    const WireStyle& ws = sw.style;
    d.sel = c.wire < wireSel.size() && wireSel[c.wire] != 0;
    d.hov = hover.kind == CanvasHit::Kind::Wire && hover.index == c.wire;
    d.col = colourOf(ws, c.wire < wireLit.size() && wireLit[c.wire]);
    d.glow = c.glow;
    d.poly = ws.poly;
    d.alpha = sw.inactive && !d.sel && !d.hov ? kInactiveAlpha : 1.0f;
    if (fadeOn && bx.x1 >= fade.x0 && bx.x0 <= fade.x1 && bx.y1 >= fade.y0 && bx.y0 <= fade.y1)
      for (const Point2 q : P)
        if (q.x >= fade.x0 && q.x <= fade.x1 && q.y >= fade.y0 && q.y <= fade.y1) {
          d.alpha *= kCableFade;
          break;
        }
    d.flip = P.back().x < P.front().x ? -1.0f : 1.0f;
    const float ka = nodeScale(c.fromNode), kb = nodeScale(c.toNode);
    d.w = tubeOf(std::min(ka, kb));
    d.plugA = plugOf(ka);
    d.plugB = plugOf(kb);
    cdraw.push_back(d);
  }
  // TX.node-menu-fix1: the cable being drawn out, last (over the others), highlighted when it snaps to a target.
  if (withLoose && loose.size() > 0) {
    const std::span<const Point2> P = loose.points(0);
    CableDraw d;
    d.cable = UINT32_MAX;
    d.first = static_cast<uint32_t>(cpts.size());
    spline(P);
    d.count = static_cast<uint32_t>(cpts.size()) - d.first;
    d.col = colourOf(looseStyle, false);
    d.poly = looseStyle.poly;
    d.sel = looseSnap;
    d.flip = P.back().x < P.front().x ? -1.0f : 1.0f;
    d.w = d.plugA = d.plugB = w;
    cdraw.push_back(d);
  }
  if (cdraw.empty()) return;
  // Normals (the highlight on each cable's upper side) and arc lengths (the flow of light).
  cnrm.resize(cpts.size());
  carc.resize(cpts.size());
  for (const CableDraw& d : cdraw) {
    const Vec2* p = cpts.data() + d.first;
    float arc = 0.0f;
    for (uint32_t k = 0; k < d.count; ++k) {
      const Vec2 a = p[k > 0 ? k - 1 : 0], b = p[k + 1 < d.count ? k + 1 : d.count - 1];
      const float tx = b.x - a.x, ty = b.y - a.y, l = std::sqrt(tx * tx + ty * ty);
      cnrm[d.first + k] = l > 1e-6f ? Vec2{ty / l * d.flip, -tx / l * d.flip} : Vec2{0.0f, -1.0f};
      if (k > 0) arc += std::hypot(p[k].x - p[k - 1].x, p[k].y - p[k - 1].y);
      carc[d.first + k] = arc;
    }
  }
  const bool far = w < 3.0f;  // thin (mid zoom): no shadow, no glow layers beyond the core
  float off[kTubeLanes];
  Rgba cols[kTubeLanes];
  // 1. Soft drop shadows on what the cables cross, below and right of them.
  if (!far) {
    for (const CableDraw& d : cdraw) {
      if (d.w < 3.0f) continue;
      const float hw = 0.5f * d.w;
      const Vec2 shift{0.25f * d.w, 0.7f * d.w + us};
      const Rgba sh = ui::rgba(0, 0, 0, static_cast<uint32_t>(std::lround(100.0f * d.alpha)));
      const float o[3] = {1.7f * hw + us, 0.0f, -1.7f * hw - us};
      const Rgba c3[3] = {ui::withAlpha(sh, 0), sh, ui::withAlpha(sh, 0)};
      ribbon(dl, cpts.data() + d.first, cnrm.data() + d.first, d.count, o, c3, 3, nullptr, shift);
    }
  }
  // 2. The glow around lit cables (additive light, under the tubes so they stay solid).
  const double now = self.tree() != nullptr ? self.tree()->now() : 0.0;
  bool anyGlow = false;
  for (const CableDraw& d : cdraw) anyGlow = anyGlow || d.glow > 0.0f;
  if (anyGlow) {
    dl.setBlend(ui::BlendMode::Additive);
    for (const CableDraw& d : cdraw) {
      const float g = d.glow * d.alpha;
      if (g <= 0.0f) continue;
      const float hw = 0.5f * d.w;
      const float o[5] = {4.5f * hw + 2.0f * us, 1.6f * hw, 0.0f, -1.6f * hw, -4.5f * hw - 2.0f * us};
      const Rgba c5[5] = {ui::withAlpha(d.col, 0), ui::setAlpha(d.col, 0.3f * g), ui::setAlpha(d.col, 0.65f * g),
                          ui::setAlpha(d.col, 0.3f * g), ui::withAlpha(d.col, 0)};
      ribbon(dl, cpts.data() + d.first, cnrm.data() + d.first, d.count, o, c5, 5);
      ++st.cablesGlowing;
    }
    dl.setBlend(ui::BlendMode::Alpha);
  }
  // 3. The tubes (selected and hovered ones last, with a halo), their twin core when per voice, the plugs.
  for (int emph = 0; emph < 2; ++emph)
    for (const CableDraw& d : cdraw) {
      if ((d.sel || d.hov || d.cable == UINT32_MAX) != (emph == 1)) continue;  // the loose cable on top
      const Vec2* p = cpts.data() + d.first;
      const Vec2* nr = cnrm.data() + d.first;
      const float hw = 0.5f * d.w, feather = std::min(1.0f, 0.35f * hw);
      if (d.sel || d.hov) {
        const Rgba h = d.sel ? ui::withAlpha(C.selected, 230) : ui::withAlpha(C.hover, 190);
        const float o[4] = {hw + 2.6f * us, hw + 1.2f * us, -hw - 1.2f * us, -hw - 2.6f * us};
        const Rgba c4[4] = {ui::withAlpha(h, 0), h, h, ui::withAlpha(h, 0)};
        ribbon(dl, p, nr, d.count, o, c4, 4);
      }
      tubeProfile(d.col, hw, feather, d.alpha, off, cols);
      ribbon(dl, p, nr, d.count, off, cols, kTubeLanes);
      if (d.poly && d.w >= 3.0f) {  // per voice: a twin-lead (a dark groove along the middle)
        const Rgba gr = ui::mulAlpha(ui::scaleRgb(d.col, 0.3f), d.alpha);
        const float o[3] = {0.3f * hw, 0.0f, -0.3f * hw};
        const Rgba c3[3] = {ui::withAlpha(gr, 0), gr, ui::withAlpha(gr, 0)};
        ribbon(dl, p, nr, d.count, o, c3, 3);
      }
      const float chord = std::hypot(p[d.count - 1].x - p[0].x, p[d.count - 1].y - p[0].y);
      const float plugK = std::clamp(chord / (3.5f * (d.plugA + d.plugB)), 0.3f, 1.0f);
      const float reach = 2.4f * d.plugA * plugK, reachB = 2.4f * d.plugB * plugK;
      cablePlug(dl, p[0], endDirection(p, d.count, reach), d.plugA, d.col, d.alpha, plugK);
      Vec2 back{-1.0f, 0.0f};
      // The far end's direction about one plug length in (the rope's last points, walked back).
      for (uint32_t k = d.count - 1; k-- > 0;) {
        const float dx = p[k].x - p[d.count - 1].x, dy = p[k].y - p[d.count - 1].y, dd = std::sqrt(dx * dx + dy * dy);
        if (dd >= reachB || k == 0) {
          if (dd > 1e-4f) back = {dx / dd, dy / dd};
          break;
        }
      }
      cablePlug(dl, p[d.count - 1], back, d.plugB, d.col, d.alpha, plugK);
      if (d.cable != UINT32_MAX) ++st.cablesPainted;
    }
  // 4. Light flowing along lit cables from source to target (additive, inside the tube).
  if (anyGlow && !far) {
    dl.setBlend(ui::BlendMode::Additive);
    const float period = std::max(48.0f * us, 16.0f * w), speed = 110.0f * us;
    const float phase = static_cast<float>(std::fmod(now * static_cast<double>(speed), static_cast<double>(period)));
    for (const CableDraw& d : cdraw) {
      const float g = d.glow * d.alpha;
      if (g <= 0.0f) continue;
      cfade.resize(d.count);
      // A pulse per period: a smooth bump a quarter period wide (no trig per vertex).
      const float inv = 1.0f / period;
      for (uint32_t k = 0; k < d.count; ++k) {
        const float x = (carc[d.first + k] - phase) * inv;
        const float u = x - std::floor(x) - 0.5f;
        const float b = std::max(0.0f, 1.0f - 16.0f * u * u);
        cfade[k] = g * (0.18f + 0.82f * b * b);
      }
      const Rgba core = ui::lerp(d.col, ui::rgba(255, 255, 255), 0.45f);
      const float hw = 0.5f * d.w;
      const float o[3] = {0.7f * hw, 0.0f, -0.7f * hw};
      const Rgba c3[3] = {ui::withAlpha(core, 0), core, ui::withAlpha(core, 0)};
      ribbon(dl, cpts.data() + d.first, cnrm.data() + d.first, d.count, o, c3, 3, cfade.data());
    }
    dl.setBlend(ui::BlendMode::Alpha);
  }
}

namespace {
Rgba cableColour(const ui::Theme& th, const WireStyle& ws) noexcept {
  const auto& C = th.circuit;
  if (ws.boolean) return C.wireBool;
  if (ws.rate == WireRate::Const) return C.wireConst;
  if (ws.notes) return C.wireNotes;
  if (ws.modulation) return C.wireMod;
  return ws.rate == WireRate::Param ? C.wireParam : C.wireAudio;
}
}  // namespace

void CircuitCanvas::Impl::paintPorts(ui::PaintContext& ctx, Point2 w0, Point2 w1) {
  if (orbits.empty() || !scene) return;
  const ui::Theme& th = *ctx.res.theme;
  ui::DrawList& dl = ctx.dl;
  const float us = ctx.scale.factor;
  const CircuitScene& sc = *scene;
  const float pad = 3.0f * (kOrbitPortR + kOrbitGap) * 3.0f;
  struct PortDraw {
    Vec2 at;
    float r;
    Rgba col;
    bool input, hot, connected;
  };
  thread_local std::vector<PortDraw> draws;
  draws.clear();
  for (const OrbitRef& o : orbits) {
    if (!showNode(o.node) || sc.nodes[o.node].hidden) continue;
    const Point2 p = orbitPos(o, true);
    if (p.x < w0.x - pad || p.x > w1.x + pad || p.y < w0.y - pad || p.y > w1.y + pad) continue;
    const float r = kOrbitPortR * nodeScale(o.node) * s();
    if (r < 0.9f) continue;  // smaller than a pixel: the block's outline says it
    PortDraw d;
    d.at = {devX(p.x), devY(p.y)};
    d.r = r;
    d.input = sc.pins[o.pin].input;
    d.connected = o.endCount > 0;
    d.col = d.connected ? cableColour(th, sc.wires[cables[orbitEnds[o.firstEnd]].wire].style) : th.face.jackRing;
    d.hot = hover.kind == CanvasHit::Kind::Pin && hover.index == o.pin;
    draws.push_back(d);
  }
  if (draws.empty()) return;
  // The outputs' soft glow (additive light around the disc).
  dl.setBlend(ui::BlendMode::Additive);
  for (const PortDraw& d : draws)
    if (!d.input) dl.addBlob(d.at, 2.6f * d.r + 2.0f * us, ui::setAlpha(d.col, d.connected ? 0.42f : 0.22f));
  dl.setBlend(ui::BlendMode::Alpha);
  for (const PortDraw& d : draws) {
    const float t = std::max(1.0f, 0.34f * d.r);
    if (d.hot) dl.addCircleStroke(d.at, d.r + t + 1.5f * us, th.circuit.hover, std::max(1.0f, std::round(1.5f * us)));
    if (d.input) {  // a hollow ring (a dark rim keeps it legible over a cable or a face)
      dl.addCircleStroke(d.at, d.r - 0.5f * t, ui::withAlpha(th.circuit.background, 200), t + 2.0f);
      dl.addCircleStroke(d.at, d.r - 0.5f * t, d.connected ? d.col : ui::mulAlpha(d.col, 0.85f), t);
    } else {  // a filled disc with a bright core
      dl.addCircleFilled(d.at, d.r + 1.0f, ui::withAlpha(th.circuit.background, 200));
      dl.addCircleFilled(d.at, d.r, d.connected ? d.col : ui::mulAlpha(d.col, 0.85f));
      dl.addCircleFilled(d.at, 0.45f * d.r, ui::lerp(d.col, ui::rgba(255, 255, 255), 0.75f));
    }
  }
}

void CircuitCanvas::Impl::paintPopups(ui::PaintContext& ctx) {
  if (!facesShown || faces.empty() || !scene) return;
  for (Hosted& h : faces) {
    if (!h.face->popupRect() || scene->nodes[h.node].hidden) continue;
    const SceneNode& n = scene->nodes[h.node];
    const Point2 sh = shiftOf(h.node);
    FaceEnv env;
    env.theme = ctx.res.theme;
    env.uiScale = ctx.scale.factor;
    env.selected = h.node < nodeSel.size() && nodeSel[h.node] != 0;
    env.jacks = h.jacks;
    ui::FacePainter p(ctx.dl, {{std::round(devX(n.x + sh.x)), std::round(devY(n.y + sh.y))}, s() * h.scale});
    p.setText(ctx.res.text);
    h.face->paintPopup(p, env);
  }
}

void CircuitCanvas::paint(ui::PaintContext& ctx) {
  Impl& m = *impl_;
  m.retired.clear();  // TMM.7: faces a rebuild replaced (no handler of theirs runs during a paint)
  const ui::Theme* theme = ctx.res.theme;
  if (!theme) return;
  const auto& C = theme->circuit;
  ui::DrawList& dl = ctx.dl;
  const ui::IRect r = rect();
  const ui::IRect clip = ui::intersect(ctx.clip, r);
  if (clip.empty()) return;
  dl.pushClip(r.toRect());
  dl.addRectFilled(clip.toRect(), C.background);
  ++m.stats.paints;
  Stats& st = m.stats;
  st.nodesPainted = st.nodesFromTiles = st.nodesStretched = st.nodesDirect = 0;
  st.wiresPainted = st.segmentsPainted = st.junctionsPainted = st.labelsPainted = 0;
  st.chunksFromTiles = st.chunksStretched = st.chunksDirect = 0;
  st.facesPainted = st.faceLayersFromTiles = st.faceLayersStretched = st.faceLayersDirect = st.facePrimitives = 0;
  st.panelsPainted = 0;
  st.cablesPainted = st.cablesGlowing = 0;
  if (!m.scene) {
    dl.popClip();
    return;
  }
  const CircuitScene& sc = *m.scene;
  const float s = m.s(), us = ctx.scale.factor;
  const Lod lod = m.lod();

  // Grid dots (Near), at least 24 device px apart.
  if (lod == Lod::Near) {
    float step = 16.0f;
    while (step * s < 24.0f * us) step *= 2.0f;
    const float dot = std::max(1.0f, std::round(us));
    const Point2 w0 = toWorld({static_cast<float>(clip.x), static_cast<float>(clip.y)});
    const Point2 w1 = toWorld({static_cast<float>(clip.right()), static_cast<float>(clip.bottom())});
    for (float wy = std::ceil(w0.y / step) * step; wy <= w1.y; wy += step) {
      const float y = std::round(m.devY(wy));
      for (float wx = std::ceil(w0.x / step) * step; wx <= w1.x; wx += step) {
        const float x = std::round(m.devX(wx));
        dl.addRectFilled({x, y, x + dot, y + dot}, C.grid);
      }
    }
  }

  // TX.block-drag: blocks being dragged are painted in a second pass over the rest (the lifted pass: the view shifted by
  // their offset), with their cables last; the first pass leaves them out.
  const bool lifting = m.anyLift();
  m.paintPass = lifting ? 0 : -1;
  paintUnderlay(ctx);

  // Visible world rect (+ the margin of captions and AA).
  const Point2 vis0 = toWorld({static_cast<float>(clip.x), static_cast<float>(clip.y)});
  const Point2 vis1 = toWorld({static_cast<float>(clip.right()), static_cast<float>(clip.bottom())});

  // TMM.6: oversample regions under everything else: one opaque wash (mixed from the background and the signal colour,
  // so overlapping pieces read as one shape) behind exactly the region's members and their wires, and "×n" in the
  // title band above its first member, at every zoom where it fits.
  const Rgba wash = ui::lerp(C.background, C.wireAudio, 0.16f);
  for (const SceneRegion& rg : sc.regions) {
    if (rg.x1 < vis0.x || rg.x0 > vis1.x || rg.y1 < vis0.y || rg.y0 > vis1.y) continue;
    const float radius = std::max(1.0f, std::round(5.0f * s));
    for (uint32_t k = rg.firstRect; k < rg.firstRect + rg.rectCount; ++k) {
      const SceneRect& q = sc.regionRects[k];
      if (q.x1 < vis0.x || q.x0 > vis1.x || q.y1 < vis0.y || q.y0 > vis1.y) continue;
      const ui::Rect rr{std::round(m.devX(q.x0)), std::round(m.devY(q.y0)), std::round(m.devX(q.x1)), std::round(m.devY(q.y1))};
      if (rr.x1 - rr.x0 >= 1.0f && rr.y1 - rr.y0 >= 1.0f) dl.addRoundedRectFilled(rr, radius, wash);
    }
    const SceneRect& band = sc.regionRects[rg.firstRect];
    if (ctx.res.text && ctx.res.uiFonts) {
      ui::TextSystem& ts = *ctx.res.text;
      const ui::FontId font = ctx.res.uiFonts->small;
      const float px = static_cast<float>(ts.metrics(font).pixelSize);
      const float h = kRegionTitle * s;
      if (h >= px) {
        const float x0 = std::round(m.devX(band.x0)), y0 = std::round(m.devY(band.y0));
        const float padX = std::round(5.0f * us);
        // The title may run past the band to the right (a phase list): up to 4× the band's width.
        const int32_t avail = static_cast<int32_t>(4.0f * (m.devX(band.x1) - x0) - 2.0f * padX);
        const ui::TextRun run = ts.shapeTruncated(font, sc.strings[rg.text], avail);
        if (run.advance > 0) {
          const ui::Rect pillR{x0, y0, x0 + static_cast<float>(run.advance) + 2.0f * padX, y0 + h};
          dl.addRoundedRectFilled(pillR, radius, wash);
          const float baseline = std::round(y0 + h * 0.5f + 0.727f * px * 0.5f);
          ui::TextSystem::drawRun(dl, run, {x0 + padX, baseline}, C.wireAudio);
        }
      }
    }
  }
  const float pad = 4.0f / s + 2.0f;
  ui::TileAtlas* atlas = ctx.res.tiles;
  bool missing = false;
  const float thin = std::max(1.0f, std::round(us));
  const float thick = std::max(2.0f, std::round(2.0f * us));
  auto baseWireColour = [&](uint32_t w) {
    if (m.wireSel[w]) return C.selected;
    if (m.hover.kind == CanvasHit::Kind::Wire && m.hover.index == w) return C.hover;
    const WireStyle& ws = sc.wires[w].style;
    if (ws.boolean) return m.wireLit[w] ? C.wireBoolLit : C.wireBool;
    if (ws.rate != WireRate::Const) {  // TMM.13-fix2 (M10): the Device view's palette (const: an off wire stays dim)
      if (ws.notes) return C.wireNotes;
      if (ws.modulation) return C.wireMod;
    }
    switch (ws.rate) {
      case WireRate::Const: return C.wireConst;
      case WireRate::Param: return C.wireParam;
      case WireRate::Audio: return C.wireAudio;
    }
    return C.wireParam;
  };
  // T4.15: an inactive part shown (Show Inactive) is drawn faint; selected or hovered it is drawn as usual.
  auto wireColour = [&](uint32_t w) {
    const Rgba c = baseWireColour(w);
    const bool emphasised = m.wireSel[w] || (m.hover.kind == CanvasHit::Kind::Wire && m.hover.index == w);
    return sc.wires[w].inactive && !emphasised ? ui::mulAlpha(c, kInactiveAlpha) : c;
  };
  auto strokeOf = [&](uint32_t v) {
    if (m.nodeSel[v]) return C.selected;
    if (m.hover.kind == CanvasHit::Kind::Node && m.hover.index == v) return C.hover;
    return sc.nodes[v].inactive ? ui::mulAlpha(C.symbolStroke, kInactiveAlpha) : C.symbolStroke;
  };
  auto dimIf = [&](uint32_t v, Rgba c) { return sc.nodes[v].inactive ? ui::mulAlpha(c, kInactiveAlpha) : c; };

  // One pass's nodes and wires (TX.block-drag: m.paintPass picks what it shows; w0 … w1 its visible world rect).
  auto layers = [&](Point2 w0, Point2 w1) {
  if (lod != Lod::Far) {
    if (m.paintPass >= 1) {  // the lifted blocks (TX.drag-grow: or a shift layer's nodes) and the wires between them
      m.visNodes.clear();
      for (const uint32_t v : m.nodesOfLayer(static_cast<uint8_t>(m.paintPass))) {
        const SceneNode& n = sc.nodes[v];
        if (!n.hidden && n.x <= w1.x + pad && n.x + n.w >= w0.x - pad && n.y - 12.0f <= w1.y + pad && n.y + n.h >= w0.y - pad) m.visNodes.push_back(v);
      }
      m.visSegs.clear();
      for (const uint32_t g : m.liftSegs)
        if (m.showWire(sc.segments[g].wire)) m.visSegs.push_back(g);
    } else {
      m.collectVisible(w0.x - pad, w0.y - pad, w1.x + pad, w1.y + pad);
      if (m.paintPass == 0) {
        std::erase_if(m.visNodes, [&](uint32_t v) { return m.lifted(v); });
        std::erase_if(m.visSegs, [&](uint32_t g) { return !m.showWire(sc.segments[g].wire); });
      }
    }
  }
  // ---- wires ----
  if (lod == Lod::Far) {
    if (m.paintPass >= 1) {  // the lifted blocks' outlines and their wires over the cached picture (or a shift layer's)
      const double ox = std::round(r.x - m.view.x * s), oy = std::round(r.y - m.view.y * s);
      for (const uint32_t g : m.liftSegs) {
        if (!m.showWire(sc.segments[g].wire)) continue;
        const Point2 a = sc.points[sc.segments[g].point], b = sc.points[sc.segments[g].point + 1];
        const float ax = static_cast<float>(ox + a.x * s), ay = static_cast<float>(oy + a.y * s);
        const float bx = static_cast<float>(ox + b.x * s), by = static_cast<float>(oy + b.y * s);
        if (a.y == b.y) dl.addHLine(std::min(ax, bx) - thin * 0.5f, std::max(ax, bx) + thin * 0.5f, ay, m.farWireCol, thin);
        else dl.addVLine(ax, std::min(ay, by) - thin * 0.5f, std::max(ay, by) + thin * 0.5f, m.farWireCol, thin);
      }
      for (const uint32_t v : m.nodesOfLayer(static_cast<uint8_t>(m.paintPass))) {
        const SceneNode& n = sc.nodes[v];
        if (m.faced(v) || n.hidden) continue;
        const ui::Rect rr{static_cast<float>(std::round(ox + n.x * s)), static_cast<float>(std::round(oy + n.y * s)),
                          static_cast<float>(std::round(ox + (n.x + n.w) * s)), static_cast<float>(std::round(oy + (n.y + n.h) * s))};
        if (rr.x1 > rr.x0 && rr.y1 > rr.y0) dl.addRectStroke(rr, m.farNodeCol, 1.0f);
      }
    } else {
      missing = m.paintFar(dl, *theme, clip, atlas, thin);
    }
  } else {
    // Whole wires (dashes and double lines follow the polyline), each once: the wires of the visible segments.
    uint32_t last = UINT32_MAX;
    for (const uint32_t sg : m.visSegs) {
      const uint32_t w = sc.segments[sg].wire;
      if (w == last) continue;
      last = w;
      const SceneWire& sw = sc.wires[w];
      if (sw.cable) continue;  // TX.cables: drawn over the blocks (paintCables)
      const WireStyle& ws = sw.style;
      const float t = (!ws.boolean && ws.rate == WireRate::Audio) ? thick : thin;
      const Rgba col = wireColour(w);
      m.pts.clear();
      for (uint32_t i = 0; i < sw.pointCount; ++i) {
        const Point2 q = sc.points[sw.firstPoint + i];
        m.pts.push_back({snapCentre(m.devX(q.x), ws.poly ? thin : t), snapCentre(m.devY(q.y), ws.poly ? thin : t)});
      }
      // Snapping keeps each segment axis-aligned (the same coordinate snaps the same way).
      auto strokePath = [&](const std::vector<Vec2>& path, float width) {
        float phase = 0.0f;
        for (size_t i = 0; i + 1 < path.size(); ++i) {
          if (ws.modulation) dashedSegment(dl, path[i], path[i + 1], col, width, 6.0f * us, 4.0f * us, phase);
          else crispSegment(dl, path[i], path[i + 1], col, width);
          ++st.segmentsPainted;
        }
      };
      if (ws.poly) {
        const float gap = (!ws.boolean && ws.rate == WireRate::Audio) ? 2.0f * thin : thin;
        const float d = std::round((gap + thin) * 0.5f);
        offsetPolyline(m.pts, d, m.offA);
        offsetPolyline(m.pts, -d, m.offB);
        strokePath(m.offA, thin);
        strokePath(m.offB, thin);
      } else {
        strokePath(m.pts, t);
      }
      ++st.wiresPainted;
    }
    // Junction dots.
    const float dotR = std::max(2.0f, 2.5f * us);
    for (size_t i = 0; i < sc.junctions.size(); ++i) {
      const Point2 j = sc.junctions[i];
      if (j.x < w0.x - pad || j.x > w1.x + pad || j.y < w0.y - pad || j.y > w1.y + pad) continue;
      const uint32_t w = sc.junctionWire[i];
      if (!m.showWire(w)) continue;  // TX.block-drag
      const WireStyle& ws = sc.wires[w].style;
      const float t = ws.poly ? thin : ((!ws.boolean && ws.rate == WireRate::Audio) ? thick : thin);
      dl.addCircleFilled({snapCentre(m.devX(j.x), t), snapCentre(m.devY(j.y), t)}, ws.poly ? dotR + thin : dotR, wireColour(w));
      ++st.junctionsPainted;
    }
  }

  // ---- nodes ----
  if (lod == Lod::Far) {
    // The selection and the hovered node over the cached picture (the chunks hold the unselected look).
    const double ox = std::round(r.x - m.view.x * s), oy = std::round(r.y - m.view.y * s);
    auto outline = [&](uint32_t v, Rgba col) {
      if (m.faced(v)) return;  // TMM.4: a face shows its own selection / hover
      const SceneNode& n = sc.nodes[v];
      const ui::Rect rr{static_cast<float>(std::round(ox + n.x * s)), static_cast<float>(std::round(oy + n.y * s)),
                        static_cast<float>(std::round(ox + (n.x + n.w) * s)), static_cast<float>(std::round(oy + (n.y + n.h) * s))};
      if (rr.x1 > rr.x0 && rr.y1 > rr.y0) dl.addRectStroke(rr, col, 1.0f);
    };
    for (const uint32_t v : m.selection.nodes)
      if (v < sc.nodes.size() && !sc.nodes[v].hidden && m.showNode(v)) outline(v, C.selected);
    if (m.hover.kind == CanvasHit::Kind::Node && m.showNode(m.hover.index)) outline(m.hover.index, C.hover);
    for (const uint32_t w : m.selection.wires)
      if (w < sc.wires.size() && m.showWire(w) && !(m.paintPass >= 1 && sc.wires[w].cable)) m.farWire(dl, w, ox, oy, s, thin, C.selected);
    if (m.hover.kind == CanvasHit::Kind::Wire && m.hover.index < sc.wires.size() && m.showWire(m.hover.index) &&
        !(m.paintPass >= 1 && sc.wires[m.hover.index].cable))
      m.farWire(dl, m.hover.index, ox, oy, s, thin, C.hover);
  } else {
    const float stubT = std::max(1.0f, std::round(strokeWidth(s)));
    // Pin stubs (crisp, snapped like the wires); T5.x-circuit-chain: only from Near when the view asks.
    for (const uint32_t v : m.visNodes) {
      if (m.stubsNearOnly && lod != Lod::Near) break;
      if (m.faced(v)) continue;  // TMM.4: the face draws its jacks
      const SceneNode& n = sc.nodes[v];
      const Rgba col = strokeOf(v);
      for (uint32_t k = 0; k < n.pinCount; ++k) {
        const ScenePin& pin = sc.pins[n.firstPin + k];
        if (!pin.visible || pin.stub <= 0.0f) continue;
        const float y = snapCentre(m.devY(pin.y), stubT);
        const float x0 = m.devX(pin.x), x1 = m.devX(pin.input ? pin.x + pin.stub : pin.x - pin.stub);
        dl.addHLine(std::min(x0, x1), std::max(x0, x1), y, col, stubT);
      }
    }
    // T4.16c: the hovered pin (a press there starts a wire in the editor) is ringed in the hover colour.
    if (m.hover.kind == CanvasHit::Kind::Pin && m.hover.index < sc.pins.size()) {
      const ScenePin& pin = sc.pins[m.hover.index];
      if (pin.visible && !pin.orbit && m.showNode(pin.node)) {  // TX.orbit-ports: a port shows its own hover (paintPorts)
        const float rr = std::max(3.0f, std::round(3.0f * us));
        dl.addCircleStroke({m.devX(pin.x), m.devY(pin.y)}, rr, C.hover, std::max(1.0f, std::round(1.5f * us)));
      }
    }
    // Bodies: from tiles (two quads), else directly. Tile quads are grouped per page and layer to keep draw commands
    // few: bodies never overlap, so the order between them does not matter.
    struct Quad {
      uint32_t page;
      uint8_t layer;
      ui::Rect r;
      Vec2 uv0, uv1;
      Rgba col;
    };
    thread_local std::vector<Quad> quads;
    quads.clear();
    for (const uint32_t v : m.visNodes) {
      if (m.faced(v)) continue;  // TMM.4: drawn as its face (below)
      const SceneNode& n = sc.nodes[v];
      const SymbolSpec& spec = sc.specs[n.spec];
      const Vec2 o{std::round(m.devX(n.x)), std::round(m.devY(n.y))};
      const Rgba fillCol = dimIf(v, fillColour(*theme, spec.symbol)), strokeCol = strokeOf(v);
      bool drawn = false;
      if (atlas) {
        auto it = m.tiles.find(spec.bodyKey);
        if (it != m.tiles.end() && !it->second.failed && atlas->valid(it->second.fill) && atlas->valid(it->second.stroke)) {
          const BodyTile& t = it->second;
          const float k = s / t.scale;  // 1 when current; stretched while zooming
          const float inv = atlas->uvScale();
          for (int layer = 0; layer < 2; ++layer) {
            const ui::TileAtlas::Tile& tile = layer == 0 ? t.fill : t.stroke;
            atlas->touch(tile.page);
            const float x = o.x - t.margin * k, y = o.y - t.margin * k;
            quads.push_back({tile.page, static_cast<uint8_t>(layer),
                             {x, y, x + static_cast<float>(tile.w) * k, y + static_cast<float>(tile.h) * k},
                             {static_cast<float>(tile.x) * inv, static_cast<float>(tile.y) * inv},
                             {static_cast<float>(tile.x + tile.w) * inv, static_cast<float>(tile.y + tile.h) * inv},
                             layer == 0 ? fillCol : strokeCol});
          }
          drawn = true;
          if (t.scale == s) ++st.nodesFromTiles;
          else {
            ++st.nodesStretched;
            missing = true;
          }
        } else if (it == m.tiles.end() || !(it->second.failed && it->second.scale == s)) {
          missing = true;
        }
      }
      if (!drawn) {
        paintBody(dl, spec, o, s, Layer::Fill, fillCol);
        paintBody(dl, spec, o, s, Layer::Stroke, strokeCol);
        ++st.nodesDirect;
      }
      ++st.nodesPainted;
    }
    std::stable_sort(quads.begin(), quads.end(), [](const Quad& a, const Quad& b) {
      return a.page != b.page ? a.page < b.page : a.layer < b.layer;
    });
    for (const Quad& q : quads) dl.addImage(ui::TileAtlas::texture(q.page), q.r, q.uv0, q.uv1, q.col);
    // Subcircuit title bands, over the bodies (a third colour the two tile layers do not carry).
    for (const uint32_t v : m.visNodes) {
      const SceneNode& n = sc.nodes[v];
      if (sc.specs[n.spec].symbol != Symbol::Subcircuit || m.faced(v)) continue;
      const Vec2 o{std::round(m.devX(n.x)), std::round(m.devY(n.y))};
      const float bw = std::max(1.0f, std::round(strokeWidth(s)));
      const float x0 = o.x + std::round(8.0f * s) + bw, x1 = o.x + std::round((n.w - 8.0f) * s) - bw;
      const float y0 = o.y + bw, y1 = o.y + std::round(16.0f * s) - std::ceil(bw * 0.5f);
      if (x1 > x0 && y1 > y0) dl.addRectFilled({x0, y0, x1, y1}, dimIf(v, C.subcircuitTitle));
    }
  }

  // ---- TMM.4: faces, at every level of detail ----
  m.paintFaces(ctx, clip, atlas, missing);

  // ---- TX.mod-all: wires that end on a face's control, over the face ----
  // The scene wire stops on the block's edge (its last point); from there the wire goes on, straight into the face and
  // across to the control as the face places it now (a handle moves with its value, a panel's control with its layout),
  // and stops at the control's rim with a plug dot. A control not shown now (another band selected): a socket at the
  // edge. Drawn in the wire's own colour and style (hover and selection follow the wire), at every level of detail.
  if (m.facesShown && !m.faces.empty() && !m.controlWires.empty()) {
    const bool far = lod == Lod::Far;  // the Far picture's wires: thin, in its one colour (selection / hover over it)
    const float t = far ? thin : thick;
    for (const uint32_t w : m.controlWires) {
      if (w >= sc.wires.size()) continue;
      const SceneWire& sw = sc.wires[w];
      if (sw.control == UINT32_MAX || sw.cable || sw.hidden || sw.pointCount < 2 || sw.toPin >= sc.pins.size() || !m.showWire(w)) continue;
      const uint32_t v = sc.pins[sw.toPin].node;
      if (!m.faced(v)) continue;
      const SceneNode& n = sc.nodes[v];
      if (n.hidden || n.x > w1.x + pad || n.x + n.w < w0.x - pad || n.y > w1.y + pad || n.y + n.h < w0.y - pad) continue;
      const auto& hosted = m.faces[static_cast<size_t>(m.faceOf[v])];
      const Face& f = *hosted.face;
      const float fk = hosted.scale;  // TX.resize: the face's units on a scaled node
      const Point2 e = sc.points[sw.firstPoint + sw.pointCount - 1];
      const bool emphasised = m.wireSel[w] || (m.hover.kind == CanvasHit::Kind::Wire && m.hover.index == w);
      const Rgba col = far && !emphasised ? m.farWireCol : wireColour(w);
      const float plugR = std::max(1.0f, std::round(2.5f * us * std::min(1.0f, 1.25f * s)));  // small far out
      const std::optional<ControlAnchor> a = f.controlAnchor(sc.strings[sw.control]);
      if (!a) {
        dl.addCircleFilled({snapCentre(m.devX(e.x), t), snapCentre(m.devY(e.y), t)}, plugR + t, col);
        continue;
      }
      // E → (along the entry direction to the control's row / column) → the control, the last leg cut at its rim.
      const Point2 c{n.x + a->at.x * fk, n.y + a->at.y * fk};
      const bool vertical = std::fabs(e.y - n.y) < 0.5f || std::fabs(e.y - (n.y + n.h)) < 0.5f;  // a top / bottom entry
      Point2 path[3] = {e, vertical ? Point2{e.x, c.y} : Point2{c.x, e.y}, c};
      uint32_t count = 3;
      if (path[1].x == path[2].x && path[1].y == path[2].y) count = 2;
      // Stop `radius` short of the control along the path (a last leg shorter than that — the control moved a little
      // since the layout — goes, and the one before it is cut).
      for (float rest = a->radius * fk; rest > 0.0f && count >= 2;) {
        const float dx = path[count - 1].x - path[count - 2].x, dy = path[count - 1].y - path[count - 2].y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > rest) {
          path[count - 1] = {path[count - 1].x - dx / len * rest, path[count - 1].y - dy / len * rest};
          break;
        }
        rest -= len;
        if (count == 2) {
          path[1] = path[0];
          break;
        }
        --count;
      }
      float phase = 0.0f;
      for (uint32_t i = 0; i + 1 < count; ++i) {
        const Vec2 p0{snapCentre(m.devX(path[i].x), t), snapCentre(m.devY(path[i].y), t)};
        const Vec2 p1{snapCentre(m.devX(path[i + 1].x), t), snapCentre(m.devY(path[i + 1].y), t)};
        if (sw.style.modulation && !far) dashedSegment(dl, p0, p1, col, t, 6.0f * us, 4.0f * us, phase);
        else crispSegment(dl, p0, p1, col, t);
      }
      const Point2 end = path[count - 1];
      dl.addCircleFilled({snapCentre(m.devX(end.x), t), snapCentre(m.devY(end.y), t)}, plugR, col);
    }
  }

  // ---- labels ----
  if (lod == Lod::Near && ctx.res.text && ctx.res.uiFonts) {
    ui::TextSystem& ts = *ctx.res.text;
    const ui::FontId font = ctx.res.uiFonts->small;
    const uint32_t px = ts.metrics(font).pixelSize;
    const float cap = std::round(0.727f * static_cast<float>(px));
    const bool pins = m.view.zoom >= kPinLabelZoom;
    for (const SceneLabel& lb : sc.labels) {
      if (lb.role == SceneLabel::Role::Pin && !pins) continue;
      if (lb.node != UINT32_MAX ? !m.showNode(lb.node) : m.paintPass >= 1) continue;  // TX.block-drag
      if (lb.node != UINT32_MAX && lb.role != SceneLabel::Role::Caption && m.faced(lb.node)) continue;  // TMM.4: label-free
      if (lb.node != UINT32_MAX && lb.node < m.nodeStamp.size()) {
        const SceneNode& n = sc.nodes[lb.node];
        if (n.x > w1.x + pad || n.x + n.w < w0.x - pad || n.y - 12 > w1.y + pad || n.y + n.h < w0.y - pad) continue;
      } else if (lb.y < w0.y - 16 || lb.y > w1.y + 16 || lb.x0 > w1.x) {
        continue;
      }
      const std::string& text = sc.strings[lb.text];
      const float x0 = m.devX(lb.x0), x1 = m.devX(lb.x1);
      const int32_t avail = static_cast<int32_t>(x1 - x0);
      if (avail <= 0) continue;
      const ui::TextRun run = ts.shape(font, text);
      if (run.advance > avail && lb.role != SceneLabel::Role::Comment) continue;  // does not fit: hidden, not cut
      float x = x0;
      if (lb.align == SceneLabel::Align::Center) x = x0 + static_cast<float>(avail - run.advance) * 0.5f;
      else if (lb.align == SceneLabel::Align::Right) x = x1 - static_cast<float>(run.advance);
      const float baseline = std::round(m.devY(lb.y) + cap * 0.5f);
      Rgba col = C.symbolText;
      if (lb.role == SceneLabel::Role::Caption || lb.role == SceneLabel::Role::Pin) col = C.pinText;
      // TMM.13-fix2 (MM review contrast table: 4.35:1): a face's name is its only title — lifted halfway to the symbols' text.
      if (lb.role == SceneLabel::Role::Caption && lb.node != UINT32_MAX && m.faced(lb.node)) col = ui::lerp(C.pinText, C.symbolText, 0.5f);
      if (lb.role == SceneLabel::Role::Comment) col = C.comment;
      if (lb.node != UINT32_MAX && lb.node < sc.nodes.size()) col = dimIf(lb.node, col);
      const ui::ClipRect tc{std::max(clip.x, static_cast<int32_t>(std::floor(x0))), clip.y,
                            std::min(clip.right(), static_cast<int32_t>(std::ceil(x1)) + (lb.role == SceneLabel::Role::Comment ? 4096 : 0)),
                            clip.bottom()};
      ui::TextSystem::drawRunClipped(dl, run, {std::round(x), baseline}, col, tc);
      ++st.labelsPainted;
    }
  }

  // M4 review M15: below Near the terminals (inputs, params, sources, outputs, exports) keep their names, in small
  // pills over their symbols; a pill that would overlap one already placed is left out.
  // TMM.13-fix2 (MM review "UI and aesthetic" 10): not while the faces are drawn smaller than three pill rows — the
  // names ("note", "y") then outweighed the faces they name; the faces' own sockets and the wires say it there.
  bool facesTiny = false;
  if (m.facesShown && !m.faces.empty()) {
    float tallest = 0.0f;
    for (const auto& h : m.faces)
      if (h.face && !h.panel) tallest = std::max(tallest, h.face->worldSize().y * h.scale * s);
    facesTiny = tallest > 0.0f && tallest < 3.0f * ctx.scale.exact(13.0f);
  }
  if (lod != Lod::Near && !facesTiny && m.view.zoom >= m.pillMinZoom && ctx.res.text && ctx.res.uiFonts) {
    ui::TextSystem& ts = *ctx.res.text;
    const ui::FontId font = ctx.res.uiFonts->small;
    const float pillPad = std::round(ctx.scale.exact(3.0f)), lh = std::round(ctx.scale.exact(13.0f));
    std::vector<ui::Rect> placed;
    for (const SceneLabel& lb : sc.labels) {
      if (lb.role != SceneLabel::Role::Title || lb.node >= sc.nodes.size() || !m.showNode(lb.node)) continue;
      const SceneNode& n = sc.nodes[lb.node];
      const Symbol sym = sc.specs[n.spec].symbol;
      if (sym != Symbol::Input && sym != Symbol::Param && sym != Symbol::Source && sym != Symbol::Output && sym != Symbol::Export)
        continue;
      const float cx = m.devX(n.x + n.w * 0.5f), cy = m.devY(n.y + n.h * 0.5f);
      if (cx < static_cast<float>(clip.x) - 200.0f || cx > static_cast<float>(clip.right()) + 200.0f || cy < static_cast<float>(clip.y) - lh ||
          cy > static_cast<float>(clip.bottom()) + lh)
        continue;
      const ui::TextRun run = ts.shape(font, sc.strings[lb.text]);
      const float w = static_cast<float>(run.advance) + 2.0f * pillPad;
      // Inputs read leftwards from their symbol, outputs rightwards (the pill's inner edge at the symbol's centre).
      const bool left = sym == Symbol::Input || sym == Symbol::Param || sym == Symbol::Source;
      const float x0 = std::round(left ? cx - w : cx), y0 = std::round(cy - lh * 0.5f);
      const ui::Rect box{x0, y0, x0 + w, y0 + lh};
      bool free = true;
      for (const ui::Rect& q : placed) free = free && (box.x1 <= q.x0 || q.x1 <= box.x0 || box.y1 <= q.y0 || q.y1 <= box.y0);
      if (!free) continue;
      placed.push_back(box);
      dl.addRoundedRectFilled(box, ctx.scale.exact(2.0f), ui::withAlpha(theme->surface.sunken, 225));
      ui::TextSystem::drawRun(dl, run, {x0 + pillPad, std::round(y0 + lh - std::round(ctx.scale.exact(3.5f)))}, dimIf(lb.node, C.symbolText));
      ++st.labelsPainted;
    }
  }

  };  // layers

  layers(vis0, vis1);
  // ---- TX.cables: over everything of the scene (near / mid; far: in the chunks) ----
  m.paintCables(ctx, vis0, vis1, lifting ? 0 : -1);
  m.paintPorts(ctx, vis0, vis1);  // TX.orbit-ports: over the cables' plugs
  paintNodeOverlay(ctx);

  // ---- TX.block-drag: the lifted pass (TX.drag-grow: after a pass per shift layer) ----
  if (lifting) {
    thread_local std::vector<uint32_t> baseVis;
    baseVis = m.visNodes;
    const View unshifted = m.view;
    auto shift = [&](Point2 o) {
      m.view = unshifted;
      m.view.x -= static_cast<double>(o.x), m.view.y -= static_cast<double>(o.y);
    };
    // One moved layer's parts: the host's underlay, its nodes and wires, the host's overlay of them.
    auto underlayPass = [&](uint8_t k) {
      m.paintPass = static_cast<int8_t>(k);
      shift(m.layerOffset(k));
      paintUnderlay(ctx);
      m.view = unshifted;
    };
    auto layerPass = [&](uint8_t k) {
      m.paintPass = static_cast<int8_t>(k);
      shift(m.layerOffset(k));
      layers(toWorld({static_cast<float>(clip.x), static_cast<float>(clip.y)}),
             toWorld({static_cast<float>(clip.right()), static_cast<float>(clip.bottom())}));
      paintNodeOverlay(ctx);
      m.view = unshifted;
      for (const uint32_t v : m.visNodes) baseVis.push_back(v);  // what onFrame builds tiles for: every pass's nodes
    };
    for (uint8_t k = 0; k < m.shiftOffsets.size(); ++k) {
      underlayPass(static_cast<uint8_t>(kShiftLayer0 + k));
      layerPass(static_cast<uint8_t>(kShiftLayer0 + k));
    }
    if (!m.dragNodes.empty()) underlayPass(1);
    // The traces with ends in different layers: a Z from end to end (their route is laid out again by the drop).
    for (const uint32_t w : m.liftHalf) {
      const SceneWire& sw = sc.wires[w];
      Point2 a = sc.points[sw.firstPoint], b = sc.points[sw.firstPoint + sw.pointCount - 1];
      const Point2 oa = m.shiftOf(wireEnd(sc, sw, false)), ob = m.shiftOf(wireEnd(sc, sw, true));
      a = {a.x + oa.x, a.y + oa.y};
      b = {b.x + ob.x, b.y + ob.y};
      const WireStyle& ws = sw.style;
      const float t = lod == Lod::Far || ws.boolean || ws.rate != WireRate::Audio ? thin : thick;
      const Rgba col = lod == Lod::Far ? m.farWireCol : wireColour(w);
      const Vec2 da{snapCentre(m.devX(a.x), t), snapCentre(m.devY(a.y), t)}, db{snapCentre(m.devX(b.x), t), snapCentre(m.devY(b.y), t)};
      const float mx = snapCentre((da.x + db.x) * 0.5f, t);
      crispSegment(dl, da, {mx, da.y}, col, t);
      crispSegment(dl, {mx, da.y}, {mx, db.y}, col, t);
      crispSegment(dl, {mx, db.y}, db, col, t);
    }
    if (!m.dragNodes.empty()) layerPass(1);
    m.paintCables(ctx, vis0, vis1, 1);  // their cables over everything (they plug into the moved blocks)
    for (uint8_t k = 0; k < m.shiftOffsets.size(); ++k) {
      m.paintPass = static_cast<int8_t>(kShiftLayer0 + k);
      m.paintPorts(ctx, vis0, vis1);  // TX.orbit-ports: the moved blocks' ports (at their offset)
    }
    m.paintPass = 1;
    m.paintPorts(ctx, vis0, vis1);
    m.visNodes = baseVis;
    m.paintPass = -1;
  }

  // ---- rubber band ----
  if (m.mode == Impl::DragMode::Band && m.tracker.moved) {
    const ui::IRect band = drag::rubberBand(m.tracker.press, m.tracker.last);
    dl.addRectFilled(band.toRect(), C.rubberBand);
    dl.addRectStroke(band.toRect(), C.rubberBandEdge, 1.0f);
  }
  paintOverlay(ctx);
  m.paintPopups(ctx);  // TX.orbit-ports: faces' floating panels over everything
  dl.popClip();
  if (missing && !m.zooming) {
    if (atlas) m.tilesWanted = true;
    requestFrame();  // TX.panel-faces: also a panel's layout at this zoom (onFrame, without an atlas too)
  }
}

void CircuitCanvas::onFrame(double now) {
  Impl& m = *impl_;
  m.stepOrbits(now);  // TX.orbit-ports: the ports' slides (frames only while one slides); before the cables' step
  m.stepCables(now);  // TX.cables: the swing (frames only while a cable is awake)
  m.stepLoose(now);   // TX.node-menu-fix1: the cable being drawn out
  if (m.pumpWanted) m.pumpFaces();  // TX.panel-faces: a face's own work (a panel's widgets)
  if (m.scene && !m.zooming && m.prepareFaces()) markDirty();  // TX.panel-faces: a panel laid out at the settled zoom
  if (!m.scene || m.zooming || !m.tilesWanted || !tree() || !tree()->resources() || !tree()->resources()->tiles) return;
  m.tilesWanted = false;
  ui::TileAtlas& atlas = *tree()->resources()->tiles;
  bool built = m.lod() == Lod::Far ? m.buildFarChunks(atlas) : m.buildVisibleTiles(atlas);
  built = m.buildFaceTiles(atlas) || built;  // TMM.4: a few tiles per face type (all at once: they are small)
  if (built) markDirty();
  if (m.tilesWanted) requestFrame();
}

void CircuitCanvas::onTimer(uint32_t id) {
  Impl& m = *impl_;
  if (id == m.faceTimer && id != 0) {  // TMM.4: the faces' <= 60 Hz poll
    m.faceTimer = 0;
    m.pollFaces();
    return;
  }
  if (id == m.glowTimer && id != 0) {  // TX.cables: the glow's ≤ 30 Hz poll
    m.glowTimer = 0;
    m.pollCables();
    return;
  }
  if (id == m.pumpTimer && id != 0) {  // TX.panel-faces: a face's own timer is due
    m.pumpTimer = 0;
    m.pumpDue = std::numeric_limits<double>::infinity();
    m.pumpFaces();
    return;
  }
  if (id != m.settleTimer) return;
  m.settleTimer = 0;
  m.zooming = false;
  markDirty();  // the settled frame: stale tiles are wanted again (paint asks for the build)
}

// ---- input ----------------------------------------------------------------------------------------------------------

bool CircuitCanvas::onPointer(const ui::PointerEvent& e) {
  Impl& m = *impl_;
  const float us = m.uiScale();
  if (facePointer(e)) return true;  // TMM.4
  switch (e.type) {
    case ui::PointerType::Wheel: {
      if (e.mods & ui::mod::kCtrl) {
        zoomAt(e.pos, std::pow(1.15, static_cast<double>(e.wheel.y)));
      } else {
        const float step = 48.0f * us;
        if (e.mods & ui::mod::kShift) panBy(e.wheel.y * step, 0.0f);
        else panBy(-e.wheel.x * step, e.wheel.y * step);
      }
      return true;
    }
    case ui::PointerType::Down: {
      if (tree()) tree()->setFocus(this);
      m.tracker.begin(e);
      if (e.button == ui::PointerButton::Middle || (e.button == ui::PointerButton::Left && (e.mods & ui::mod::kAlt))) {
        m.mode = Impl::DragMode::Pan;
        m.panLast = e.pos;
        return true;
      }
      if (e.button != ui::PointerButton::Left) return false;
      const CanvasHit hit = hitTest(e.pos);
      if (e.clickCount >= 2 && hit.kind == CanvasHit::Kind::Node && m.scene && onOpen) {
        const Symbol sym = m.scene->specs[m.scene->nodes[hit.index].spec].symbol;
        if (sym == Symbol::Subcircuit || sym == Symbol::Filter) {
          m.mode = Impl::DragMode::None;
          onOpen(hit.index);
          return true;
        }
      }
      if (hit.kind == CanvasHit::Kind::None) {
        m.mode = Impl::DragMode::Band;
        m.bandAdd = (e.mods & (ui::mod::kShift | ui::mod::kCtrl)) != 0;
        m.bandBase = m.selection;
        if (!m.bandAdd) clearSelection();
        return true;
      }
      m.mode = Impl::DragMode::Click;
      uint32_t node = UINT32_MAX, wire = UINT32_MAX;
      if (hit.kind == CanvasHit::Kind::Node) node = hit.index;
      else if (hit.kind == CanvasHit::Kind::Pin) node = m.scene->pins[hit.index].node;
      else wire = hit.index;
      const bool toggle = (e.mods & ui::mod::kCtrl) != 0, add = (e.mods & ui::mod::kShift) != 0;
      if (toggle) {
        Selection s = m.selection;
        auto flip = [](std::vector<uint32_t>& v, uint32_t x) {
          auto it = std::lower_bound(v.begin(), v.end(), x);
          if (it != v.end() && *it == x) v.erase(it);
          else v.insert(it, x);
        };
        if (node != UINT32_MAX) flip(s.nodes, node);
        if (wire != UINT32_MAX) flip(s.wires, wire);
        m.selection = std::move(s);
        m.selectionChanged();
      } else {
        const uint32_t nodes[1] = {node}, wires[1] = {wire};
        select(node != UINT32_MAX ? std::span<const uint32_t>(nodes) : std::span<const uint32_t>(),
               wire != UINT32_MAX ? std::span<const uint32_t>(wires) : std::span<const uint32_t>(), add);
      }
      return true;
    }
    case ui::PointerType::Move: {
      if (m.mode == Impl::DragMode::Pan) {
        panBy(e.pos.x - m.panLast.x, e.pos.y - m.panLast.y);
        m.panLast = e.pos;
        return true;
      }
      if (m.mode == Impl::DragMode::Band) {
        const ui::IRect before = drag::rubberBand(m.tracker.press, m.tracker.last);
        m.tracker.move(e.pos, us);
        if (!m.tracker.moved) return true;
        // Select what the band touches (nodes) or contains (wires), live.
        const ui::IRect band = drag::rubberBand(m.tracker.press, m.tracker.last);
        const Point2 a = toWorld({static_cast<float>(band.x), static_cast<float>(band.y)});
        const Point2 b = toWorld({static_cast<float>(band.right()), static_cast<float>(band.bottom())});
        Selection s = m.bandAdd ? m.bandBase : Selection{};
        if (m.scene) {
          for (uint32_t v = 0; v < m.scene->nodes.size(); ++v) {
            const SceneNode& n = m.scene->nodes[v];
            if (!n.hidden && n.x <= b.x && n.x + n.w >= a.x && n.y <= b.y && n.y + n.h >= a.y) s.nodes.push_back(v);
          }
          for (uint32_t w = 0; w < m.scene->wires.size(); ++w) {
            const SceneWire& sw = m.scene->wires[w];
            if (!sw.hidden && sw.x0 >= a.x && sw.x1 <= b.x && sw.y0 >= a.y && sw.y1 <= b.y) s.wires.push_back(w);
          }
        }
        for (auto* v : {&s.nodes, &s.wires}) {
          std::sort(v->begin(), v->end());
          v->erase(std::unique(v->begin(), v->end()), v->end());
        }
        if (s.nodes != m.selection.nodes || s.wires != m.selection.wires) {
          m.selection = std::move(s);
          m.selectionChanged();
        } else {
          markDirty();
        }
        (void)before;
        return true;
      }
      if (m.mode == Impl::DragMode::Click) {
        m.tracker.move(e.pos, us);
        return true;
      }
      const CanvasHit hit = hitTest(e.pos);
      if (!(hit == m.hover)) {
        m.hover = hit;
        markDirty();
        if (onHoverChanged) onHoverChanged(hit);
        infoChanged();  // the info view names the node, pin or wire now under the pointer
      }
      return false;
    }
    case ui::PointerType::Up: {
      const bool band = m.mode == Impl::DragMode::Band;
      m.mode = Impl::DragMode::None;
      m.tracker.end();
      if (band) markDirty();  // the rubber band goes
      return true;
    }
    case ui::PointerType::Leave:
      if (m.hover.kind != CanvasHit::Kind::None) {
        m.hover = {};
        markDirty();
        if (onHoverChanged) onHoverChanged(m.hover);
      }
      return false;
  }
  return false;
}

bool CircuitCanvas::faceKey(const ui::KeyEvent& e) {
  Impl& m = *impl_;
  if (!m.facesShown || m.faceFocus < 0 || static_cast<size_t>(m.faceFocus) >= m.faces.size() || faceDragging()) return false;
  const bool used = m.faces[static_cast<size_t>(m.faceFocus)].face->onKey(e);
  m.syncTextInput();  // TX.panel-faces: a digit may have opened a panel control's type-in
  if (!used) return false;
  markDirty();
  return true;
}

bool CircuitCanvas::onKey(const ui::KeyEvent& e) {
  if (faceKey(e)) return true;  // TMM.7: the focused face first (a keyframe's Delete)
  if (!e.down) return false;
  const ui::Mods mods = e.mods & ui::mod::kChordMask;
  if (e.key == ui::key::kEscape && faceDragging()) {  // TMM.4: Esc undoes a face's drag
    onInteractionCancelled();
    return true;
  }
  const Vec2 centre{static_cast<float>(rect().x) + static_cast<float>(rect().w) * 0.5f,
                    static_cast<float>(rect().y) + static_cast<float>(rect().h) * 0.5f};
  if (mods == ui::mod::kNone && e.key == 'f') {
    fitAll();
    return true;
  }
  if (mods == ui::mod::kNone && (e.key == '=' || e.key == '+')) {
    zoomAt(centre, 1.25);
    return true;
  }
  if (mods == ui::mod::kNone && e.key == '-') {
    zoomAt(centre, 0.8);
    return true;
  }
  if (mods == ui::mod::kNone && e.key == ui::key::kEscape && (!impl_->selection.nodes.empty() || !impl_->selection.wires.empty())) {
    clearSelection();
    return true;
  }
  return false;
}

void CircuitCanvas::onCaptureLost() {
  Impl& m = *impl_;
  clearNodeDrag();  // TX.block-drag
  if (m.faceCapture >= 0 && static_cast<size_t>(m.faceCapture) < m.faces.size()) {  // TMM.4: the drag is undone
    m.faces[static_cast<size_t>(m.faceCapture)].face->cancelDrag();
    m.faceCapture = -1;
    markDirty();
  }
  if (m.mode == Impl::DragMode::Band) markDirty();
  m.mode = Impl::DragMode::None;
  m.tracker.end();
}

void CircuitCanvas::onInteractionCancelled() {
  Impl& m = *impl_;
  clearNodeDrag();  // TX.block-drag: a host's drag cancelled: the lifted nodes back in place
  if (m.faceCapture >= 0 && static_cast<size_t>(m.faceCapture) < m.faces.size()) {  // TMM.4
    m.faces[static_cast<size_t>(m.faceCapture)].face->cancelDrag();
    m.faceCapture = -1;
    markDirty();
  }
  if (m.mode == Impl::DragMode::Band) {  // the selection from before the band comes back
    m.selection = m.bandBase;
    m.selectionChanged();
  }
  m.mode = Impl::DragMode::None;
  m.tracker.end();
}

const CanvasHit& CircuitCanvas::hovered() const noexcept { return impl_->hover; }
bool CircuitCanvas::panning() const noexcept { return impl_->mode == Impl::DragMode::Pan; }

bool CircuitCanvas::altHeld() const noexcept { return tree() && (tree()->mods() & ui::mod::kAlt) != 0; }

ui::Cursor CircuitCanvas::cursorAt(ui::Vec2 pos) const {
  if (panning() || altHeld()) return ui::Cursor::Move;
  return faceCursor(pos);  // TMM.4 (Inherit elsewhere)
}

ui::InfoText CircuitCanvas::info() const {
  const Impl& m = *impl_;
  if (ui::InfoText t; faceInfo(t)) return t;  // TMM.4
  if (!m.scene || m.hover.kind == CanvasHit::Kind::None) return {"Circuit", "Ctrl+wheel zooms, the wheel scrolls, F fits the whole patch."};
  const CircuitScene& sc = *m.scene;
  m.infoBody.clear();
  if (m.hover.kind == CanvasHit::Kind::Wire) {
    const WireStyle& ws = sc.wires[m.hover.index].style;
    m.infoTitle = "Wire";
    m.infoBody = ws.boolean ? "bool" : "real";
    m.infoBody += ws.rate == WireRate::Const ? ", const" : (ws.rate == WireRate::Param ? ", param rate" : ", audio rate");
    if (ws.poly) m.infoBody += ", per voice";
    if (ws.modulation) m.infoBody += ", modulating a parameter";
    return {m.infoTitle, m.infoBody};
  }
  const uint32_t v = m.hover.kind == CanvasHit::Kind::Pin ? sc.pins[m.hover.index].node : m.hover.index;
  const SymbolSpec& spec = sc.specs[sc.nodes[v].spec];
  // Title: what the node is ("add", "parameter cutoff", "mix = lerp"); body: what it does, then the hovered pin.
  const bool atomLike = spec.symbol != Symbol::Subcircuit && spec.symbol != Symbol::Filter && spec.symbol != Symbol::Const &&
                        spec.symbol != Symbol::Param && spec.symbol != Symbol::Input && spec.symbol != Symbol::Output &&
                        spec.symbol != Symbol::Export && spec.symbol != Symbol::Unknown;
  const std::string what = atomLike && !spec.kind.empty() ? std::string(spec.kind)
                           : spec.title.empty()            ? std::string(spec.kind.empty() ? "node" : spec.kind)
                           : spec.kind.empty() || spec.kind == "subcircuit" || spec.kind == "constant"
                               ? spec.title
                               : std::string(spec.kind) + " " + spec.title;
  m.infoTitle = spec.caption.empty() ? what : spec.caption + " = " + what;
  if (spec.symbol == Symbol::Subcircuit || spec.symbol == Symbol::Filter)
    m.infoBody = "Subcircuit " + spec.title + ". Double-click to open it.";
  else if (spec.symbol == Symbol::Const)
    m.infoBody = "Constant: a fixed value.";
  else
    m.infoBody = std::string(spec.help);
  if (m.hover.kind == CanvasHit::Kind::Pin) {
    const ScenePin& pin = sc.pins[m.hover.index];
    if (!m.infoBody.empty()) m.infoBody += " ";
    m.infoBody += std::string(pin.input ? "Input " : "Output ") +
                  (pin.label != UINT32_MAX ? sc.strings[pin.label] : std::to_string(pin.port)) + ".";
  }
  return {m.infoTitle, m.infoBody};
}

// ---- TMM.4: faces --------------------------------------------------------------------------------------------------

void CircuitCanvas::rebindFaces() {
  impl_->bindFaces(true);
  impl_->pollFaces();
  markDirty();
}

MetaFace* CircuitCanvas::faceOn(uint32_t node) const noexcept {
  const Impl& m = *impl_;
  return node < m.faceOf.size() && m.faceOf[node] >= 0 ? m.faces[static_cast<size_t>(m.faceOf[node])].meta() : nullptr;
}
size_t CircuitCanvas::faceCount() const noexcept { return impl_->metaIdx.size(); }
MetaFace& CircuitCanvas::faceAt(size_t i) const noexcept { return *impl_->faces[impl_->metaIdx[i]].meta(); }
uint32_t CircuitCanvas::faceNode(size_t i) const noexcept { return impl_->faces[impl_->metaIdx[i]].node; }
// TX.panel-faces
size_t CircuitCanvas::panelCount() const noexcept { return impl_->panelIdx.size(); }
Face& CircuitCanvas::panelAt(size_t i) const noexcept { return *impl_->faces[impl_->panelIdx[i]].face; }
uint32_t CircuitCanvas::panelNode(size_t i) const noexcept { return impl_->faces[impl_->panelIdx[i]].node; }
Face* CircuitCanvas::panelOn(uint32_t node) const noexcept {
  const Impl& m = *impl_;
  if (node >= m.faceOf.size() || m.faceOf[node] < 0) return nullptr;
  const auto& h = m.faces[static_cast<size_t>(m.faceOf[node])];
  return h.panel ? h.face.get() : nullptr;
}
bool CircuitCanvas::faceVisible(uint32_t node) const noexcept {
  const Impl& m = *impl_;
  return m.facesShown && m.scene && node < m.faceOf.size() && m.faceOf[node] >= 0 &&
         m.faceVisible(m.faces[static_cast<size_t>(m.faceOf[node])]);
}
void CircuitCanvas::damageFace(uint32_t node, const ui::Rect* local) {  // TL.6
  Impl& m = *impl_;
  ui::UiTree* tr = tree();
  if (tr == nullptr || !faceVisible(node)) return;
  const auto& h = m.faces[static_cast<size_t>(m.faceOf[node])];
  if (local == nullptr) return m.damageFace(h);
  SceneNode n = m.scene->nodes[h.node];
  n.x += m.shiftOf(h.node).x, n.y += m.shiftOf(h.node).y;  // TX.block-drag
  const float k = h.scale;
  const ui::IRect cr = rect();
  const ui::Rect d{std::max(m.devX(n.x + local->x0 * k) - 2.0f, static_cast<float>(cr.x)),
                   std::max(m.devY(n.y + local->y0 * k) - 2.0f, static_cast<float>(cr.y)),
                   std::min(m.devX(n.x + local->x1 * k) + 2.0f, static_cast<float>(cr.right())),
                   std::min(m.devY(n.y + local->y1 * k) + 2.0f, static_cast<float>(cr.bottom()))};
  if (d.x1 > d.x0 && d.y1 > d.y0) tr->damage().add(d);
}
void CircuitCanvas::wakeFaces() { impl_->pollFaces(); }
bool CircuitCanvas::animatingFaces() const noexcept { return impl_->faceTimer != 0; }
void CircuitCanvas::setFacesShown(bool on) {
  Impl& m = *impl_;
  if (m.facesShown == on) return;
  m.facesShown = on;
  m.chunks.clear();  // the Far chunks hold face nodes as boxes when faces are off
  m.chunkScale = 0.0f;
  markDirty();
}
bool CircuitCanvas::facesShown() const noexcept { return impl_->facesShown; }
bool CircuitCanvas::faceDragging() const noexcept { return impl_->faceCapture >= 0; }

// ---- TX.cables -------------------------------------------------------------------------------------------------------

void CircuitCanvas::setCablesAnimated(bool on) {
  Impl& m = *impl_;
  if (m.sim.animated() == on) return;
  const bool wasAwake = m.sim.awake();
  m.sim.setAnimated(on);
  m.orbitAnimated = on;  // TX.orbit-ports: the ports' slides too
  if (!on && m.orbitsMoving()) {
    for (Impl::OrbitRef& o : m.orbits) o.cur = o.target;
    m.refreshOrbitCables();
    markDirty();
  }
  if (wasAwake) markDirty();
}
bool CircuitCanvas::cablesAnimated() const noexcept { return impl_->sim.animated(); }

graph::Point2 CircuitCanvas::pinPoint(uint32_t pin) const noexcept {
  const Impl& m = *impl_;
  if (!m.scene || pin >= m.scene->pins.size()) return {};
  return m.pinXY(pin);
}
bool CircuitCanvas::pinOrbits(uint32_t pin) const noexcept { return impl_->orbitAt(pin) >= 0; }
float CircuitCanvas::portAngle(uint32_t pin) const noexcept {
  const int32_t o = impl_->orbitAt(pin);
  return o >= 0 ? impl_->orbits[static_cast<size_t>(o)].cur : 0.0f;
}
float CircuitCanvas::portTarget(uint32_t pin) const noexcept {
  const int32_t o = impl_->orbitAt(pin);
  return o >= 0 ? impl_->orbits[static_cast<size_t>(o)].target : 0.0f;
}
bool CircuitCanvas::portsMoving() const noexcept { return impl_->orbitsMoving(); }
uint64_t CircuitCanvas::portSolves() const noexcept { return impl_->orbitSolves; }

void CircuitCanvas::setPortAngle(uint32_t pin, float angle) {
  Impl& m = *impl_;
  const int32_t oi = m.orbitAt(pin);
  if (oi < 0) return;
  Impl::OrbitRef& o = m.orbits[static_cast<size_t>(oi)];
  o.held = true;
  o.heldAngle = wrapAngle(angle);
  const uint32_t one[1] = {o.node};
  m.retarget(one);
  m.damagePort(o);
  o.cur = o.target;  // the held port is where the pointer puts it (the other one slides out of its way)
  m.damagePort(o);
  m.refreshOrbitCables();
  markDirty();
}

void CircuitCanvas::releasePort(uint32_t pin) {
  Impl& m = *impl_;
  const int32_t oi = m.orbitAt(pin);
  if (oi < 0 || !m.orbits[static_cast<size_t>(oi)].held) return;
  m.orbits[static_cast<size_t>(oi)].held = false;
  const uint32_t one[1] = {m.orbits[static_cast<size_t>(oi)].node};
  m.retarget(one);
  m.refreshOrbitCables();
  markDirty();
}

bool CircuitCanvas::cablesMoving() const noexcept { return impl_->sim.awake(); }

std::span<const graph::Point2> CircuitCanvas::cablePoints(uint32_t wire) const noexcept {
  const Impl& m = *impl_;
  if (wire >= m.cableOf.size() || m.cableOf[wire] < 0) return {};
  return m.sim.points(static_cast<uint32_t>(m.cableOf[wire]));
}

graph::Point2 CircuitCanvas::cableMidpoint(uint32_t wire) const noexcept {
  const std::span<const graph::Point2> p = cablePoints(wire);
  if (p.empty()) return {};
  // The point halfway along the rope.
  float total = 0.0f;
  for (size_t i = 0; i + 1 < p.size(); ++i) total += std::hypot(p[i + 1].x - p[i].x, p[i + 1].y - p[i].y);
  float rest = 0.5f * total;
  for (size_t i = 0; i + 1 < p.size(); ++i) {
    const float l = std::hypot(p[i + 1].x - p[i].x, p[i + 1].y - p[i].y);
    if (l >= rest && l > 0.0f) return {p[i].x + (p[i + 1].x - p[i].x) * rest / l, p[i].y + (p[i + 1].y - p[i].y) * rest / l};
    rest -= l;
  }
  return p.back();
}

void CircuitCanvas::setLooseCable(graph::Point2 from, graph::Point2 to, const WireStyle& style, bool snapped) {
  Impl& m = *impl_;
  if (!m.looseOn || m.looseDropped) {
    if (m.looseOn) m.clearLoose();
    m.loose.setCarry(kLooseCableCarry);
    m.loose.setAnimated(m.sim.animated());
    const CableSim::Spec sp{1, from, from};  // it comes out of the port
    m.loose.sync({&sp, 1});
    m.looseOn = true;
    m.looseBase = m.simKeys;
    std::sort(m.looseBase.begin(), m.looseBase.end());
  }
  if (m.looseSnap != snapped || !(m.looseStyle == style)) markDirty();
  m.looseSnap = snapped;
  m.looseStyle = style;
  m.loose.setEnds(0, from, to, true);
  m.damageLoose();
  if (m.loose.awake()) {
    if (m.looseLast < 0.0 && tree() != nullptr) m.looseLast = tree()->now();
    requestFrame();
  }
}

void CircuitCanvas::dropLooseCable() {
  Impl& m = *impl_;
  if (!m.looseOn) return;
  m.looseDropped = true;
  m.looseSnap = false;
  requestFrame();  // it goes at the next frame unless a new cable took its motion over first
}

void CircuitCanvas::clearLooseCable() { impl_->clearLoose(); }
bool CircuitCanvas::looseCable() const noexcept { return impl_->looseOn; }
bool CircuitCanvas::looseCableSnapped() const noexcept { return impl_->looseOn && impl_->looseSnap; }
bool CircuitCanvas::looseCableMoving() const noexcept { return impl_->looseOn && impl_->loose.awake(); }
std::span<const graph::Point2> CircuitCanvas::looseCablePoints() const noexcept {
  const Impl& m = *impl_;
  return m.looseOn && m.loose.size() > 0 ? m.loose.points(0) : std::span<const graph::Point2>{};
}

std::optional<graph::Point2> CircuitCanvas::faceControlPoint(uint32_t node, std::string_view param, graph::Point2 toward) const {
  const Impl& m = *impl_;
  if (!m.scene || node >= m.scene->nodes.size() || !m.faced(node)) return std::nullopt;
  const auto& h = m.faces[static_cast<size_t>(m.faceOf[node])];
  const std::optional<ControlAnchor> an = h.face->controlAnchor(param);
  if (!an) return std::nullopt;
  const SceneNode& n = m.scene->nodes[node];
  const Point2 centre{n.x + an->at.x * h.scale, n.y + an->at.y * h.scale};
  const float dx = toward.x - centre.x, dy = toward.y - centre.y, len = std::sqrt(dx * dx + dy * dy);
  const float r = an->radius * h.scale;
  return len > r + 1e-3f ? Point2{centre.x + dx / len * r, centre.y + dy / len * r} : centre;
}

void CircuitCanvas::setNodeDrag(std::span<const uint32_t> nodes, graph::Point2 offset) {
  Impl& m = *impl_;
  if (!m.scene) return;
  // The same set (each move of a drag): only the offset — no allocation, no scan.
  const bool same = nodes.size() == m.dragNodes.size() && std::is_sorted(nodes.begin(), nodes.end()) &&
                    std::equal(nodes.begin(), nodes.end(), m.dragNodes.begin());
  if (same && offset == m.dragOffset) return;
  if (!same) {
    m.dragNodes.assign(nodes.begin(), nodes.end());
    std::sort(m.dragNodes.begin(), m.dragNodes.end());
    m.dragNodes.erase(std::unique(m.dragNodes.begin(), m.dragNodes.end()), m.dragNodes.end());
    std::erase_if(m.dragNodes, [&](uint32_t v) { return v >= m.scene->nodes.size(); });
    if (m.dragNodes.empty()) m.shiftNodes.clear(), m.shiftLayer.clear(), m.shiftOffsets.clear();
    m.buildLift();
    m.buildOrbitLive();  // TX.orbit-ports
  }
  m.dragOffset = offset;
  if (!m.anyLift()) m.clearLift();
  // TX.orbit-ports: the ports on the moved blocks and on their cables' other ends turn toward where those are now.
  m.retarget(m.orbitLive);
  m.refreshCableEnds(true);
  markDirty();
}

void CircuitCanvas::setNodeShift(std::span<const uint32_t> nodes, std::span<const uint8_t> layers,
                                 std::span<const graph::Point2> offsets) {
  Impl& m = *impl_;
  if (!m.scene || m.dragNodes.empty() || layers.size() != nodes.size()) return;
  // The same sets (each move of a drag): only the offsets — no allocation, no scan of the scene.
  const bool same = nodes.size() == m.shiftNodes.size() && offsets.size() == m.shiftOffsets.size() &&
                    std::equal(nodes.begin(), nodes.end(), m.shiftNodes.begin()) &&
                    std::equal(layers.begin(), layers.end(), m.shiftLayer.begin());
  if (same && std::equal(offsets.begin(), offsets.end(), m.shiftOffsets.begin())) return;
  if (!same) {
    m.shiftNodes.clear();
    m.shiftLayer.clear();
    for (size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i] < m.scene->nodes.size() && layers[i] < offsets.size() &&
          !std::binary_search(m.dragNodes.begin(), m.dragNodes.end(), nodes[i]))
        m.shiftNodes.push_back(nodes[i]), m.shiftLayer.push_back(layers[i]);
    m.shiftOffsets.assign(offsets.begin(), offsets.end());
    m.buildLift();
    m.buildOrbitLive();
  } else {
    std::copy(offsets.begin(), offsets.end(), m.shiftOffsets.begin());
  }
  m.retarget(m.orbitLive);
  m.refreshCableEnds(true);
  markDirty();
}

void CircuitCanvas::clearNodeDrag() {
  Impl& m = *impl_;
  if (!m.anyLift()) return;
  m.clearLift();
  // TX.orbit-ports: back to where the scene settles them.
  thread_local std::vector<uint32_t> live;
  live.swap(m.orbitLive);
  m.orbitLive.clear();
  m.retarget(live);
  m.refreshCableEnds(true);
  markDirty();
}

std::span<const uint32_t> CircuitCanvas::draggedNodes() const noexcept { return impl_->dragNodes; }
graph::Point2 CircuitCanvas::nodeDragOffset() const noexcept { return impl_->dragOffset; }
bool CircuitCanvas::nodeLifted(uint32_t node) const noexcept { return impl_->layerOf(node) == 1; }
bool CircuitCanvas::lifting() const noexcept { return impl_->paintPass >= 1; }
std::span<const uint32_t> CircuitCanvas::shiftedNodes() const noexcept { return impl_->shiftNodes; }
graph::Point2 CircuitCanvas::nodeOffset(uint32_t node) const noexcept { return impl_->shiftOf(node); }
uint8_t CircuitCanvas::nodeLayer(uint32_t node) const noexcept { return impl_->layerOf(node); }
uint8_t CircuitCanvas::paintLayer() const noexcept { return static_cast<uint8_t>(std::max<int8_t>(impl_->paintPass, 0)); }

void CircuitCanvas::wakeCables() {
  Impl& m = *impl_;
  if (m.glowTimer != 0 || m.cables.empty() || !cableLevels) return;  // polling already, or nothing to light
  m.pollCables();
}

float CircuitCanvas::cableGlow(uint32_t wire) const noexcept {
  const Impl& m = *impl_;
  if (wire >= m.cableOf.size() || m.cableOf[wire] < 0) return 0.0f;
  return m.cables[static_cast<size_t>(m.cableOf[wire])].glow;
}
bool CircuitCanvas::cableGlowPolling() const noexcept { return impl_->glowTimer != 0; }

bool CircuitCanvas::facePointer(const ui::PointerEvent& e) {
  Impl& m = *impl_;
  if (!m.facesShown || m.faces.empty() || !m.scene) return false;
  using K = FacePointer::Kind;
  switch (e.type) {
    case ui::PointerType::Down: {
      // Any press moves the key focus; a face's control takes it below (TX.panel-faces: the old face's type-in commits).
      const int32_t before = m.faceFocus;
      m.faceFocus = -1;
      // Left presses, and right presses a face uses (TMM.9: resetting harmonic bars); Alt+Left pans.
      bool taken = false;
      Face* fp0 = nullptr;
      if ((e.button == ui::PointerButton::Left || e.button == ui::PointerButton::Right) && !(e.mods & ui::mod::kAlt)) {
        // TX.cables: a press on a cable's plug is the cable's, not the control's under it.
        // TX.orbit-ports: a floating panel is over the cables too.
        if (const int32_t i = m.faceUnder(e.pos); i >= 0 && (m.popupUnder(e.pos) >= 0 || m.cableEndAt(e.pos) == UINT32_MAX)) {
          Face& f = *m.faces[static_cast<size_t>(i)].face;
          const FacePointer fp = m.pointerOf(i, e, K::Down);
          if (before >= 0 && before != i) m.blurFace(before);
          if (f.hitTest(fp.local, fp.pxPerUnit) >= 0 && f.onPointer(fp)) {
            taken = true;
            fp0 = &f;
          }
        }
      }
      if (!taken) {
        m.blurFace(before);
        m.syncTextInput();
        return false;
      }
      if (tree()) tree()->setFocus(this);
      // The face's edit may have rebuilt the scene (TMM.7): find it again (it lives on, kept or retired).
      const int32_t now = m.indexOf(fp0);
      m.faceFocus = now;
      m.faceCapture = now >= 0 && fp0->dragging() ? now : -1;
      m.syncTextInput();
      markDirty();
      return true;
    }
    case ui::PointerType::Move: {
      if (m.faceCapture >= 0) {
        (void)m.faces[static_cast<size_t>(m.faceCapture)].face->onPointer(m.pointerOf(m.faceCapture, e, K::Move));
        markDirty();
        return true;
      }
      const int32_t i = m.faceUnder(e.pos);
      if (m.faceHover >= 0 && m.faceHover != i && static_cast<size_t>(m.faceHover) < m.faces.size() &&
          m.faces[static_cast<size_t>(m.faceHover)].face->setHoveredPart(Face::kNone))
        markDirty();
      if (m.faceHover != i) infoChanged();
      m.faceHover = i;
      if (i >= 0) {
        Face& f = *m.faces[static_cast<size_t>(i)].face;
        const FacePointer fp = m.pointerOf(i, e, K::Move);
        if (f.setHoveredPart(f.hitTest(fp.local, fp.pxPerUnit))) {
          markDirty();
          infoChanged();  // the face's info names the control under the pointer
        }
        if (f.onPointer(fp)) {  // TMM.9: the face's own hover (a bar, a draw cursor, Alt)
          markDirty();
          infoChanged();
        }
      }
      return false;
    }
    case ui::PointerType::Up: {
      if (m.faceCapture < 0) return false;
      const int32_t i = m.faceCapture;
      m.faceCapture = -1;
      Face* f = m.faces[static_cast<size_t>(i)].face.get();
      (void)f->onPointer(m.pointerOf(i, e, K::Up));
      // TX.panel-faces: a panel's control may take the keys after its press (a number box's type-in).
      m.faceFocus = m.indexOf(f);
      m.syncTextInput();
      markDirty();
      return true;
    }
    case ui::PointerType::Wheel: {
      const int32_t i = m.faceUnder(e.pos);
      return i >= 0 && m.faces[static_cast<size_t>(i)].face->onPointer(m.pointerOf(i, e, K::Wheel));
    }
    case ui::PointerType::Leave: {
      if (m.faceHover >= 0 && static_cast<size_t>(m.faceHover) < m.faces.size() &&
          m.faces[static_cast<size_t>(m.faceHover)].face->setHoveredPart(Face::kNone))
        markDirty();
      m.faceHover = -1;
      return false;
    }
  }
  return false;
}

bool CircuitCanvas::onText(const ui::TextEvent& e) {
  Impl& m = *impl_;
  if (!m.facesShown || m.faceFocus < 0 || static_cast<size_t>(m.faceFocus) >= m.faces.size()) return false;
  Face& f = *m.faces[static_cast<size_t>(m.faceFocus)].face;
  const bool used = f.onText(e);
  m.syncTextInput();
  if (used) markDirty();
  return used;
}

ui::Cursor CircuitCanvas::faceCursor(ui::Vec2 pos) const {
  const Impl& m = *impl_;
  if (m.faceCapture >= 0) {
    const Face& f = *m.faces[static_cast<size_t>(m.faceCapture)].face;
    return f.cursor(f.hoveredPart() >= 0 ? f.hoveredPart() : 0);
  }
  const int32_t i = m.faceUnder(pos);
  if (i < 0) return ui::Cursor::Inherit;
  const auto& h = m.faces[static_cast<size_t>(i)];
  const Face& f = *h.face;
  return f.cursor(f.hitTest(m.localOf(h, toWorld(pos)), m.s() * h.scale));
}

bool CircuitCanvas::faceTakes(ui::Vec2 pos) const { return impl_->faceCapture >= 0 || impl_->faceUnder(pos) >= 0; }

CircuitCanvas::FaceParamHit CircuitCanvas::faceParamAt(ui::Vec2 pos) const {
  const Impl& m = *impl_;
  FaceParamHit out;
  const int32_t i = m.faceUnder(pos);
  if (i < 0) return out;
  const auto& h = m.faces[static_cast<size_t>(i)];
  out.node = h.node;
  out.param = h.face->paramAt(m.localOf(h, toWorld(pos)));
  return out;
}

bool CircuitCanvas::faceInfo(ui::InfoText& out) const {
  const Impl& m = *impl_;
  const int32_t i = m.faceCapture >= 0 ? m.faceCapture : m.faceHover;
  if (i < 0 || static_cast<size_t>(i) >= m.faces.size()) return false;
  const Face& f = *m.faces[static_cast<size_t>(i)].face;
  if (f.hoveredPart() == Face::kNone) return false;
  f.describe(f.hoveredPart(), m.faceTitle, m.faceBody);
  if (m.faceTitle.empty() && m.faceBody.empty()) return false;
  out = {m.faceTitle, m.faceBody};
  return true;
}

}  // namespace pq::views::circuit
