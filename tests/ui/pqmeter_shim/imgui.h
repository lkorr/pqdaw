#pragma once

// Minimal stand-in for <imgui.h>, for the PQ Meter differential test only (T0.9-fix1,
// tests/ui/test_pqmeter_diff.cpp). It is NOT Dear ImGui: it declares just the surface PQ Meter's line helpers
// touch (src/ui/smooth_line.h and the helper block of src/modules/spectrum.cpp), with ImGui's semantics:
//   - ImVec2, ImVec4, ImU32, IM_COL32 / IM_COL32_*_SHIFT (default RGBA packing: R in the low byte)
//   - ImDrawIdx = unsigned int (PQ Meter builds ImGui with ImDrawIdx=unsigned int, see its CMakeLists.txt)
//   - ImDrawList::PrimReserve / PrimWriteVtx / PrimWriteIdx / _VtxCurrentIdx, recording into plain vectors
//   - ImGui::GetFontTexUvWhitePixel (set by the test to pq::ui's white-texel uv, so uvs compare exactly)
//   - ImGui::ColorConvertFloat4ToU32 (ImGui's IM_F32_TO_INT8_SAT rounding)

#include <cstddef>
#include <vector>

using ImU32 = unsigned int;
using ImDrawIdx = unsigned int;

#define IM_COL32_R_SHIFT 0
#define IM_COL32_G_SHIFT 8
#define IM_COL32_B_SHIFT 16
#define IM_COL32_A_SHIFT 24
#define IM_COL32(R, G, B, A)                                                                                     \
  (((ImU32)(A) << IM_COL32_A_SHIFT) | ((ImU32)(B) << IM_COL32_B_SHIFT) | ((ImU32)(G) << IM_COL32_G_SHIFT) | \
   ((ImU32)(R) << IM_COL32_R_SHIFT))

struct ImVec2 {
  float x, y;
  constexpr ImVec2() : x(0.0f), y(0.0f) {}
  constexpr ImVec2(float x_, float y_) : x(x_), y(y_) {}
};

struct ImVec4 {
  float x, y, z, w;
  constexpr ImVec4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
  constexpr ImVec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};

struct ImDrawVert {
  ImVec2 pos;
  ImVec2 uv;
  ImU32 col;
};

struct ImDrawList {
  std::vector<ImDrawVert> VtxBuffer;
  std::vector<ImDrawIdx> IdxBuffer;
  unsigned int _VtxCurrentIdx = 0;
  ImDrawVert* _VtxWritePtr = nullptr;
  ImDrawIdx* _IdxWritePtr = nullptr;

  // As ImGui: grows both buffers by the counts and points the write cursors at the new space.
  void PrimReserve(int idx_count, int vtx_count) {
    const size_t vtxOld = VtxBuffer.size(), idxOld = IdxBuffer.size();
    VtxBuffer.resize(vtxOld + static_cast<size_t>(vtx_count));
    IdxBuffer.resize(idxOld + static_cast<size_t>(idx_count));
    _VtxWritePtr = VtxBuffer.data() + vtxOld;
    _IdxWritePtr = IdxBuffer.data() + idxOld;
  }
  void PrimWriteVtx(const ImVec2& pos, const ImVec2& uv, ImU32 col) {
    _VtxWritePtr->pos = pos;
    _VtxWritePtr->uv = uv;
    _VtxWritePtr->col = col;
    ++_VtxWritePtr;
    ++_VtxCurrentIdx;
  }
  void PrimWriteIdx(ImDrawIdx idx) { *_IdxWritePtr++ = idx; }
};

namespace ImGui {

inline ImVec2& shimWhiteUv() {
  static ImVec2 uv;
  return uv;
}
inline ImVec2 GetFontTexUvWhitePixel() { return shimWhiteUv(); }

inline ImU32 ColorConvertFloat4ToU32(const ImVec4& in) {
  auto sat8 = [](float v) { return (ImU32)((v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v) * 255.0f + 0.5f); };
  return sat8(in.x) << IM_COL32_R_SHIFT | sat8(in.y) << IM_COL32_G_SHIFT | sat8(in.z) << IM_COL32_B_SHIFT |
         sat8(in.w) << IM_COL32_A_SHIFT;
}

}  // namespace ImGui
