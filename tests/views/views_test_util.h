#pragma once

// Shared helpers for the views tests: a scripted FileDialogs, a temporary project directory, a project with every
// kind of content built through the T3.6 edit operations.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/store.h"
#include "pq/views/view_context.h"

namespace pq::views::test {

// Dialogs that answer immediately with preset values (as if the user picked them).
struct ScriptedDialogs final : FileDialogs {
  std::string openPath, savePath, scalaPath;
  SaveChoice choice = SaveChoice::Discard;
  int opens = 0, saves = 0, asks = 0, scalas = 0;
  void chooseScalaFile(PathCallback done) override {
    ++scalas;
    done(scalaPath);
  }
  void chooseProjectToOpen(PathCallback done) override {
    ++opens;
    done(openPath);
  }
  void chooseProjectToSave(std::string_view, PathCallback done) override {
    ++saves;
    done(savePath);
  }
  SaveChoice askSaveChanges(std::string_view) override {
    ++asks;
    return choice;
  }
};

// A fresh empty directory under the system temp directory (removed first if it exists).
inline std::string tempDir(std::string_view name) {
  const std::filesystem::path p = std::filesystem::temp_directory_path() / ("pq_views_" + std::string(name));
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  std::filesystem::create_directories(p, ec);
  return p.string();
}

inline std::string fixtureDir(std::string_view name) { return std::string(PQ_TEST_DATA_DIR) + "/views/fixtures/" + std::string(name); }

// Two MIDI tracks (one inside a group), a clip with notes of several pitch kinds, a changed tempo, signature and loop,
// a locator, view state (fold / height). Every edit through the store (history entries).
inline void buildRichProject(doc::DocumentStore& store) {
  using namespace pq::doc;
  auto run = [&](const ops::EditResult& e) {
    REQUIRE(e.ok());
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  };
  const ops::Edit t1 = run(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Bass"));
  const ops::Edit t2 = run(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, t1.tracks[0], "Lead"));
  const ops::Edit g = run(ops::groupTracks(*store.current(), store.ids(), std::vector<TrackId>{t2.tracks[0]}, "Group"));
  const ops::Edit c = run(ops::insertEmptyClip(*store.current(), store.ids(), t1.tracks[0], 0, 2 * kTicksPerWhole, "Riff"));
  std::vector<Note> notes(3);
  notes[0].start = 0;
  notes[0].pitch = tuning::PitchId::fromDegree(7, 1);
  notes[1].start = kPpq;
  notes[1].pitch = tuning::PitchId::fromRatio(tuning::Rational(5, 4));
  notes[1].velocity = 64.5f;
  notes[2].start = 3 * kPpq;
  notes[2].pitch = tuning::PitchId::fromCents(-123.25);
  notes[2].chance = 0.5f;
  run(ops::addNotes(*store.current(), store.ids(), c.clips[0], notes));
  std::vector<TempoPoint> tempo{{0, 133.25, TempoCurve::Linear}, {4 * kTicksPerWhole, 90.0, TempoCurve::Step}};
  REQUIRE(store.dispatch(SetTempoMap{tempo}).ok());
  REQUIRE(store.dispatch(SetTimeSignatures{{{0, {6, 8}}, {8, {4, 4}}}}).ok());
  REQUIRE(store.dispatch(SetLoop{{kTicksPerWhole, 3 * kTicksPerWhole, true}}).ok());
  REQUIRE(store.dispatch(AddLocator{{store.ids().locator(), 2 * kTicksPerWhole, "Chorus"}}).ok());
  ops::setFolded(store.view(), g.tracks[0], true);
  store.view().tracks[t1.tracks[0]].height = 77;
}

}  // namespace pq::views::test
