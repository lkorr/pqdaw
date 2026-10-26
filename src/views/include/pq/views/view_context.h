#pragma once

// ViewContext (T3.7): the controller every view works through. It owns the DocumentStore (the project), routes
// global actions (undo/redo, new/open/save, transport, detail view), keeps the view state that is not part of the
// document (start marker, grid, which panels are shown) and tells views when that state changes.
//
//   ViewContext ctx(transport);           // a new default project
//   ui::UiTree tree; ...
//   Shell shell(tree, ctx);               // the main window layout (pq/views/shell/shell.h)
//
// Views read the document from ctx.doc() / ctx.store(), edit it only through commands (pq::doc::ops builders,
// store().dispatch or begin/commitGesture for drags: one undo step per gesture), subscribe to document changes with
// ctx.subscribe(...) and to view-state changes with ctx.observe(...). Both return RAII handles that unsubscribe.
//
// Actions: views register their actions in ctx.actions() (string id → handler, before the keymap is applied) and
// set a keymap scope (ctx.scope("arrangement")) on their root widget. The keymap file (assets/keymaps/ableton.json)
// binds chords to action ids; a binding marked "implemented" whose action is not registered fails applyKeymap.
//
// Project I/O: newProject / openProject / saveProject / saveProjectAs take explicit directories (tests, CLI); the
// file.* actions ask the FileDialogs the app installs (SDL dialogs; asynchronous, the callback runs on the UI
// thread). Without dialogs, file.open / file.saveAs are disabled and file.save of an untitled project reports an
// error in the status line.
//
// UI thread only. Nothing here is RT-safe.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pq/core/grid.h"
#include "pq/core/result.h"
#include "pq/document/model.h"
#include "pq/document/store.h"
#include "pq/ui/actions.h"
#include "pq/ui/keymap_file.h"
#include "pq/views/transport_port.h"

namespace pq::views {

class AudioPrefsPort;  // pq/views/prefs/audio_prefs.h
}  // namespace pq::views
namespace pq::doc {
class DeviceLibrary;  // pq/document/devices.h
}  // namespace pq::doc
namespace pq::views {

// Bits passed to ctx.observe() callbacks: which part of the view state changed.
namespace view_change {
inline constexpr uint32_t kTransport = 1u << 0;  // play state requested, start marker moved
inline constexpr uint32_t kGrid = 1u << 1;       // grid spec or the focused editor's resolved division
inline constexpr uint32_t kLayout = 1u << 2;     // detail view shown/hidden or switched clip ↔ device, browser
inline constexpr uint32_t kProject = 1u << 3;    // project directory, name or unsaved-changes state
inline constexpr uint32_t kStatus = 1u << 4;     // status line text
inline constexpr uint32_t kTool = 1u << 5;       // draw mode (B) toggled (T3.11)
inline constexpr uint32_t kDetailClip = 1u << 6; // the clip shown in the clip view changed (T3.11)
inline constexpr uint32_t kTuningPreview = 1u << 7;  // the MOS wheel's live tuning preview changed (T3.14)
inline constexpr uint32_t kAutomation = 1u << 8;     // automation overridden / re-enabled (T3.15)
inline constexpr uint32_t kSelection = 1u << 9;      // the selected track changed (T3.x-fix-look: the control bar's chip)
inline constexpr uint32_t kInfo = 1u << 10;          // the hovered widget's info text changed in place (T3.19: the roll's pitch readout)
inline constexpr uint32_t kDevices = 1u << 11;       // the device library changed (T4.1: the device menu, panels)
}  // namespace view_change

// What a tuning edit applies to (T3.14): one clip, one track, or a main tuning. The MOS wheel popover commits its
// tuning to it as one command (SetClipTuning / SetTrackTuning with a private tuning, or SetTuningDef).
struct TuningTarget {
  enum class Kind : uint8_t { Clip, Track, Main };
  Kind kind = Kind::Main;
  doc::ClipRef clip{};   // Kind::Clip
  doc::TrackId track{};  // Kind::Track (and the clip's track for Kind::Clip)
  doc::TuningId main{};  // Kind::Main
  static TuningTarget forClip(doc::ClipRef c) { return {Kind::Clip, c, c.track, {}}; }
  static TuningTarget forTrack(doc::TrackId t) { return {Kind::Track, {}, t, {}}; }
  static TuningTarget forMain(doc::TuningId id) { return {Kind::Main, {}, {}, id}; }
};

// A tuning shown in place of the target's while the MOS wheel is open (not part of the document, no history).
struct TuningPreview {
  TuningTarget target;
  doc::TuningDef def;
};

enum class DetailMode : uint8_t { Clip, Device };

// Asked before discarding unsaved changes (new / open / quit).
enum class SaveChoice : uint8_t { Save, Discard, Cancel };

// Platform dialogs (the app implements them with SDL3's file dialogs and message boxes). Callbacks must be invoked
// on the UI thread (the SDL implementation marshals them through the event queue). An empty path = cancelled.
class FileDialogs {
public:
  using PathCallback = std::function<void(std::string pathUtf8)>;
  virtual ~FileDialogs() = default;
  // A project to open: the path of its project.pqd or its directory (see projectDirFromPickedPath).
  virtual void chooseProjectToOpen(PathCallback done) = 0;
  // Where to save: a new project name/directory (see projectDirFromPickedPath).
  virtual void chooseProjectToSave(std::string_view suggestedName, PathCallback done) = 0;
  // Unsaved changes in `projectName`: save them first, discard them, or cancel the operation. Synchronous.
  virtual SaveChoice askSaveChanges(std::string_view projectName) = 0;
  // A Scala scale file (.scl) to import as a tuning (T3.14). The default implementation cancels (no dialogs).
  virtual void chooseScalaFile(PathCallback done) { done(std::string()); }
  // A patch file (.pqp) to open in the circuit view (T4.2). The default implementation cancels (no dialogs).
  virtual void choosePatchFile(PathCallback done) { done(std::string()); }
};

// What a file dialog returned → the project directory: "…/X/project.pqd" → "…/X"; "…/Song.pqd" → "…/Song" (a save
// dialog with a .pqd filter appends the extension to a new name); anything else is the directory itself. Trailing
// separators are dropped. Pure string work, '/' and '\\' both accepted.
std::string projectDirFromPickedPath(std::string_view picked);
// The display name of a project directory: its last path component ("Untitled" for an empty path).
std::string projectNameOf(std::string_view dirUtf8);

class ViewContext {
public:
  // A new default project (doc::makeDefaultProject). `transport` must outlive the context.
  explicit ViewContext(TransportPort& transport);
  ~ViewContext();
  ViewContext(const ViewContext&) = delete;
  ViewContext& operator=(const ViewContext&) = delete;

  // --- document ---
  doc::DocumentStore& store() noexcept { return store_; }
  const doc::DocumentStore& store() const noexcept { return store_; }
  const doc::Snapshot& doc() const noexcept { return store_.current(); }

  // Document change notifications (StoreEvent: Dispatch / Undo / Redo / Gesture* / Reset). The context's own
  // listener runs first, so dirty() and the transport loop are current when views are called.
  class Subscription {
  public:
    Subscription() = default;
    Subscription(Subscription&& o) noexcept : fn_(std::exchange(o.fn_, nullptr)) {}
    Subscription& operator=(Subscription&& o) noexcept {
      if (this != &o) {
        reset();
        fn_ = std::exchange(o.fn_, nullptr);
      }
      return *this;
    }
    ~Subscription() { reset(); }
    void reset() noexcept {
      if (fn_) fn_();
      fn_ = nullptr;
    }

  private:
    friend class ViewContext;
    explicit Subscription(std::function<void()> unsubscribe) : fn_(std::move(unsubscribe)) {}
    std::function<void()> fn_;
  };
  [[nodiscard]] Subscription subscribe(doc::Listener listener);
  // View-state notifications: `fn(bits)` with view_change bits.
  [[nodiscard]] Subscription observe(std::function<void(uint32_t)> fn);

  // --- project I/O (explicit paths) ---
  // New default project; history cleared; the project becomes untitled. Stops the transport.
  void newProject();
  // Loads `dirUtf8` (doc::openProject: history cleared, view state from the file). On failure nothing changes and
  // the error is returned (and shown in the status line). Stops the transport.
  Status openProject(std::string_view dirUtf8);
  // Saves to projectDir(). Fails for an untitled project (use saveProjectAs).
  Status saveProject();
  // Saves to `dirUtf8` (created if needed); the project is then titled `dirUtf8`.
  Status saveProjectAs(std::string_view dirUtf8);
  const std::string& projectDir() const noexcept { return projectDir_; }
  std::string projectName() const { return projectNameOf(projectDir_); }
  // The document differs from the last saved/loaded one (pointer comparison: undoing back to it is clean again), or
  // view state that the project file keeps (folds, track and lane heights, lane visibility) changed since.
  bool dirty() const noexcept { return store_.current() != saved_ || viewStateDirty_; }
  // Views call this after changing the store's ViewState (it is saved with the project but not undoable, so the
  // snapshot comparison cannot see it: T3.x-fix-edit, M3 review L19).
  void viewStateChanged();

  // --- dialogs (the action layer) ---
  void setFileDialogs(FileDialogs* d) noexcept { dialogs_ = d; }
  FileDialogs* fileDialogs() const noexcept { return dialogs_; }
  // T3.18: the app's audio device manager, for the preferences window (Ctrl+,); null in tests and scenes without one.
  void setAudioPrefs(AudioPrefsPort* p) noexcept { audioPrefs_ = p; }
  AudioPrefsPort* audioPrefs() const noexcept { return audioPrefs_; }
  // T4.1: the device library (pq/document/devices.h: the catalog the device view's menu and generic panels are built
  // from). The app sets the playback link's; null in bare tests and scenes without one (the device view then shows
  // chains without param controls and offers nothing to add). Must outlive its use. Notifies kDevices.
  void setDeviceLibrary(const doc::DeviceLibrary* lib);
  const doc::DeviceLibrary* deviceLibrary() const noexcept { return devices_; }
  // What file.new / file.open / file.save / file.saveAs do: ask about unsaved changes, pick a path, then call the
  // explicit-path functions above. `then` runs after a successful operation (e.g. quit after "save and quit").
  void requestNew();
  void requestOpen();
  void requestSave(std::function<void()> then = {});
  void requestSaveAs(std::function<void()> then = {});
  // Before quitting: true = go ahead (saved, discarded or nothing to save). With unsaved changes in an untitled
  // project and "Save", the save-as dialog runs and `quitAfterSave` is called once it succeeds; returns false.
  bool confirmQuit(std::function<void()> quitAfterSave);

  // --- undo ---
  bool undo();  // store().undo(); false when there is nothing to undo or a gesture is running
  bool redo();

  // --- transport ---
  TransportPort& transport() noexcept { return transport_; }
  bool playing() { return transport_.playhead().playing; }
  // Space: stop if playing, else play from the start marker. Shift+Space: continue from the stop position.
  void togglePlay();
  void continuePlayback();
  // The stop button: stops; when already stopped, returns the start marker to the beginning (Live).
  void stopButton();
  Ticks startMarker() const noexcept { return startMarker_; }
  void setStartMarker(Ticks t);

  // --- grid (shared by the arrangement and the piano roll; Ctrl+1–5 and the grid menu edit it, T3.9/T3.11) ---
  const GridSpec& grid() const noexcept { return grid_; }
  void setGrid(const GridSpec& g);
  // The focused editor reports the division its adaptive grid resolves to at its zoom (nullopt: no editor).
  void setResolvedGrid(std::optional<GridDivision> d);
  const std::optional<GridDivision>& resolvedGrid() const noexcept { return resolvedGrid_; }
  // Control-bar text: the resolved division ("1/16", "1/8T"), else the spec ("Fixed 1/8", "Adaptive (Medium)"),
  // "Off" for an off grid; " · Snap off" appended when snapping is off.
  std::string gridLabel() const;

  // --- editing tools (T3.11) ---
  // Draw mode (B): shared by the clip view and the arrangement, like Live's.
  bool drawMode() const noexcept { return drawMode_; }
  void setDrawMode(bool on);

  // --- the clip in the clip view (T3.11) ---
  // The note clip the detail view's clip view edits (an invalid ClipRef: none). The arrangement (T3.8) sets it when
  // a clip is selected; the clip view shows "No clip selected" when it is unset or the clip no longer exists.
  const doc::ClipRef& detailClip() const noexcept { return detailClip_; }
  // Setting a clip opens the detail view the first time (it starts collapsed until a clip is opened: T3.x-fix-look,
  // the M3 review's UI §5); after the user has shown or hidden it (Ctrl+Alt+L, Shift+Tab, the status bar) their
  // choice stands.
  void setDetailClip(const doc::ClipRef& clip);
  // The track the arrangement has selected (its first selected track; invalid: none). The control bar shows its tuning.
  doc::TrackId selectedTrack() const noexcept { return selectedTrack_; }
  void setSelectedTrack(doc::TrackId t);

  // --- tuning preview (T3.14) ---
  // The MOS wheel shows its tuning live while it is open: views that resolve a clip's tuning use previewedTuning()
  // so the target (and everything inheriting from it) draws in the previewed tuning. Notifies kTuningPreview.
  // Cleared by clearTuningPreview, newProject and openProject.
  void setTuningPreview(const TuningTarget& target, doc::TuningDef def);
  void clearTuningPreview();
  const std::optional<TuningPreview>& tuningPreview() const noexcept { return preview_; }
  // doc::effectiveTuning with the preview applied, for clip `clip` (id `clipId`; nullptr = the track's own tuning)
  // on the track at `path`. Without a preview this is exactly doc::effectiveTuning.
  const doc::TuningDef* previewedTuning(const doc::Project& p, const doc::TrackPath& path, const doc::NoteClip* clip,
                                        doc::ClipId clipId) const noexcept;

  // --- automation override (T3.15, SPEC §9.6) ---
  // A parameter with an automation lane that is changed by hand (a mixer drag, a type-in, the activator) is
  // overridden: its automation is off until "Re-enable Automation" (Live). The context marks it from the document's
  // Param changes; state only until T3.17 lowers it. Cleared by reenableAutomation, newProject and openProject;
  // an override whose lane is gone is dropped. Notifies kAutomation.
  bool automationOverridden(const doc::ParamRef& p) const noexcept;
  bool anyAutomationOverridden() const noexcept { return !overrides_.empty(); }
  std::span<const doc::ParamRef> automationOverrides() const noexcept { return overrides_; }
  void overrideAutomation(const doc::ParamRef& p);
  void reenableAutomation();

  // --- layout state ---
  bool detailVisible() const noexcept { return detailVisible_; }
  void setDetailVisible(bool v);
  DetailMode detailMode() const noexcept { return detailMode_; }
  void setDetailMode(DetailMode m);
  // Shift+Tab: switch clip ↔ device (and show the detail view if hidden).
  void toggleClipDevice();
  bool browserVisible() const noexcept { return browserVisible_; }
  void setBrowserVisible(bool v);
  // The Tunings panel (T3.19, SPEC §3.2; View > Tunings): hidden at start. Notifies kLayout.
  bool tuningsVisible() const noexcept { return tuningsVisible_; }
  void setTuningsVisible(bool v);

  // --- info view (T3.19) ---
  // A widget whose info() text changes while it stays hovered (the piano roll's pitch readout under the pointer) calls
  // this; the info view re-reads infoFor(hovered). Notifies kInfo.
  void infoChanged() { notify(view_change::kInfo); }

  // --- status line ---
  void setStatus(std::string text);
  const std::string& status() const noexcept { return status_; }

  // --- actions and keymap ---
  ui::ActionRegistry& actions() noexcept { return actions_; }
  const ui::ActionRegistry& actions() const noexcept { return actions_; }
  const ui::Keymap& keymap() const noexcept { return keymap_; }
  // A keymap scope id ("arrangement", "pianoroll", …): stable for the context's lifetime, also across applyKeymap.
  ui::ScopeId scope(std::string_view name);
  // Rebinds the keymap from `file` (every "implemented" action must be registered by now). Replaces any previous
  // bindings, so it can be called again after more views registered their actions.
  Status applyKeymap(const ui::KeymapFile& file);
  // applyKeymap(assets/keymaps/ableton.json).
  Status applyDefaultKeymap();
  // View actions (T3.11): the context registers every id in kPianoRollActions up front with a forwarding handler,
  // so the keymap binds them whether or not the view exists (a bare context or shell, tests); while no view has
  // installed a handler the action is disabled and its chord falls through (Q27). A view installs its handlers
  // when it is created and clears them when it is destroyed. Unknown ids are ignored (asserted in dev builds).
  void setViewAction(std::string_view id, std::function<void()> handler, std::function<bool()> enabled = {});
  void clearViewAction(std::string_view id);
  static constexpr std::string_view kPianoRollActions[] = {
      "pianoroll.selectAll", "pianoroll.copy",      "pianoroll.cut",       "pianoroll.paste",
      "pianoroll.duplicate", "pianoroll.delete",    "pianoroll.quantize",  "pianoroll.stepUp",
      "pianoroll.stepDown",  "pianoroll.equaveUp",  "pianoroll.equaveDown", "pianoroll.toggleActive",
      "pianoroll.fold",      "pianoroll.toggleScale", "pianoroll.nudgeLeft", "pianoroll.nudgeRight"};
  // View actions of the MOS wheel (T3.14): installMosWheel sets the handler (Ctrl+Shift+W, Q15).
  static constexpr std::string_view kMosWheelActions[] = {"tuning.openMosWheel"};
  // T3.10: the mixer view's toggle (Ctrl+Alt+M), forwarded the same way (installMixer sets the handler).
  static constexpr std::string_view kMixerActions[] = {"view.toggleMixer"};
  // T3.18: the preferences window (Ctrl+, as in Live), forwarded the same way (installPreferences sets the handler).
  static constexpr std::string_view kPrefsActions[] = {"app.preferences"};
  // T3.19: Create > New Main Tuning (installTuningsPanel sets the handler: it shows the panel with the new tuning).
  static constexpr std::string_view kTuningsActions[] = {"tuning.newMain"};
  // T4.2: View > Open Patch in Circuit View… (installCircuitViewer sets the handler: file dialog → circuit viewer).
  static constexpr std::string_view kCircuitActions[] = {"view.openPatch"};
  // T4.6: View > Open Device… (installDeviceView sets the handler: a menu of the stock devices → the Device tab).
  static constexpr std::string_view kDeviceActions[] = {"view.openDevice"};
  // T4.1: the Device view's edits (installDeviceChain sets the handlers; scope "devices").
  static constexpr std::string_view kDeviceChainActions[] = {"device.delete", "device.copy",      "device.cut",
                                                             "device.paste",  "device.duplicate", "device.toggleActive"};
  // Ids of the global actions this context registers (also in docs/HOTKEYS.md).
  static constexpr std::string_view kGlobalActions[] = {
      "transport.playStop", "transport.continue", "edit.undo",   "edit.redo",          "file.new",
      "file.open",          "file.save",          "file.saveAs", "view.toggleClipDevice", "view.toggleDetail",
      "edit.toggleDrawMode", "grid.narrow",        "grid.widen",  "grid.toggleTriplet", "grid.toggleSnap",
      "grid.toggleFixed",   "view.toggleTunings"};

private:
  void notify(uint32_t bits);
  void onStoreEvent(const doc::StoreEvent& e);
  void syncLoop();
  void afterLoad();
  void registerGlobalActions();
  void declareViewActions(std::span<const std::string_view> ids);
  // Asks about unsaved changes; runs `proceed` when it may go ahead (possibly after an asynchronous save-as).
  void guardUnsaved(std::function<void()> proceed);

  TransportPort& transport_;
  doc::DocumentStore store_;
  doc::Snapshot saved_;  // the snapshot last saved or loaded (dirty() compares pointers)
  std::string projectDir_;
  bool lastDirty_ = false;
  bool viewStateDirty_ = false;  // ViewState edited since the last save / load
  doc::ListenerId storeListener_ = 0;

  FileDialogs* dialogs_ = nullptr;
  AudioPrefsPort* audioPrefs_ = nullptr;
  const doc::DeviceLibrary* devices_ = nullptr;
  Ticks startMarker_ = 0;
  GridSpec grid_{};
  std::optional<GridDivision> resolvedGrid_;
  bool detailVisible_ = false;  // T3.x-fix-look: collapsed until a clip is shown in it (Live)
  bool detailAuto_ = true;       // no explicit show / hide yet: the first detail clip opens the detail view
  doc::TrackId selectedTrack_{};
  DetailMode detailMode_ = DetailMode::Clip;
  bool browserVisible_ = false;
  bool tuningsVisible_ = false;
  bool drawMode_ = false;
  doc::ClipRef detailClip_{};
  std::optional<TuningPreview> preview_;
  std::vector<doc::ParamRef> overrides_;  // sorted (T3.15)
  std::string status_;
  TransportLoop loopSent_{};
  bool loopSentValid_ = false;

  ui::ActionRegistry actions_;
  ui::Keymap keymap_;
  struct ViewAction {
    std::string id;
    std::function<void()> handler;
    std::function<bool()> enabled;
  };
  std::vector<ViewAction> viewActions_;  // filled in the constructor only: indices are stable
  std::vector<std::string> scopes_;  // interned in this order (stable ScopeIds across applyKeymap)

  uint32_t nextObserver_ = 1;
  int notifyDepth_ = 0;
  std::vector<std::pair<uint32_t, std::function<void(uint32_t)>>> observers_;
};

}  // namespace pq::views
