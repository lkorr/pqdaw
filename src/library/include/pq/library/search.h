#pragma once

// Browser search (T5.9; SPEC §9.1: instant, incremental, fuzzy over names and tags).
//
// A query's text is split at spaces into terms; an entry matches when every term is a subsequence (ASCII case-folded)
// of the entry's name stem or of one of its free tags. A term's score is its best alignment in the stem (or in a tag,
// minus kScoreTagPenalty, whichever is higher):
//
//   score(p_0 < … < p_{m−1}) = Σ_i (kScoreMatch + bonus(p_i)) + Σ_{i≥1} (p_i = p_{i−1} + 1 ? kScoreConsecutive
//                                                                                  : −kScoreGap · (p_i − p_{i−1} − 1))
//   bonus(j) = kScoreNameStart for j = 0, kScoreWordStart at a word start (after a space or one of _-.,+&()[]{}/;
//              a lower → upper case step; a letter ↔ digit step), else 0
//
// and the entry's score is the sum over its terms minus kScoreDepth per directory level below the place root. Results
// are ordered by score (descending), then stem length (ascending; with an empty text this key is skipped), then index
// order (= path order). A dynamic program finds each term's best alignment in O(m · n) (a brute-force enumeration of
// every alignment gives the same scores; tests check this).
//
// Cost: a 64-bit character-set prefilter per entry (Index::Entries::mask, the tag combination's mask) rejects most
// entries with one AND; a greedy subsequence pass rejects most of the rest before the dynamic program runs. A query
// whose text extends the previous query's text (same index, tags and filter) searches only the previous matches.
// Only the top kMaxResults hits are sorted and materialised.
//
// Not RT code. search() is pure and may run on any thread; Searcher runs it on one worker thread where a newer
// submit() cancels the query in flight.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pq/library/index.h"
#include "pq/library/tags.h"

namespace pq::library {

inline constexpr int32_t kScoreMatch = 16;
inline constexpr int32_t kScoreNameStart = 40;
inline constexpr int32_t kScoreWordStart = 24;
inline constexpr int32_t kScoreConsecutive = 16;
inline constexpr int32_t kScoreGap = 1;
inline constexpr int32_t kScoreDepth = 4;
inline constexpr int32_t kScoreTagPenalty = 32;
inline constexpr uint32_t kMaxResults = 1000;
inline constexpr int32_t kNoMatch = std::numeric_limits<int32_t>::min();

// Word-start test of position j in s (raw case: the case step needs it).
bool isWordStart(std::string_view s, std::size_t j) noexcept;
// Best alignment score of `foldedTerm` (non-empty, already folded) within `s`, or kNoMatch. `scratch` is reused
// between calls (grown as needed).
int32_t fuzzyScore(std::string_view foldedTerm, std::string_view s, std::vector<int32_t>& scratch);

// Tags as the search reads them: each entry's tag combination (collections + free tags), deduplicated.
struct EntryTags {
  struct Combo {
    uint8_t collections = 0;
    std::vector<uint32_t> tags;  // indices into tagNames, sorted
    uint64_t mask = 0;           // charMask of all its tags
  };
  std::vector<uint32_t> combo;       // per entry; 0 = no tags (combos[0] is empty)
  std::vector<Combo> combos;
  std::vector<std::string> tagNames;       // as stored
  std::vector<std::string> foldedTagNames; // case-folded
  uint32_t findTag(std::string_view name) const noexcept;  // case-insensitive; kNone when absent
  static constexpr uint32_t kNone = 0xFFFFFFFFu;
};
std::shared_ptr<const EntryTags> buildEntryTags(const Index& index, const TagStore& store);

struct SearchFilter {
  uint32_t kinds = ~0u;        // bit per ItemKind
  uint32_t categories = ~0u;   // bit per Category
  uint64_t places = ~0ull;     // bit per place index (places ≥ 64 always pass)
  double minSeconds = 0;       // duration bounds; when set (min > 0 or max < ∞) entries without a duration fail
  double maxSeconds = std::numeric_limits<double>::infinity();
  float minBpm = 0;            // BPM bounds; when set, entries without a BPM fail
  float maxBpm = std::numeric_limits<float>::infinity();
  std::vector<uint32_t> rates; // allowed sample rates (empty = any)
  uint16_t channels = 0;       // 0 = any
  uint8_t collections = 0;     // entry must be in one of these (0 = no constraint)
  std::vector<std::string> tags;  // entry must have all of these free tags (case-insensitive)
  friend bool operator==(const SearchFilter&, const SearchFilter&) = default;
};

struct SearchQuery {
  std::string text;
  SearchFilter filter;
};

struct SearchHit {
  uint32_t entry = 0;
  int32_t score = 0;
  friend bool operator==(const SearchHit&, const SearchHit&) = default;
};

struct SearchResult {
  uint64_t ticket = 0;
  bool cancelled = false;
  std::vector<SearchHit> hits;  // best first, at most kMaxResults
  uint32_t total = 0;           // all matches
  // What the next keystroke narrows from.
  std::shared_ptr<const Index> index;
  std::shared_ptr<const EntryTags> tags;
  SearchQuery query;
  std::vector<uint32_t> matches;  // every matching entry, in index order
  bool narrowed = false;          // this result was computed from the previous one's matches
};

// Runs a query. `tags` may be null (no tags: tag terms, collection and tag filters match nothing). `previous`
// (optional) is narrowed from when its index, tags and filter are the same and the new text starts with its text.
// When `latestTicket` is given, the search stops early (cancelled = true) once *latestTicket != ticket.
SearchResult search(const std::shared_ptr<const Index>& index, const std::shared_ptr<const EntryTags>& tags,
                    const SearchQuery& query, const SearchResult* previous = nullptr,
                    const std::atomic<uint64_t>* latestTicket = nullptr, uint64_t ticket = 0);

class Searcher {
public:
  Searcher();
  ~Searcher();
  Searcher(const Searcher&) = delete;
  Searcher& operator=(const Searcher&) = delete;

  // Queues a query (replacing a queued one, cancelling one in flight); returns its ticket (1, 2, …).
  uint64_t submit(std::shared_ptr<const Index> index, std::shared_ptr<const EntryTags> tags, SearchQuery query);
  // The newest finished result (never a cancelled one), or null.
  std::shared_ptr<const SearchResult> latest() const;
  // Waits until the result of `ticket` (or a newer one) is finished; null on timeout.
  std::shared_ptr<const SearchResult> waitFor(uint64_t ticket, std::chrono::milliseconds timeout) const;
  // Called on the worker after each finished result (the app posts a UI event). Set before submitting.
  void setOnResult(std::function<void(uint64_t ticket)> fn);

private:
  void run();

  struct Job {
    std::shared_ptr<const Index> index;
    std::shared_ptr<const EntryTags> tags;
    SearchQuery query;
    uint64_t ticket = 0;
  };
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  bool stop_ = false;
  bool hasJob_ = false;
  Job job_;
  std::atomic<uint64_t> latestTicket_{0};
  std::shared_ptr<const SearchResult> latest_;
  std::function<void(uint64_t)> onResult_;
  std::thread thread_;
};

}  // namespace pq::library
