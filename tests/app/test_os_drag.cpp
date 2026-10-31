// T5.12: the OLE drag source's data object (pq/app/os_drag.h) without a real drag: it offers CF_HDROP (QueryGetData,
// EnumFormatEtc) without producing the files, produces them once on the first GetData (lazy rendering of long clips),
// and the files read back through the same CF_HDROP reader the drop target uses; the drag-out temp folder is cleaned.

#include <doctest/doctest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pq/app/os_drag.h"
#include "pq/app/os_drop.h"

using namespace pq;
namespace fs = std::filesystem;

TEST_CASE("drag out: the data object offers CF_HDROP and produces its files on the first GetData only") {
  REQUIRE(SUCCEEDED(OleInitialize(nullptr)) );
  const std::vector<std::string> files = {"C:\\temp\\pq drag\\Kick 1.wav", "D:\\\xC3\xA9t\xC3\xA9\\B.wav"};
  int calls = 0;
  void* obj = app::makeFileDataObject([&] { return files; }, &calls);
  auto* d = static_cast<IDataObject*>(obj);
  FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  CHECK(d->QueryGetData(&f) == S_OK);
  FORMATETC text{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  CHECK(d->QueryGetData(&text) != S_OK);
  IEnumFORMATETC* en = nullptr;
  REQUIRE(d->EnumFormatEtc(DATADIR_GET, &en) == S_OK);
  FORMATETC got{};
  ULONG n = 0;
  REQUIRE(en->Next(1, &got, &n) == S_OK);
  CHECK(got.cfFormat == CF_HDROP);
  en->Release();
  CHECK(calls == 0);  // nothing produced yet
  // The drop target's reader gets the files (UTF-8 round trip), produced once.
  CHECK(app::filesOfDataObject(obj) == files);
  CHECK(app::filesOfDataObject(obj) == files);
  CHECK(calls == 1);
  app::releaseComObject(obj);
  // A provider without files: GetData fails (a refused drop).
  void* empty = app::makeFileDataObject([] { return std::vector<std::string>{}; });
  CHECK(app::filesOfDataObject(empty).empty());
  app::releaseComObject(empty);
  OleUninitialize();
}

TEST_CASE("drag out: the temp folder (PQ_DRAGOUT_DIR in tests) is created and cleaned") {
  const fs::path dir = fs::temp_directory_path() / "pq_dragout_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  _putenv_s("PQ_DRAGOUT_DIR", dir.string().c_str());
  CHECK(app::dragOutFolder() == dir);
  CHECK(fs::is_directory(dir));
  { std::ofstream(dir / "a.wav") << "x"; }
  fs::create_directories(dir / "sub");
  app::cleanDragOutFolder();
  CHECK(fs::is_empty(dir));
  _putenv_s("PQ_DRAGOUT_DIR", "");
  fs::remove_all(dir, ec);
}

// M5 review H9: a clip dragged out of the window (an OLE drag of our own data object) and back in. Our drop target
// used to read its CF_HDROP (forcing the lazy render of a long clip while hovering) and, on release, imported the
// drag-out temp WAV as a new clip, a file deleted at exit. It now refuses our own data object without producing it;
// a data object of another process (Explorer, another pqdaw) is unaffected.
TEST_CASE("drag out: our own drop target refuses our own drag (never renders it); other data objects still drop") {
  REQUIRE(SUCCEEDED(OleInitialize(nullptr)));
  int calls = 0;
  void* obj = app::makeFileDataObject([] { return std::vector<std::string>{"C:/temp/pqdaw/dragout/Clip.wav"}; }, &calls);
  CHECK(app::isOwnDragData(obj));
  int events = 0;
  auto t = app::OsDropTarget::unregistered([&](const app::OsDragEvent&) { ++events; return ui::DropEffect::Copy; },
                                           [](int32_t sx, int32_t sy, float& x, float& y) {
                                             x = static_cast<float>(sx);
                                             y = static_cast<float>(sy);
                                           });
  auto* target = static_cast<IDropTarget*>(t->comTarget());
  auto* data = static_cast<IDataObject*>(obj);
  DWORD fx = DROPEFFECT_COPY;
  REQUIRE(target->DragEnter(data, 0, POINTL{10, 10}, &fx) == S_OK);
  CHECK(fx == DROPEFFECT_NONE);
  fx = DROPEFFECT_COPY;
  REQUIRE(target->DragOver(0, POINTL{20, 20}, &fx) == S_OK);
  CHECK(fx == DROPEFFECT_NONE);
  fx = DROPEFFECT_COPY;
  REQUIRE(target->Drop(data, 0, POINTL{20, 20}, &fx) == S_OK);
  CHECK(fx == DROPEFFECT_NONE);
  CHECK(events == 0);
  CHECK(calls == 0);  // the files were never produced (no render)
  // Its files are still there for other targets (Explorer).
  CHECK(app::filesOfDataObject(obj).size() == 1);
  CHECK(calls == 1);
  app::releaseComObject(obj);
  t.reset();
  OleUninitialize();
}
