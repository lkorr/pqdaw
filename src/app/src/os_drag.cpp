// T5.12: the OLE drag source (pq/app/os_drag.h).

#include "pq/app/os_drag.h"

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
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace pq::app {

namespace fs = std::filesystem;

namespace {

std::wstring wideOf(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  for (wchar_t& c : w)
    if (c == L'/') c = L'\\';
  return w;
}

// A DROPFILES block (wide names, double-NUL terminated) in a movable global.
HGLOBAL dropFiles(const std::vector<std::string>& files) {
  std::wstring list;
  for (const std::string& f : files) {
    list += wideOf(f);
    list.push_back(L'\0');
  }
  list.push_back(L'\0');
  const size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
  HGLOBAL g = GlobalAlloc(GHND, bytes);
  if (!g) return nullptr;
  auto* df = static_cast<DROPFILES*>(GlobalLock(g));
  df->pFiles = sizeof(DROPFILES);
  df->fWide = TRUE;
  std::memcpy(reinterpret_cast<char*>(df) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
  GlobalUnlock(g);
  return g;
}

bool isHdrop(const FORMATETC* f) {
  return f && f->cfFormat == CF_HDROP && (f->tymed & TYMED_HGLOBAL) && f->dwAspect == DVASPECT_CONTENT;
}

CLIPFORMAT ownFormat() {
  static const auto cf = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"pqdaw.DragOut"));
  return cf;
}

bool isOwn(const FORMATETC* f) {
  return f && ownFormat() != 0 && f->cfFormat == ownFormat() && (f->tymed & TYMED_HGLOBAL) && f->dwAspect == DVASPECT_CONTENT;
}

struct DataObject final : IDataObject {
  std::atomic<ULONG> refs{1};
  FileProvider provider;
  int* calls = nullptr;
  bool produced = false;
  std::vector<std::string> files;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *out = static_cast<IDataObject*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs;
    if (n == 0) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override {
    if (!m) return E_INVALIDARG;
    if (isOwn(f)) {  // the process id (never produces the files)
      HGLOBAL g = GlobalAlloc(GHND, sizeof(DWORD));
      if (!g) return E_OUTOFMEMORY;
      *static_cast<DWORD*>(GlobalLock(g)) = GetCurrentProcessId();
      GlobalUnlock(g);
      m->tymed = TYMED_HGLOBAL;
      m->hGlobal = g;
      m->pUnkForRelease = nullptr;
      return S_OK;
    }
    if (!isHdrop(f)) return DV_E_FORMATETC;
    if (!produced) {  // lazily: the first time a target wants the files
      produced = true;
      if (calls) ++*calls;
      if (provider) files = provider();
    }
    if (files.empty()) return E_FAIL;
    HGLOBAL g = dropFiles(files);
    if (!g) return E_OUTOFMEMORY;
    m->tymed = TYMED_HGLOBAL;
    m->hGlobal = g;
    m->pUnkForRelease = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override { return isHdrop(f) || isOwn(f) ? S_OK : DV_E_FORMATETC; }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
    if (out) out->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override {
    if (!out) return E_POINTER;
    if (dir != DATADIR_GET) return E_NOTIMPL;
    FORMATETC f[2] = {{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
                      {ownFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
    return SHCreateStdEnumFmtEtc(ownFormat() != 0 ? 2 : 1, f, out);
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

struct DropSource final : IDropSource {
  std::atomic<ULONG> refs{1};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropSource) {
      *out = static_cast<IDropSource*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs;
    if (n == 0) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
    if (escape) return DRAGDROP_S_CANCEL;
    if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

}  // namespace

void* makeFileDataObject(FileProvider provider, int* calls) {
  auto* d = new DataObject();
  d->provider = std::move(provider);
  d->calls = calls;
  return static_cast<IDataObject*>(d);
}

bool isOwnDragData(void* dataObject) {
  auto* d = static_cast<IDataObject*>(dataObject);
  if (!d || ownFormat() == 0) return false;
  FORMATETC f{ownFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (d->QueryGetData(&f) != S_OK) return false;
  STGMEDIUM m{};
  if (d->GetData(&f, &m) != S_OK) return false;
  bool own = false;
  if (m.tymed == TYMED_HGLOBAL && GlobalSize(m.hGlobal) >= sizeof(DWORD))
    if (const auto* pid = static_cast<const DWORD*>(GlobalLock(m.hGlobal))) {
      own = *pid == GetCurrentProcessId();
      GlobalUnlock(m.hGlobal);
    }
  ReleaseStgMedium(&m);
  return own;
}

void releaseComObject(void* object) {
  if (object) static_cast<IUnknown*>(static_cast<IDataObject*>(object))->Release();
}

ui::DropEffect dragFilesOut(FileProvider provider) {
  auto* data = static_cast<IDataObject*>(makeFileDataObject(std::move(provider)));
  auto* source = new DropSource();
  DWORD effect = DROPEFFECT_NONE;
  const HRESULT hr = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
  source->Release();
  data->Release();
  return hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE ? ui::DropEffect::Copy : ui::DropEffect::None;
}

fs::path dragOutFolder() {
  fs::path dir;
  if (const char* env = std::getenv("PQ_DRAGOUT_DIR"); env != nullptr && *env != '\0') {
    dir = fs::path(std::u8string(reinterpret_cast<const char8_t*>(env)));
  } else {
    std::error_code ec;
    dir = fs::temp_directory_path(ec) / "pqdaw" / "dragout";
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

void cleanDragOutFolder() {
  const fs::path dir = dragOutFolder();
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code rm;
    fs::remove_all(it->path(), rm);  // a file Explorer still copies stays until the next start
  }
}

}  // namespace pq::app
