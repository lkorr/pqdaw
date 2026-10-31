// T5.7: the OLE drop target (pq/app/os_drop.h) without a real drag: a hand-built CF_HDROP data object goes through
// DragEnter / DragOver / DragLeave / Drop, and the target registers on a real window.

#include <doctest/doctest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>

#include <cstring>
#include <string>
#include <vector>

#include "pq/app/os_drop.h"

using namespace pq;
using namespace pq::app;

namespace {

// A data object that offers CF_HDROP of the given paths (wide, double-NUL terminated after a DROPFILES header).
struct FileData final : IDataObject {
  ULONG refs = 1;
  std::vector<std::wstring> files;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *out = static_cast<IDataObject*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override {
    if (QueryGetData(f) != S_OK) return DV_E_FORMATETC;
    size_t chars = 1;
    for (const std::wstring& w : files) chars += w.size() + 1;
    const size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GHND, bytes);
    auto* df = static_cast<DROPFILES*>(GlobalLock(g));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    auto* p = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DROPFILES));
    for (const std::wstring& w : files) {
      std::memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
      p += w.size() + 1;
    }
    *p = L'\0';
    GlobalUnlock(g);
    m->tymed = TYMED_HGLOBAL;
    m->hGlobal = g;
    m->pUnkForRelease = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override {
    return f && f->cfFormat == CF_HDROP && (f->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* o) override {
    o->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

}  // namespace

TEST_CASE("os drop: files, positions, keys and effects through the IDropTarget") {
  std::vector<OsDragEvent> seen;
  ui::DropEffect answer = ui::DropEffect::Copy;
  auto t = OsDropTarget::unregistered(
      [&](const OsDragEvent& e) {
        seen.push_back(e);
        return answer;
      },
      [](int32_t sx, int32_t sy, float& x, float& y) {  // the window's client area starts at (100, 50) on screen
        x = static_cast<float>(sx - 100);
        y = static_cast<float>(sy - 50);
      });
  auto* target = static_cast<IDropTarget*>(t->comTarget());
  FileData data;
  data.files = {L"C:\\Samples\\Kick.wav", L"C:\\Samples\\H\u00e9 Snare.flac"};
  DWORD fx = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
  REQUIRE(target->DragEnter(&data, MK_CONTROL, POINTL{150, 80}, &fx) == S_OK);
  CHECK(fx == DROPEFFECT_COPY);
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].type == OsDragEvent::Type::Enter);
  REQUIRE(seen[0].paths.size() == 2);
  CHECK(seen[0].paths[0] == "C:\\Samples\\Kick.wav");
  CHECK(seen[0].paths[1] == "C:\\Samples\\H\xC3\xA9 Snare.flac");  // UTF-8
  CHECK(seen[0].x == 50.0f);
  CHECK(seen[0].y == 30.0f);
  CHECK(seen[0].mods == ui::mod::kCtrl);
  // Refused over some target: the OS shows its "no" cursor.
  answer = ui::DropEffect::None;
  fx = DROPEFFECT_COPY | DROPEFFECT_MOVE;
  REQUIRE(target->DragOver(MK_SHIFT, POINTL{160, 90}, &fx) == S_OK);
  CHECK(fx == DROPEFFECT_NONE);
  CHECK(seen.back().type == OsDragEvent::Type::Over);
  CHECK(seen.back().mods == ui::mod::kShift);
  // A Move the source does not allow falls back to Copy.
  answer = ui::DropEffect::Move;
  fx = DROPEFFECT_COPY;
  REQUIRE(target->DragOver(0, POINTL{170, 90}, &fx) == S_OK);
  CHECK(fx == DROPEFFECT_COPY);
  REQUIRE(target->DragLeave() == S_OK);
  CHECK(seen.back().type == OsDragEvent::Type::Leave);
  // A drop.
  answer = ui::DropEffect::Copy;
  fx = DROPEFFECT_COPY | DROPEFFECT_MOVE;
  REQUIRE(target->DragEnter(&data, 0, POINTL{200, 200}, &fx) == S_OK);
  REQUIRE(target->Drop(&data, 0, POINTL{210, 220}, &fx) == S_OK);
  CHECK(fx == DROPEFFECT_COPY);
  CHECK(seen.back().type == OsDragEvent::Type::Drop);
  CHECK(seen.back().paths.size() == 2);
  CHECK(seen.back().x == 110.0f);
  CHECK(seen.back().y == 170.0f);
}

TEST_CASE("os drop: a drag without files is refused and never reaches the sink") {
  int calls = 0;
  auto t = OsDropTarget::unregistered(
      [&](const OsDragEvent&) {
        ++calls;
        return ui::DropEffect::Copy;
      },
      [](int32_t, int32_t, float& x, float& y) { x = y = 0; });
  auto* target = static_cast<IDropTarget*>(t->comTarget());
  FileData empty;  // offers CF_HDROP with no files
  DWORD fx = DROPEFFECT_COPY;
  REQUIRE(target->DragEnter(&empty, 0, POINTL{0, 0}, &fx) == S_OK);
  CHECK(fx == DROPEFFECT_NONE);
  fx = DROPEFFECT_COPY;
  REQUIRE(target->DragOver(0, POINTL{1, 1}, &fx) == S_OK);
  CHECK(fx == DROPEFFECT_NONE);
  REQUIRE(target->Drop(&empty, 0, POINTL{1, 1}, &fx) == S_OK);
  CHECK(fx == DROPEFFECT_NONE);
  CHECK(calls == 0);
}

TEST_CASE("os drop: registers on a real window (OLE initialised) and revokes cleanly") {
  REQUIRE(SUCCEEDED(OleInitialize(nullptr)));
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"pq_os_drop_test";
  RegisterClassW(&wc);
  HWND w = CreateWindowExW(0, wc.lpszClassName, L"drop", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, nullptr, nullptr,
                           wc.hInstance, nullptr);
  REQUIRE(w != nullptr);
  {
    auto t = OsDropTarget::registerOn(w, [](const OsDragEvent&) { return ui::DropEffect::Copy; });
    REQUIRE_MESSAGE(t.ok(), (t.ok() ? "" : t.error().message));
    CHECK((*t)->registered());
    // A second registration on the same window fails (the slot is taken).
    auto again = OsDropTarget::registerOn(w, [](const OsDragEvent&) { return ui::DropEffect::Copy; });
    CHECK_FALSE(again.ok());
  }
  {
    // Revoked by the destructor: the window takes a new target.
    auto t2 = OsDropTarget::registerOn(w, [](const OsDragEvent&) { return ui::DropEffect::Copy; });
    CHECK(t2.ok());
  }
  DestroyWindow(w);
  OleUninitialize();
}
