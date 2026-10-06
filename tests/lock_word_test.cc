#include "dslr/lock_word.h"

#include <gtest/gtest.h>

namespace dslr {
namespace {

TEST(LockWord, EncodeDecodeRoundTrip) {
  const LockWord word{1, 2, 3, 4};
  const uint64_t raw = word.encode();
  EXPECT_EQ(raw, (uint64_t{1} << 48) | (uint64_t{2} << 32) | (uint64_t{3} << 16) | 4);
  EXPECT_EQ(LockWord::decode(raw), word);
}

TEST(LockWord, LayoutMatchesFigure3) {
  // Figure 3: nX is the most significant segment, maxS the least significant one.
  EXPECT_EQ(LockWord::decode(0x0001'0000'0000'0000ull), (LockWord{1, 0, 0, 0}));
  EXPECT_EQ(LockWord::decode(0x0000'0001'0000'0000ull), (LockWord{0, 1, 0, 0}));
  EXPECT_EQ(LockWord::decode(0x0000'0000'0001'0000ull), (LockWord{0, 0, 1, 0}));
  EXPECT_EQ(LockWord::decode(0x0000'0000'0000'0001ull), (LockWord{0, 0, 0, 1}));
}

TEST(LockWord, SegmentAddTouchesOnlyOneSegment) {
  const LockWord start{10, 20, 30, 40};
  EXPECT_EQ(LockWord::decode(start.encode() + segment_add(Segment::MaxS, 2)),
            (LockWord{10, 20, 30, 42}));
  EXPECT_EQ(LockWord::decode(start.encode() + segment_add(Segment::MaxX, 2)),
            (LockWord{10, 20, 32, 40}));
  EXPECT_EQ(LockWord::decode(start.encode() + segment_add(Segment::NS, 2)),
            (LockWord{10, 22, 30, 40}));
  EXPECT_EQ(LockWord::decode(start.encode() + segment_add(Segment::NX, 2)),
            (LockWord{12, 20, 30, 40}));
}

TEST(LockWord, PaperExampleFigure4) {
  // T3 draws an exclusive ticket on L = {1, 1, 1, 4}: FA(L, maxX, 1) returns the ticket and
  // leaves L = {1, 1, 2, 4}.
  const LockWord before{1, 1, 1, 4};
  const LockWord after = LockWord::decode(before.encode() + segment_add(Segment::MaxX, 1));
  EXPECT_EQ(after, (LockWord{1, 1, 2, 4}));
  // T3 must wait for nX == 1 and nS == 4, i.e. for maxS - nS = 3 shared tickets.
  EXPECT_EQ(before.maxS - before.nS, 3);
}

TEST(LockWord, GetBySegment) {
  const LockWord word{1, 2, 3, 4};
  EXPECT_EQ(word.get(Segment::NX), 1);
  EXPECT_EQ(word.get(Segment::NS), 2);
  EXPECT_EQ(word.get(Segment::MaxX), 3);
  EXPECT_EQ(word.get(Segment::MaxS), 4);
}

TEST(LockWord, ToString) {
  EXPECT_EQ(to_string(LockWord{1, 2, 3, 4}), "{nX=1, nS=2, maxX=3, maxS=4}");
}

}  // namespace
}  // namespace dslr
