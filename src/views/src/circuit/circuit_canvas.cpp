// The circuit canvas (T4.2): view transform, level of detail, symbol tiles, wire drawing, culling, hit testing,
// selection and input. See pq/views/circuit/circuit_canvas.h.

#include "pq/views/circuit/circuit_canvas.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
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

namespace pq::views::circuit {

using graph::Point2;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr uint32_t kTileBuildTexelsPerFrame = 1u << 20;
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

struct CircuitCanvas::Impl {
  CircuitCanvas& self;
  std::shared_ptr<const CircuitScene> scene;
  View view;
  bool fitPending = false;
  bool fitReadable = false;  // M15: the pending / next fit is an open (readable), not F (the whole patch)
  bool stubsNearOnly = false;  // T5.x-circuit-chain
  double pillMinZoom = 0.0;    // T5.x-circuit-chain
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
    for (const uint32_t sg : visSegs) {
      const SceneSegment& seg = s.segments[sg];
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
      if (faced(v)) continue;  // TMM.4: faces are drawn over the chunks at every level
      const SceneNode& n = s.nodes[v];
      const ui::Rect rr{static_cast<float>(std::round(ox + n.x * sc)), static_cast<float>(std::round(oy + n.y * sc)),
                        static_cast<float>(std::round(ox + (n.x + n.w) * sc)), static_cast<float>(std::round(oy + (n.y + n.h) * sc))};
      if (rr.x1 <= rr.x0 || rr.y1 <= rr.y0) continue;
      dlN.addRectStroke(rr, nodeCol, 1.0f);
      ++stats.nodesPainted;
    }
  }

  // A wire drawn over the Far picture (selection, hover).
  void farWire(ui::DrawList& dl, uint32_t w, double ox, double oy, float sc, float thin, Rgba col) {
    if (!scene || w >= scene->wires.size()) return;
    const SceneWire& sw = scene->wires[w];
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
        if (atlas) {
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
    std::unique_ptr<MetaFace> face;
    std::vector<FaceJack> jacks;
    uint32_t owner = 0;  // TMM.7: the instance (SceneFace owner, path): a new scene keeps its face
    std::string path;
    bool bound = false;
  };
  std::vector<Hosted> faces;
  std::vector<int32_t> faceOf;  // per scene node: index into faces, or -1
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
  static uint64_t faceKey(const MetaFace& f, uint32_t layer) {
    const uint64_t base = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&f.type())) << 3) ^ layer;
    const uint64_t inst = f.staticKey(layer);
    return inst == 0 ? base : (base ^ (inst * 0x9E3779B97F4A7C15ull)) | 1ull << 63;
  }
  // Drops tiles no hosted face uses any more (instance keys change with their data) once the map has grown.
  void pruneFaceTiles() {
    if (faceTiles.size() <= 32 + 4 * faces.size()) return;
    std::unordered_map<uint64_t, ui::LayerTile> keep;
    for (const Hosted& h : faces)
      for (uint32_t layer = 0; layer < h.face->staticLayerCount(); ++layer) {
        const uint64_t k = faceKey(*h.face, layer);
        auto it = faceTiles.find(k);
        if (it != faceTiles.end()) keep.emplace(k, it->second);
      }
    faceTiles.swap(keep);
  }

  // TMM.7: a new scene keeps the faces of the instances it still shows (same type, owner and path): their bindings,
  // their own state (a selected keyframe, voice fades) and any drag in progress survive a rebuild — which a face's own
  // edit can cause while its handler runs (a setting is structural: the host rebuilds synchronously). Faces that go
  // are retired, not destroyed, until the next paint, so a handler that caused the rebuild returns into a live object.
  std::vector<std::unique_ptr<MetaFace>> retired;
  int32_t indexOf(const MetaFace* f) const noexcept {
    for (size_t i = 0; i < faces.size(); ++i)
      if (faces[i].face.get() == f) return static_cast<int32_t>(i);
    return -1;
  }

  void buildFaces() {
    std::vector<Hosted> old;
    old.swap(faces);
    auto ptrAt = [&](int32_t i) -> const MetaFace* {
      return i >= 0 && static_cast<size_t>(i) < old.size() ? old[static_cast<size_t>(i)].face.get() : nullptr;
    };
    const MetaFace *capture = ptrAt(faceCapture), *hoverFace = ptrAt(faceHover), *focus = ptrAt(faceFocus);
    faceCapture = faceHover = faceFocus = -1;
    faceOf.assign(scene ? scene->nodes.size() : 0, -1);
    for (uint32_t e = 0; scene && e < scene->faces.size(); ++e) {
      const SceneFace& f = scene->faces[e];
      if (f.node >= scene->nodes.size() || f.type == nullptr) continue;
      std::unique_ptr<MetaFace> face;
      bool reused = false;
      for (Hosted& o : old)
        if (o.face && &o.face->type() == f.type && o.owner == f.owner && o.path == f.path) {
          face = std::move(o.face);
          reused = true;
          break;
        }
      if (!face) face = makeFace(*f.type);
      if (!face) continue;
      Hosted h;
      h.node = f.node;
      h.entry = e;
      h.owner = f.owner;
      h.path = f.path;
      h.bound = reused;
      h.face = std::move(face);
      const SceneNode& n = scene->nodes[f.node];
      for (uint32_t k = 0; k < n.pinCount; ++k) {
        const ScenePin& pin = scene->pins[n.firstPin + k];
        FaceJack j;
        j.at = {pin.x - n.x, pin.y - n.y};
        j.input = pin.input;
        j.connected = pin.connected;
        j.visible = pin.visible;
        if (pin.label != UINT32_MAX) j.name = scene->strings[pin.label];
        h.jacks.push_back(j);
      }
      faceOf[f.node] = static_cast<int32_t>(faces.size());
      faces.push_back(std::move(h));
    }
    for (Hosted& o : old)
      if (o.face) retired.push_back(std::move(o.face));
    faceCapture = indexOf(capture);
    faceHover = indexOf(hoverFace);
    faceFocus = indexOf(focus);
    bindFaces(false);
  }

  // Binds the faces through the host's binder (all: the host's state moved; else only the faces new to this scene).
  void bindFaces(bool all) {
    for (Hosted& h : faces) {
      if (h.bound && !all) continue;
      CircuitCanvas::FaceBinding b;
      if (self.faceBinder) b = self.faceBinder(scene->faces[h.entry]);
      h.face->bind(std::move(b.target), b.voices);
      h.bound = true;
    }
  }

  // The face whose block is under a device point (-1: none).
  int32_t faceUnder(Vec2 d) const noexcept {
    if (!facesShown || !scene || !self.rect().contains(d)) return -1;
    const Point2 w = self.toWorld(d);
    for (size_t i = faces.size(); i-- > 0;) {
      const SceneNode& n = scene->nodes[faces[i].node];
      if (!n.hidden && w.x >= n.x && w.x <= n.x + n.w && w.y >= n.y && w.y <= n.y + n.h) return static_cast<int32_t>(i);
    }
    return -1;
  }

  FacePointer pointerOf(int32_t i, const ui::PointerEvent& e, FacePointer::Kind kind) const {
    const SceneNode& n = scene->nodes[faces[static_cast<size_t>(i)].node];
    const Point2 w = self.toWorld(e.pos);
    FacePointer fp;
    fp.kind = kind;
    fp.local = {w.x - n.x, w.y - n.y};
    fp.device = e.pos;
    fp.pxPerUnit = s();
    fp.uiScale = uiScale();
    fp.mods = e.mods;
    fp.clicks = e.clickCount;
    fp.wheel = e.wheel;
    if (e.button != ui::PointerButton::None) fp.button = e.button;
    return fp;
  }

  bool faceVisible(const Hosted& h) const noexcept {
    const SceneNode& n = scene->nodes[h.node];
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
      if (rd == nullptr || !faceVisible(h)) continue;
      if (h.face->pollVoices() && tr != nullptr) {
        const SceneNode& n = scene->nodes[h.node];
        faceRects.clear();
        h.face->liveRects(faceRects);
        for (const ui::Rect& lr : faceRects) {
          const ui::Rect d{devX(n.x + lr.x0) - 2.0f, devY(n.y + lr.y0) - 2.0f, devX(n.x + lr.x1) + 2.0f, devY(n.y + lr.y1) + 2.0f};
          const ui::Rect c{std::max(d.x0, static_cast<float>(cr.x)), std::max(d.y0, static_cast<float>(cr.y)),
                           std::min(d.x1, static_cast<float>(cr.right())), std::min(d.y1, static_cast<float>(cr.bottom()))};
          tr->damage().add(c);
        }
      }
      live = live || rd->live() || h.face->animating();
    }
    if (live && faceTimer == 0 && tr != nullptr) faceTimer = self.requestTimer(kFaceFrameSeconds);
  }

  // Draws every visible face: static layers from tiles (or directly), then jacks and the live layer.
  void paintFaces(ui::PaintContext& ctx, const ui::IRect& clip, ui::TileAtlas* atlas, bool& missing) {
    if (!facesShown || faces.empty()) return;
    const ui::Theme& th = *ctx.res.theme;
    const float sc = s();
    bool wantTimer = false;
    for (Hosted& h : faces) {
      const SceneNode& n = scene->nodes[h.node];
      if (n.hidden) continue;
      const float x0 = devX(n.x), y0 = devY(n.y), x1 = devX(n.x + n.w), y1 = devY(n.y + n.h);
      if (x1 < static_cast<float>(clip.x) - 2.0f || x0 > static_cast<float>(clip.right()) + 2.0f ||
          y1 < static_cast<float>(clip.y) - 2.0f || y0 > static_cast<float>(clip.bottom()) + 2.0f)
        continue;
      MetaFace& f = *h.face;
      const Vec2 o{std::round(x0), std::round(y0)};
      for (uint32_t layer = 0; layer < f.staticLayerCount(); ++layer) {
        const Rgba col = f.staticLayerColour(th, layer);
        bool drawn = false;
        // TMM.7: a per-instance layer of a face being dragged changes with every step (a knob re-bakes it): drawn
        // directly until the drag ends, no tile built per step.
        const bool volatileLayer = f.dragging() && f.staticKey(layer) != 0;
        if (atlas && !volatileLayer) {
          auto it = faceTiles.find(faceKey(f, layer));
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
      f.paintJacks(p, env);
      f.paintLive(p, env);
      stats.facePrimitives += p.primitives();
      ++stats.facesPainted;
      wantTimer = wantTimer || (f.voices() != nullptr && (f.voices()->live() || f.animating()));
    }
    if (wantTimer && faceTimer == 0) faceTimer = self.requestTimer(kFaceFrameSeconds);
  }

  // Builds the visible faces' missing static-layer tiles at the current scale. True when it built any.
  bool buildFaceTiles(ui::TileAtlas& atlas) {
    if (!facesShown) return false;
    const float sc = s();
    bool built = false;
    for (const Hosted& h : faces) {
      if (!faceVisible(h)) continue;
      const MetaFace& f = *h.face;
      for (uint32_t layer = 0; layer < f.staticLayerCount(); ++layer) {
        if (f.dragging() && f.staticKey(layer) != 0) continue;
        ui::LayerTile& t = faceTiles[faceKey(f, layer)];
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
  impl_->selection = {};
  impl_->hover = {};
  if (!keepTiles) impl_->tiles.clear();
  impl_->chunks.clear();
  impl_->chunkScale = 0.0f;
  impl_->wireLit.clear();
  impl_->resetSelectionFlags();
  impl_->buildFaces();  // TMM.4
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
  for (uint32_t r = r0; r <= r1; ++r)
    for (uint32_t c = c0; c <= c1; ++c) {
      const uint32_t cell = r * sc.grid.cols + c;
      for (uint32_t k = sc.grid.nodeBegin[cell]; k < sc.grid.nodeBegin[cell + 1]; ++k) {
        const SceneNode& n = sc.nodes[sc.grid.nodeItems[k]];
        for (uint32_t p = 0; p < n.pinCount; ++p) {
          const ScenePin& pin = sc.pins[n.firstPin + p];
          if (!pin.visible) continue;
          const float dd = std::max(std::fabs(pin.x - w.x), std::fabs(pin.y - w.y));
          if (dd <= bestPin) {
            bestPin = dd;
            best = {CanvasHit::Kind::Pin, n.firstPin + p};
          }
        }
      }
    }
  if (best.kind != CanvasHit::Kind::None) return best;
  sc.grid.cellRange(w.x, w.y, w.x, w.y, c0, r0, c1, r1);
  const uint32_t cell = r0 * sc.grid.cols + c0;
  for (uint32_t k = sc.grid.nodeBegin[cell + 1]; k-- > sc.grid.nodeBegin[cell];) {
    const uint32_t i = sc.grid.nodeItems[k];
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

  paintUnderlay(ctx);

  // Visible world rect (+ the margin of captions and AA).
  const Point2 w0 = toWorld({static_cast<float>(clip.x), static_cast<float>(clip.y)});
  const Point2 w1 = toWorld({static_cast<float>(clip.right()), static_cast<float>(clip.bottom())});

  // TMM.6: oversample regions under everything else: one opaque wash (mixed from the background and the signal colour,
  // so overlapping pieces read as one shape) behind exactly the region's members and their wires, and "×n" in the
  // title band above its first member, at every zoom where it fits.
  const Rgba wash = ui::lerp(C.background, C.wireAudio, 0.16f);
  for (const SceneRegion& rg : sc.regions) {
    if (rg.x1 < w0.x || rg.x0 > w1.x || rg.y1 < w0.y || rg.y0 > w1.y) continue;
    const float radius = std::max(1.0f, std::round(5.0f * s));
    for (uint32_t k = rg.firstRect; k < rg.firstRect + rg.rectCount; ++k) {
      const SceneRect& q = sc.regionRects[k];
      if (q.x1 < w0.x || q.x0 > w1.x || q.y1 < w0.y || q.y0 > w1.y) continue;
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
  if (lod != Lod::Far) m.collectVisible(w0.x - pad, w0.y - pad, w1.x + pad, w1.y + pad);

  // ---- wires ----
  const float thin = std::max(1.0f, std::round(us));
  const float thick = std::max(2.0f, std::round(2.0f * us));
  auto baseWireColour = [&](uint32_t w) {
    if (m.wireSel[w]) return C.selected;
    if (m.hover.kind == CanvasHit::Kind::Wire && m.hover.index == w) return C.hover;
    const WireStyle& ws = sc.wires[w].style;
    if (ws.boolean) return m.wireLit[w] ? C.wireBoolLit : C.wireBool;
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
  if (lod == Lod::Far) {
    missing = m.paintFar(dl, *theme, clip, atlas, thin);
  } else {
    // Whole wires (dashes and double lines follow the polyline), each once: the wires of the visible segments.
    uint32_t last = UINT32_MAX;
    for (const uint32_t sg : m.visSegs) {
      const uint32_t w = sc.segments[sg].wire;
      if (w == last) continue;
      last = w;
      const SceneWire& sw = sc.wires[w];
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
      const WireStyle& ws = sc.wires[w].style;
      const float t = ws.poly ? thin : ((!ws.boolean && ws.rate == WireRate::Audio) ? thick : thin);
      dl.addCircleFilled({snapCentre(m.devX(j.x), t), snapCentre(m.devY(j.y), t)}, ws.poly ? dotR + thin : dotR, wireColour(w));
      ++st.junctionsPainted;
    }
  }

  // ---- nodes ----
  auto strokeOf = [&](uint32_t v) {
    if (m.nodeSel[v]) return C.selected;
    if (m.hover.kind == CanvasHit::Kind::Node && m.hover.index == v) return C.hover;
    return sc.nodes[v].inactive ? ui::mulAlpha(C.symbolStroke, kInactiveAlpha) : C.symbolStroke;
  };
  auto dimIf = [&](uint32_t v, Rgba c) { return sc.nodes[v].inactive ? ui::mulAlpha(c, kInactiveAlpha) : c; };
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
      if (v < sc.nodes.size() && !sc.nodes[v].hidden) outline(v, C.selected);
    if (m.hover.kind == CanvasHit::Kind::Node) outline(m.hover.index, C.hover);
    for (const uint32_t w : m.selection.wires) m.farWire(dl, w, ox, oy, s, thin, C.selected);
    if (m.hover.kind == CanvasHit::Kind::Wire) m.farWire(dl, m.hover.index, ox, oy, s, thin, C.hover);
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
      if (pin.visible) {
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

  // ---- labels ----
  if (lod == Lod::Near && ctx.res.text && ctx.res.uiFonts) {
    ui::TextSystem& ts = *ctx.res.text;
    const ui::FontId font = ctx.res.uiFonts->small;
    const uint32_t px = ts.metrics(font).pixelSize;
    const float cap = std::round(0.727f * static_cast<float>(px));
    const bool pins = m.view.zoom >= kPinLabelZoom;
    for (const SceneLabel& lb : sc.labels) {
      if (lb.role == SceneLabel::Role::Pin && !pins) continue;
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
  if (lod != Lod::Near && m.view.zoom >= m.pillMinZoom && ctx.res.text && ctx.res.uiFonts) {
    ui::TextSystem& ts = *ctx.res.text;
    const ui::FontId font = ctx.res.uiFonts->small;
    const float pillPad = std::round(ctx.scale.exact(3.0f)), lh = std::round(ctx.scale.exact(13.0f));
    std::vector<ui::Rect> placed;
    for (const SceneLabel& lb : sc.labels) {
      if (lb.role != SceneLabel::Role::Title || lb.node >= sc.nodes.size()) continue;
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

  // ---- rubber band ----
  if (m.mode == Impl::DragMode::Band && m.tracker.moved) {
    const ui::IRect band = drag::rubberBand(m.tracker.press, m.tracker.last);
    dl.addRectFilled(band.toRect(), C.rubberBand);
    dl.addRectStroke(band.toRect(), C.rubberBandEdge, 1.0f);
  }
  paintOverlay(ctx);
  dl.popClip();
  if (atlas && missing && !m.zooming) {
    m.tilesWanted = true;
    requestFrame();
  }
}

void CircuitCanvas::onFrame(double /*now*/) {
  Impl& m = *impl_;
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
  if (!m.faces[static_cast<size_t>(m.faceFocus)].face->onKey(e)) return false;
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
  m.infoTitle = spec.caption.empty() ? (spec.title.empty() ? std::string("node") : spec.title) : spec.caption + " = " + spec.title;
  if (spec.symbol == Symbol::Subcircuit || spec.symbol == Symbol::Filter) m.infoBody = "Subcircuit " + spec.title + ". Double-click to open it.";
  if (m.hover.kind == CanvasHit::Kind::Pin) {
    const ScenePin& pin = sc.pins[m.hover.index];
    m.infoBody = std::string(pin.input ? "Input " : "Output ") + (pin.label != UINT32_MAX ? sc.strings[pin.label] : std::to_string(pin.port));
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
  return node < m.faceOf.size() && m.faceOf[node] >= 0 ? m.faces[static_cast<size_t>(m.faceOf[node])].face.get() : nullptr;
}
size_t CircuitCanvas::faceCount() const noexcept { return impl_->faces.size(); }
MetaFace& CircuitCanvas::faceAt(size_t i) const noexcept { return *impl_->faces[i].face; }
uint32_t CircuitCanvas::faceNode(size_t i) const noexcept { return impl_->faces[i].node; }
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

bool CircuitCanvas::facePointer(const ui::PointerEvent& e) {
  Impl& m = *impl_;
  if (!m.facesShown || m.faces.empty() || !m.scene) return false;
  using K = FacePointer::Kind;
  switch (e.type) {
    case ui::PointerType::Down: {
      m.faceFocus = -1;  // any press moves the key focus; a face's control takes it below
      // Left presses, and right presses a face uses (TMM.9: resetting harmonic bars); Alt+Left pans.
      if ((e.button != ui::PointerButton::Left && e.button != ui::PointerButton::Right) || (e.mods & ui::mod::kAlt))
        return false;
      const int32_t i = m.faceUnder(e.pos);
      if (i < 0) return false;
      MetaFace& f = *m.faces[static_cast<size_t>(i)].face;
      const FacePointer fp = m.pointerOf(i, e, K::Down);
      if (f.hitTest(fp.local, fp.pxPerUnit) < 0 || !f.onPointer(fp)) return false;
      if (tree()) tree()->setFocus(this);
      // The face's edit may have rebuilt the scene (TMM.7): find it again (it lives on, kept or retired).
      const int32_t now = m.indexOf(&f);
      m.faceFocus = now;
      m.faceCapture = now >= 0 && f.dragging() ? now : -1;
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
          m.faces[static_cast<size_t>(m.faceHover)].face->setHoveredPart(MetaFace::kNone))
        markDirty();
      m.faceHover = i;
      if (i >= 0) {
        MetaFace& f = *m.faces[static_cast<size_t>(i)].face;
        const FacePointer fp = m.pointerOf(i, e, K::Move);
        if (f.setHoveredPart(f.hitTest(fp.local, fp.pxPerUnit))) markDirty();
        if (f.onPointer(fp)) markDirty();  // TMM.9: the face's own hover (a bar, a draw cursor, Alt)
      }
      return false;
    }
    case ui::PointerType::Up: {
      if (m.faceCapture < 0) return false;
      const int32_t i = m.faceCapture;
      m.faceCapture = -1;
      (void)m.faces[static_cast<size_t>(i)].face->onPointer(m.pointerOf(i, e, K::Up));
      markDirty();
      return true;
    }
    case ui::PointerType::Wheel: {
      const int32_t i = m.faceUnder(e.pos);
      return i >= 0 && m.faces[static_cast<size_t>(i)].face->onPointer(m.pointerOf(i, e, K::Wheel));
    }
    case ui::PointerType::Leave: {
      if (m.faceHover >= 0 && static_cast<size_t>(m.faceHover) < m.faces.size() &&
          m.faces[static_cast<size_t>(m.faceHover)].face->setHoveredPart(MetaFace::kNone))
        markDirty();
      m.faceHover = -1;
      return false;
    }
  }
  return false;
}

ui::Cursor CircuitCanvas::faceCursor(ui::Vec2 pos) const {
  const Impl& m = *impl_;
  if (m.faceCapture >= 0) {
    const MetaFace& f = *m.faces[static_cast<size_t>(m.faceCapture)].face;
    return f.cursor(f.hoveredPart() >= 0 ? f.hoveredPart() : 0);
  }
  const int32_t i = m.faceUnder(pos);
  if (i < 0) return ui::Cursor::Inherit;
  const SceneNode& n = m.scene->nodes[m.faces[static_cast<size_t>(i)].node];
  const Point2 w = toWorld(pos);
  const MetaFace& f = *m.faces[static_cast<size_t>(i)].face;
  return f.cursor(f.hitTest({w.x - n.x, w.y - n.y}, m.s()));
}

bool CircuitCanvas::faceInfo(ui::InfoText& out) const {
  const Impl& m = *impl_;
  const int32_t i = m.faceCapture >= 0 ? m.faceCapture : m.faceHover;
  if (i < 0 || static_cast<size_t>(i) >= m.faces.size()) return false;
  const MetaFace& f = *m.faces[static_cast<size_t>(i)].face;
  if (f.hoveredPart() == MetaFace::kNone) return false;
  f.describe(f.hoveredPart(), m.faceTitle, m.faceBody);
  out = {m.faceTitle, m.faceBody};
  return true;
}

}  // namespace pq::views::circuit
