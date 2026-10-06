#pragma once

#include <cstdint>
#include <string>

namespace dslr {

/// The four 16-bit segments of a lock object and their bit offsets inside the 64-bit word
/// (Figure 3 of the paper, most significant segment first):
///
///      63      48 47      32 31      16 15       0
///     +----------+----------+----------+----------+
///     |    nX    |    nS    |   maxX   |   maxS   |
///     +----------+----------+----------+----------+
///
/// `nX`/`nS` are the "now serving" counters of Lamport's bakery: the exclusive / shared ticket
/// numbers whose holders are allowed to proceed. `maxX`/`maxS` are the "next ticket" counters:
/// a transaction draws a ticket by atomically incrementing one of them with RDMA fetch-and-add
/// and reading back the previous value.
enum class Segment : unsigned {
  MaxS = 0,
  MaxX = 16,
  NS = 32,
  NX = 48,
};

/// Decoded view of a lock object L = {nX, nS, maxX, maxS}. The member names follow the paper's
/// notation on purpose so the code can be read side by side with Algorithms 1-3.
struct LockWord {
  uint16_t nX = 0;    ///< exclusive ticket currently being served
  uint16_t nS = 0;    ///< shared ticket currently being served
  uint16_t maxX = 0;  ///< next exclusive ticket to be handed out
  uint16_t maxS = 0;  ///< next shared ticket to be handed out

  static constexpr LockWord decode(uint64_t raw) {
    return LockWord{static_cast<uint16_t>(raw >> 48), static_cast<uint16_t>(raw >> 32),
                    static_cast<uint16_t>(raw >> 16), static_cast<uint16_t>(raw)};
  }

  constexpr uint64_t encode() const {
    return (uint64_t{nX} << 48) | (uint64_t{nS} << 32) | (uint64_t{maxX} << 16) | uint64_t{maxS};
  }

  constexpr uint16_t get(Segment segment) const {
    switch (segment) {
      case Segment::MaxS:
        return maxS;
      case Segment::MaxX:
        return maxX;
      case Segment::NS:
        return nS;
      case Segment::NX:
        return nX;
    }
    return 0;
  }

  friend constexpr bool operator==(const LockWord&, const LockWord&) = default;
};

/// Addend that makes RDMA fetch-and-add increment a single segment: FA(L, segment, amount) in
/// the paper's notation is `fetch_add(L, segment_add(segment, amount))`. Neighbouring segments
/// are untouched as long as the segment does not overflow, which DSLR prevents by resetting
/// counters once they reach COUNT_MAX (Section 4.9).
constexpr uint64_t segment_add(Segment segment, uint16_t amount) {
  return uint64_t{amount} << static_cast<unsigned>(segment);
}

/// Human-readable form, e.g. "{nX=1, nS=1, maxX=2, maxS=4}".
std::string to_string(const LockWord& word);

}  // namespace dslr
