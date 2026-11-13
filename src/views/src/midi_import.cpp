// MF.2: MIDI files into the project (pq/views/midi_import.h).

#include "pq/views/midi_import.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#include "pq/document/commands.h"
#include "pq/document/midi_file.h"
#include "pq/document/midi_import.h"
#include "pq/views/view_context.h"

namespace pq::views {

namespace fs = std::filesystem;

namespace {

fs::path pathOf(std::string_view utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string utf8Of(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

Result<std::vector<uint8_t>> readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return Error{ErrorCode::NotFound, "cannot open the file", {}};
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) return Error{ErrorCode::Io, "read error", {}};
  return bytes;
}

std::string count(uint32_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

Result<MidiFilesImported> importMidiFiles(ViewContext& ctx, std::span<const std::string> paths, doc::TrackId target,
                                          Ticks at, uint64_t coalesceKey) {
  doc::IdSource& ids = ctx.store().ids();
  doc::Snapshot snap = ctx.doc();
  doc::Batch batch;
  doc::ChangeSet changes;
  MidiFilesImported out;
  doc::ops::MidiImportReport sum;
  std::string failed, lastName;
  uint32_t nFailed = 0, nFiles = 0;
  doc::TrackId after;  // the last track placed
  for (const std::string& path : paths) {
    const fs::path fp = pathOf(path);
    const std::string name = utf8Of(fp.stem());
    auto fail = [&](const std::string& why) {
      if (nFailed++ == 0) failed = utf8Of(fp.filename()) + " (" + why + ")";
    };
    auto bytes = readAll(fp);
    if (!bytes) {
      fail(bytes.error().message);
      continue;
    }
    auto file = doc::parseMidiFile(*bytes);
    if (!file) {
      fail(file.error().message);
      continue;
    }
    const doc::MidiNotes notes = doc::resolveNotes(*file);
    doc::ops::ImportMidiOptions opt;
    opt.format = file->format;
    opt.division = file->division;
    opt.target = after.valid() ? doc::TrackId{} : target;
    opt.newTracksAfter = after;
    opt.at = std::max<Ticks>(0, at);
    opt.name = name;
    doc::ops::MidiImportReport rep;
    auto e = doc::ops::importMidi(*snap, ids, notes, opt, &rep);
    if (!e) {
      fail(e.error().message);
      continue;
    }
    auto next = doc::apply(snap, e->command, changes);
    if (!next) {
      fail(next.error().message);
      continue;
    }
    snap = std::move(*next);
    for (doc::Command& c : std::get<doc::Batch>(e->command.v).commands) batch.commands.push_back(std::move(c));
    out.clips.insert(out.clips.end(), e->clips.begin(), e->clips.end());
    if (!e->tracks.empty()) after = e->tracks.back();
    ++nFiles;
    lastName = utf8Of(fp.filename());
    sum.tracks += rep.tracks;
    sum.notes += rep.notes;
    sum.tempoImported = sum.tempoImported || rep.tempoImported;
    sum.signaturesImported = sum.signaturesImported || rep.signaturesImported;
    sum.tempoKept = sum.tempoKept || rep.tempoKept;
    sum.movedSignatures += rep.movedSignatures;
    sum.skippedSignatures += rep.skippedSignatures;
    sum.unmappedKeys += rep.unmappedKeys;
    sum.droppedControllers += rep.droppedControllers;
    sum.droppedPressure += rep.droppedPressure;
    sum.midNoteBends += rep.midNoteBends;
  }
  const std::string couldNot =
      nFailed ? "could not import " + failed + (nFailed > 1 ? " and " + count(nFailed - 1, "more", "more") : "") : "";
  if (nFiles == 0) return Error{ErrorCode::InvalidArgument, couldNot.empty() ? "no MIDI files" : couldNot, {}};
  batch.label = nFiles == 1 ? "Import MIDI File" : "Import MIDI Files";
  if (Status s = coalesceKey ? ctx.store().dispatchCoalescing(batch, coalesceKey) : ctx.store().dispatch(batch); !s)
    return std::move(s).error();

  std::string& m = out.status;
  m = "Imported " + (nFiles == 1 ? lastName : count(nFiles, "MIDI file", "MIDI files")) + ": " +
      count(sum.tracks, "track", "tracks") + ", " + count(sum.notes, "note", "notes");
  if (sum.tempoImported || sum.signaturesImported)
    m += sum.tempoImported && sum.signaturesImported ? "; tempo and time signatures set"
         : sum.tempoImported                         ? "; tempo set"
                                                     : "; time signatures set";
  else if (sum.tempoKept)
    m += "; the file's tempo was not applied (the project has clips)";
  if (sum.movedSignatures) m += "; " + count(sum.movedSignatures, "time signature", "time signatures") + " moved to the next bar";
  if (sum.skippedSignatures) m += "; " + count(sum.skippedSignatures, "time signature", "time signatures") + " skipped";
  std::string dropped;
  auto add = [&](uint32_t n, const char* one, const char* many) {
    if (n) dropped += (dropped.empty() ? "" : ", ") + count(n, one, many);
  };
  add(sum.droppedControllers, "controller change", "controller changes");
  add(sum.midNoteBends, "mid-note pitch bend", "mid-note pitch bends");
  add(sum.droppedPressure, "pressure event", "pressure events");
  if (!dropped.empty()) m += "; dropped " + dropped;
  if (sum.unmappedKeys) m += "; " + count(sum.unmappedKeys, "key", "keys") + " outside the track's key map kept at 12-TET pitch";
  if (!couldNot.empty()) m += "; " + couldNot;
  return out;
}

}  // namespace pq::views
