// T5.7: our OLE drop target (pq/app/os_drop.h).

#include "pq/app/os_drop.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace pq::app {

namespace {

std::string utf8Of(const wchar_t* w, int n) {
  if (n <= 0) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), bytes, nullptr, nullptr);
  return s;
}

ui::Mods modsOf(DWORD keys) {
  ui::Mods m = ui::mod::kNone;
  if (keys & MK_CONTROL) m |= ui::mod::kCtrl;
  if (keys & MK_SHIFT) m |= ui::mod::kShift;
  if (keys & MK_ALT) m |= ui::mod::kAlt;
  return m;
}

// Our effect within what the source allows (Explorer allows copy, move and link).
DWORD effectOf(ui::DropEffect e, DWORD allowed) {
  DWORD want = DROPEFFECT_NONE;
  switch (e) {
    case ui::DropEffect::None: want = DROPEFFECT_NONE; break;
    case ui::DropEffect::Copy: want = DROPEFFECT_COPY; break;
    case ui::DropEffect::Move: want = DROPEFFECT_MOVE; break;
    case ui::DropEffect::Link: want = DROPEFFECT_LINK; break;
  }
  if (want & allowed) return want;
  return (want != DROPEFFECT_NONE && (allowed & DROPEFFECT_COPY)) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

}  // namespace

namespace {
std::wstring wideOf(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  for (wchar_t& c : w)
    if (c == L'/') c = L'\\';
  return w;
}
}  // namespace

void showInExplorer(const std::string& path) {
  const std::wstring w = wideOf(path);
  PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(w.c_str());
  if (!pidl) return;
  (void)SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
  ILFree(pidl);
}

bool moveToRecycleBin(const std::string& path) {
  std::wstring from = wideOf(path);
  from.push_back(L'\0');  // a double-NUL terminated list
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

std::vector<std::string> filesOfDataObject(void* dataObject) {
  std::vector<std::string> out;
  auto* d = static_cast<IDataObject*>(dataObject);
  if (!d) return out;
  FORMATETC f{};
  f.cfFormat = CF_HDROP;
  f.dwAspect = DVASPECT_CONTENT;
  f.lindex = -1;
  f.tymed = TYMED_HGLOBAL;
  if (d->QueryGetData(&f) != S_OK) return out;
  STGMEDIUM m{};
  if (d->GetData(&f, &m) != S_OK) return out;
  if (auto drop = static_cast<HDROP>(GlobalLock(m.hGlobal))) {
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    out.reserve(n);
    std::wstring buf;
    for (UINT i = 0; i < n; ++i) {
      const UINT len = DragQueryFileW(drop, i, nullptr, 0);
      buf.assign(len + 1, L'\0');
      const UINT got = DragQueryFileW(drop, i, buf.data(), len + 1);
      if (got) out.push_back(utf8Of(buf.data(), static_cast<int>(got)));
    }
    GlobalUnlock(m.hGlobal);
  }
  ReleaseStgMedium(&m);
  return out;
}

struct OsDropTarget::Com final : IDropTarget {
  std::atomic<ULONG> refs{1};
  Sink sink;
  ToClient toClient;
  bool active = false;  // the current drag carries files (else every call answers "none")

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *out = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --refs;
    if (r == 0) delete this;
    return r;
  }

  OsDragEvent event(OsDragEvent::Type t, DWORD keys, POINTL pt) const {
    OsDragEvent e;
    e.type = t;
    e.mods = modsOf(keys);
    toClient(pt.x, pt.y, e.x, e.y);
    return e;
  }

  // IDropTarget
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
    if (!effect) return E_INVALIDARG;
    OsDragEvent e = event(OsDragEvent::Type::Enter, keys, pt);
    e.paths = filesOfDataObject(data);
    active = !e.paths.empty();
    *effect = active && sink ? effectOf(sink(e), *effect) : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override {
    if (!effect) return E_INVALIDARG;
    *effect = active && sink ? effectOf(sink(event(OsDragEvent::Type::Over, keys, pt)), *effect) : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override {
    if (active && sink) {
      OsDragEvent e;
      e.type = OsDragEvent::Type::Leave;
      (void)sink(e);
    }
    active = false;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
    if (!effect) return E_INVALIDARG;
    if (!active || !sink) {
      *effect = DROPEFFECT_NONE;
      return S_OK;
    }
    OsDragEvent e = event(OsDragEvent::Type::Drop, keys, pt);
    e.paths = filesOfDataObject(data);
    *effect = effectOf(sink(e), *effect);
    active = false;
    return S_OK;
  }
};

Result<std::unique_ptr<OsDropTarget>> OsDropTarget::registerOn(void* hwnd, Sink sink) {
  if (!hwnd) return Error{ErrorCode::Device, "no window to register a drop target on", {}};
  std::unique_ptr<OsDropTarget> t(new OsDropTarget());
  t->com_ = new Com();
  t->com_->sink = std::move(sink);
  HWND h = static_cast<HWND>(hwnd);
  t->com_->toClient = [h](int32_t sx, int32_t sy, float& x, float& y) {
    POINT p{sx, sy};
    ScreenToClient(h, &p);
    x = static_cast<float>(p.x);
    y = static_cast<float>(p.y);
  };
  const HRESULT hr = RegisterDragDrop(h, static_cast<IDropTarget*>(t->com_));
  if (FAILED(hr)) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "RegisterDragDrop failed (0x%08lx)", static_cast<unsigned long>(hr));
    return Error{ErrorCode::Device, msg, {}};
  }
  t->hwnd_ = hwnd;
  return t;
}

std::unique_ptr<OsDropTarget> OsDropTarget::unregistered(Sink sink, ToClient toClient) {
  std::unique_ptr<OsDropTarget> t(new OsDropTarget());
  t->com_ = new Com();
  t->com_->sink = std::move(sink);
  t->com_->toClient = std::move(toClient);
  return t;
}

OsDropTarget::~OsDropTarget() {
  if (hwnd_) RevokeDragDrop(static_cast<HWND>(hwnd_));  // releases OLE's references
  if (com_) {
    com_->sink = nullptr;  // a reference OLE still holds must not call back into a dead owner
    com_->Release();
  }
}

void* OsDropTarget::comTarget() const noexcept { return static_cast<IDropTarget*>(com_); }

}  // namespace pq::app
