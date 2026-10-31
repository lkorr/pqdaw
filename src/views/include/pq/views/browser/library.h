#pragma once

// The browser's data (T5.10; SPEC §9.1): places, the index, tags and the preview, as the views see them. The ViewContext
// owns one (ctx.library()).
//
//   places   the User Library, the current project's folder and the folders the user added (a drop of a folder on the
//            arrangement or the browser, the sidebar's "Add Folder…"). The added folders are the app's to persist
//            (setFoldersListener); the project place follows the context's project directory.
//   index    the app gives a library::Indexer (setIndexer: the live index, places pushed to it whenever they change;
//            the indexer's onChange is posted to the UI thread as indexChanged()); tests and scenes give a fixed
//            library::Index (setIndex). Either way index() is an immutable snapshot.
//   tags     a library::TagStore (in memory unless setTagFile names its tags.json: tests never touch the user's
//            %LOCALAPPDATA%); entryTags() is the search's view of it, rebuilt when the index or the tags change.
//   preview  the app's PreviewPort (the cue-bus player, pq/views/browser/preview_port.h); null in tests without one.
// Changes notify view_change::kLibrary.
//
// UI thread only.

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/library_files.h"
#include "pq/library/index.h"
#include "pq/library/search.h"
#include "pq/library/tags.h"
#include "pq/views/view_context.h"

namespace pq::library {
class Indexer;
}

namespace pq::views {

class ViewContext;
class PreviewPort;

// T5.11: what the browser hot-swaps (the device title bar's preset slot, a tuning chip): while set, the list shows only
// what fits (the device's presets, or the tunings) and selecting an item loads it in place (one undo step each).
struct HotSwapTarget {
  enum class Kind : uint8_t { None, Device, Tuning };
  Kind kind = Kind::None;
  doc::DeviceId device;     // Device
  std::string patch;        // Device: whose presets are listed
  TuningTarget tuning;      // Tuning
  std::string label;        // shown in the hot-swap bar ("Saturator", "Track “Bass”")
};


class Library {
public:
  explicit Library(ViewContext& ctx);
  ~Library();
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;

  // --- places ---
  // In sidebar order: the User Library (if set), the Samples folder (if a place of its own), the project (if titled),
  // then the added folders.
  const std::vector<library::PlaceSpec>& places() const noexcept { return places_; }
  void setUserLibrary(std::string rootUtf8);  // the app: Documents\pqdaw\User Library ("" = none)
  // Preferences → Library: moves the User Library place to another folder (the indexer re-indexes it; later saves go
  // there) and calls the user library listener (the app saves the choice). False when it is already there.
  bool changeUserLibrary(std::string_view rootUtf8);
  void setUserLibraryListener(std::function<void(const std::string&)> fn) { userLibraryListener_ = std::move(fn); }
  // T5.x-samples-folder: where new sample files go (Alt-dropped clips rendered to WAV, the samples of clips saved to the
  // User Library's Clips/). The folder set (setSamplesFolder / changeSamplesFolder), else <User Library>\Samples; ""
  // with neither. Outside the User Library and every other place it is a place of its own ("Samples", second).
  std::string samplesFolder() const;
  const std::string& samplesFolderSetting() const noexcept { return samplesFolder_; }  // "" = the default
  void setSamplesFolder(std::string rootUtf8);  // the app's choice at start ("" = the default; no listener call)
  // Preferences → Library: another Samples folder (re-indexed when it is a place) and the samples folder listener (the
  // app saves the choice). False when it is already the folder.
  bool changeSamplesFolder(std::string_view rootUtf8);
  void setSamplesFolderListener(std::function<void(const std::string&)> fn) { samplesFolderListener_ = std::move(fn); }
  // The added folders, as the app loaded them (no listener call).
  void setFolders(std::vector<std::string> rootsUtf8);
  const std::vector<std::string>& folders() const noexcept { return folders_; }
  // Adds a folder place (false when it is already a place: case-insensitive, separators normalised). Calls the folders
  // listener (the app saves).
  bool addPlace(std::string_view folderUtf8);
  bool removePlace(std::string_view folderUtf8);
  void setFoldersListener(std::function<void(const std::vector<std::string>&)> fn) { foldersListener_ = std::move(fn); }

  // --- index ---
  void setIndexer(library::Indexer* indexer);  // null: none (index() then returns the fixed one)
  library::Indexer* indexer() const noexcept { return indexer_; }
  void setIndex(std::shared_ptr<const library::Index> index);
  std::shared_ptr<const library::Index> index() const;
  // The indexer published (the app posts its onChange here). Notifies kLibrary.
  void indexChanged();
  // How many index publishes the views have seen (tests).
  uint64_t indexGeneration() const noexcept { return generation_; }

  // --- tags ---
  library::TagStore& tags() noexcept { return *tags_; }
  // Loads tags.json from `file` (missing = empty). Errors: Parse / Io (the store stays empty).
  Status setTagFile(const std::filesystem::path& file);
  // After an edit of tags(): saves (when a file is set), rebuilds the search's view, notifies kLibrary.
  void tagsChanged();
  std::shared_ptr<const library::EntryTags> entryTags();
  // The same for a given index snapshot (M5 review M15: index() may publish a newer one between two calls; a search
  // must pair the tags with the index it searches).
  std::shared_ptr<const library::EntryTags> entryTags(const std::shared_ptr<const library::Index>& idx);

  // --- preview ---
  void setPreview(PreviewPort* p) noexcept { preview_ = p; }
  PreviewPort* preview() const noexcept { return preview_; }

  // --- T5.11: the User Library's files (pq/document/library_files.h; layout in docs/plans/M5.md T5.11) ---
  //   Presets/<Device>/*.pqpreset   device presets          Defaults/<Device>.pqpreset   default presets
  //   Racks/*.pqpreset              rack presets            Devices/*.pqdevice           user devices
  //   Tunings/*.pqtuning            tunings                 Clips/*.pqclip (+ Clips/Samples/)   clips
  // <Device> is the device's display name ("EQ Eight"). Every save goes through doc::writeTextFileAtomic; the
  // indexer's watcher puts the file in the browser (≤ 500 ms). Errors: no User Library (tests set one in %TEMP%), I/O.
  const std::string& userLibrary() const noexcept { return userLibrary_; }
  // `sub` below the User Library (UTF-8, '\' separators); "" without one.
  std::string userPath(std::string_view sub) const;
  // A free file "<stem><ext>" in `dir` ("<stem> 2<ext>", … when taken; characters Windows refuses in names become '_').
  static std::string uniqueFile(std::string_view dir, std::string_view stem, std::string_view ext);
  // Device presets ("Save Preset": Presets/<deviceName>/<name or deviceName>.pqpreset; a rack: Racks/…).
  Result<std::string> savePreset(const doc::Device& d, std::string_view deviceName, std::string_view name = {});
  // "Save as Default Preset": Defaults/<deviceName>.pqpreset (replaced).
  Result<std::string> saveDefaultPreset(const doc::Device& d, std::string_view deviceName);
  // The default preset of a device (by display name) when it exists and is for `patch`; null otherwise. Cached by the
  // file's write time, so a new device costs one stat when nothing changed.
  doc::DevicePtr defaultPreset(std::string_view deviceName, std::string_view patch);
  // "Save as New Device": Devices/<name>.pqdevice.
  Result<std::string> saveUserDevice(const doc::UserDeviceFile& f);
  // "Save to Library" (a tuning): Tunings/<name>.pqtuning.
  Result<std::string> saveTuning(const doc::TuningFile& f);
  // A clip as `dir`/<name>.pqclip; an audio clip's sample is copied into `dir`/Samples/ (unless it already lies below
  // `dir`) and stored relative to `dir`. `sampleFile`: where the audio clip's sample is on this machine
  // (ViewContext::samplePath). `dir` "" = Clips/ of the User Library; there, with a Samples folder set
  // (samplesFolderSetting), the sample is copied into that folder instead (unless it already lies below it).
  Result<std::string> saveClip(const doc::Project& p, const doc::ClipSlot& c, std::string_view sampleFile = {},
                               std::string_view dir = {});
  // A library clip file read back: its clip, with the sample path resolved against the file's folder.
  Result<doc::ClipFile> readClip(std::string_view pathUtf8) const;
  // The user devices of the index (*.pqdevice in any place), parsed; re-read when the index changes.
  struct UserDeviceEntry {
    std::string path;
    doc::UserDeviceFile file;
  };
  const std::vector<UserDeviceEntry>& userDevices();
  // The patch a preset file is for: its device's "patch" in the first 4 KiB ("" unreadable). Cached by path + write time.
  std::string presetPatch(std::string_view pathUtf8);
  // The browser renames this file inline as soon as the index shows it (a preset just saved; the panel clears it).
  std::string pendingRename;
  // T5.12: renders an audio clip as it plays (gain, fades, transpose, reverse; no track devices) to a WAV file in
  // `folder` and returns its path: the app's offline clip render (null in tests and scenes without one).
  using ClipRenderer = std::function<Result<std::string>(const doc::ClipRef& clip, std::string_view folder)>;
  void setClipRenderer(ClipRenderer r) { clipRenderer_ = std::move(r); }
  const ClipRenderer& clipRenderer() const noexcept { return clipRenderer_; }
  // T5.11: hot-swap requests (the Device view, tuning chips) go to the browser panel (installBrowser sets the handler).
  // False when there is no browser.
  void setHotSwapHandler(std::function<void(const HotSwapTarget&)> fn) { hotSwapHandler_ = std::move(fn); }
  bool requestHotSwap(const HotSwapTarget& t) {
    if (!hotSwapHandler_) return false;
    hotSwapHandler_(t);
    return true;
  }
  // The preset a device last loaded from the library (view state for the title bar's preset slot; "" = none).
  const std::string& presetName(doc::DeviceId d) const;
  void setPresetName(doc::DeviceId d, std::string name);

private:
  void rebuildPlaces();
  struct CachedPreset {
    int64_t mtime = 0;
    doc::DevicePtr device;
    std::string patch;
  };
  std::map<std::string, CachedPreset, std::less<>> presetCache_;  // by folded path
  std::vector<UserDeviceEntry> userDevices_;
  std::shared_ptr<const library::Index> userDevicesFor_;
  ClipRenderer clipRenderer_;
  std::function<void(const HotSwapTarget&)> hotSwapHandler_;
  std::map<uint32_t, std::string> presetNames_;

  ViewContext& ctx_;
  std::string userLibrary_;
  std::string project_;
  std::vector<std::string> folders_;
  std::vector<library::PlaceSpec> places_;
  std::function<void(const std::vector<std::string>&)> foldersListener_;
  std::function<void(const std::string&)> userLibraryListener_;
  std::string samplesFolder_;
  std::function<void(const std::string&)> samplesFolderListener_;
  library::Indexer* indexer_ = nullptr;
  std::shared_ptr<const library::Index> fixed_;
  uint64_t generation_ = 0;
  std::unique_ptr<library::TagStore> tags_;
  std::shared_ptr<const library::EntryTags> entryTags_;
  std::shared_ptr<const library::Index> tagsFor_;  // the index entryTags_ was built for
  uint64_t tagsVersion_ = ~0ull;
  PreviewPort* preview_ = nullptr;
  ViewContext::Subscription projectSub_;
};

// Folded comparison key of a place root ('/' → '\\', lower case, no trailing separator).
std::string placeKey(std::string_view root);

}  // namespace pq::views
