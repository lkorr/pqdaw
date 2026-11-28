#pragma once

// RT.3 (docs/plans/RT.md "The arrangement / mixer UI", "The circuit view"): Live's "Audio From" for an audio track — ONE
// menu builder shared by the mixer strip, the arrangement header and the circuit's Audio In terminal. Every choice is
// doc::ops::setAudioFrom (one undo step); every label is read back from the wire (doc::audioFromOf), so a cable drawn
// in the circuit reads here as "<Track> | <block>" and a choice here is a cable there (the isomorphism).
//
//   Audio From  Ext. In · Resampling · No Input · ─ · every other track (its colour as the chip) that the recording
//               track does not feed (doc::AudioInSources: refused entries are left out).
//   channel     for Ext. In the device's inputs (1, 2, …, 1/2, 3/4, …; "(not available)" past the open device's); for a
//               track its audio outputs in signal order: its source (an audio track's "Clips", a group's / return's
//               "Input"), each block of its main path (a rack: each chain as "<Rack> | <Chain>"), "Mixer" last (Live's
//               Post Mixer). Hidden for No Input and Resampling.
//   default     a track picked in Audio From starts at its last block before the mixer (Live's Post FX), else its
//               source, else its mixer.
//
// UI thread only.

#include <string>
#include <vector>

#include "pq/document/audio_in.h"
#include "pq/document/model.h"
#include "pq/ui/overlay.h"

namespace pq::views {

class ViewContext;

namespace audio_from {

// One audio output of a track that a cable onto an Audio In may start at.
struct Output {
  std::string label;  // "Clips", a block's name, "<Rack> | <Chain>", "Mixer"
  doc::Wire start;    // the cable's start (AudioFrom::output)
};

// `source`'s audio outputs that `allowed` takes, in signal order (above).
std::vector<Output> outputsOf(const ViewContext& ctx, const doc::AudioInSources& allowed, const doc::Track& source);
// The track a cable start belongs to (invalid: none).
doc::TrackId trackOf(const doc::Project& p, const doc::Wire& start) noexcept;
// The output's name on its track ("Mixer", "Clips", a block's, "<Rack> | <Chain>").
std::string outputName(const ViewContext& ctx, const doc::Wire& start);

// Chooser texts: Audio From ("No Input", "Ext. In", "Resampling", the source track's name) and the channel ("1/2", "3",
// an output's name; "" when hidden).
std::string fromLabel(const ViewContext& ctx, const doc::AudioFrom& from);
std::string channelLabel(const ViewContext& ctx, const doc::AudioFrom& from);
bool channelShown(const doc::AudioFrom& from) noexcept;
// One line: "No Input", "Ext. In 1/2", "Resampling", "<Track> | <output>".
std::string sourceLabel(const ViewContext& ctx, const doc::AudioFrom& from);
// Device channels as Live writes them: "1", "1/2".
std::string extChannels(const doc::AudioInput& in);

// The menus (each item one setAudioFrom; a refusal goes to the status line).
std::vector<ui::MenuItem> fromItems(ViewContext& ctx, doc::TrackId track);
std::vector<ui::MenuItem> channelItems(ViewContext& ctx, doc::TrackId track);
// The circuit terminal's menu: Audio From's entries, Ext. In and each track opening its channels as a submenu.
std::vector<ui::MenuItem> menu(ViewContext& ctx, doc::TrackId track);
// setAudioFrom through the store; false (the reason in the status line) when refused or nothing changed.
bool set(ViewContext& ctx, doc::TrackId track, const doc::AudioFrom& from);

}  // namespace audio_from
}  // namespace pq::views
