#include "TestFramework.h"
#include "Core/StemAligner.h"
#include <vector>

using namespace mma;

namespace {

/// A ramp starting at `first`: sample n carries first + n, so what comes out
/// says which input sample it was (zero is silence the aligner wrote).
std::vector<float> ramp (int first, int count)
{
    std::vector<float> v (static_cast<size_t> (count));
    for (int i = 0; i < count; ++i)
        v[static_cast<size_t> (i)] = static_cast<float> (first + i);
    return v;
}

} // namespace

TEST_CASE (StemAligner_AChannelWithNoOffsetPassesStraightThrough)
{
    StemAligner a;
    a.prepare (1);

    auto block = ramp (1, 64);
    a.process (0, block.data(), block.size());

    for (int i = 0; i < 64; ++i)
        REQUIRE (block[static_cast<size_t> (i)] == static_cast<float> (1 + i));

    REQUIRE (a.isExact());
    REQUIRE (a.getSilenceInserted (0) == 0u);
}

TEST_CASE (StemAligner_TheStartingOffsetOpensTheStemWithThatMuchSilence)
{
    // The quicker interface's stem starts 37 samples of silence later, and
    // then carries every sample it handed over, in order.
    StemAligner a;
    a.prepare (2);
    a.setOffset (0, 37);
    a.setOffset (1, 0);

    auto quick = ramp (1, 200), slow = ramp (1, 200);
    a.process (0, quick.data(), 100);
    a.process (0, quick.data() + 100, 100); // across a chunk boundary
    a.process (1, slow.data(), slow.size());

    for (int i = 0; i < 37; ++i)
        REQUIRE (quick[static_cast<size_t> (i)] == 0.0f);
    for (int i = 37; i < 200; ++i)
        REQUIRE (quick[static_cast<size_t> (i)] == static_cast<float> (i - 37 + 1));

    REQUIRE (slow[0] == 1.0f);
    REQUIRE (a.getStartOffset (0) == 37);
    REQUIRE (a.getOffset (0) == 37);

    // Starting silence is the take's alignment, not a mid-take change.
    REQUIRE (a.getSilenceInserted (0) == 0u);
    REQUIRE (a.getSamplesDropped (0) == 0u);
}

TEST_CASE (StemAligner_ALongerOffsetMidTakeWritesSilenceAndLosesNothing)
{
    // Another device's IO block grew: this channel is held back 1092 more
    // from sample 300 on. Silence goes out there, and every sample it handed
    // over still comes out, in order, just that much later.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 10);

    auto first = ramp (1, 300);
    a.process (0, first.data(), first.size());
    a.setOffset (0, 10 + 1092);

    auto second = ramp (301, 3000);
    a.process (0, second.data(), second.size());

    std::vector<float> out (first);
    out.insert (out.end(), second.begin(), second.end());

    // 10 of starting silence, 290 samples, 1092 of silence, then on from 291.
    for (int i = 0; i < 10; ++i)
        REQUIRE (out[static_cast<size_t> (i)] == 0.0f);
    for (int i = 10; i < 300; ++i)
        REQUIRE (out[static_cast<size_t> (i)] == static_cast<float> (i - 9));
    for (int i = 300; i < 300 + 1092; ++i)
        REQUIRE (out[static_cast<size_t> (i)] == 0.0f);
    for (int i = 300 + 1092; i < 3300; ++i)
        REQUIRE (out[static_cast<size_t> (i)] == static_cast<float> (i - 9 - 1092));

    REQUIRE (a.getSilenceInserted (0) == 1092u);
    REQUIRE (a.getSamplesDropped (0) == 0u);
    REQUIRE (a.getOffset (0) == 1102);
}

TEST_CASE (StemAligner_AShorterOffsetTakesOutTheMovesSilenceAlreadyWaiting)
{
    // This channel's own device grew its block: its stream put 500 samples
    // of silence in as it moved, and the writer brings the stem forward by
    // the same 500. What is taken out is what had just come in -- that
    // silence -- so the audio either side of it joins up.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 1092);

    std::vector<float> in;
    for (int i = 0; i < 2000; ++i)
        in.push_back (static_cast<float> (i + 1));
    for (int i = 0; i < 500; ++i)
        in.push_back (0.0f); // the stream's own move
    for (int i = 2000; i < 6000; ++i)
        in.push_back (static_cast<float> (i + 1));

    auto out = in;
    a.process (0, out.data(), 2500);
    a.setOffset (0, 1092 - 500);
    a.process (0, out.data() + 2500, out.size() - 2500);

    REQUIRE (a.getSamplesDropped (0) == 500u);
    REQUIRE (a.getSilenceInserted (0) == 0u);

    // After the starting silence the ramp runs on without a gap or a repeat.
    for (size_t i = 1092; i < out.size(); ++i)
        REQUIRE (out[i] == static_cast<float> (i - 1092 + 1));
}

TEST_CASE (StemAligner_AShorterOffsetTakesOutTheMovesSilenceStillArriving)
{
    // A move the cushion covered: the offset shrinks at the block whose start
    // carries the stream's silence (300 samples), and some of the move went
    // out as a dry pull just before it (200). Both are taken out -- the run
    // already waiting and the run arriving -- and the ramp joins up.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 2000);

    std::vector<float> before;
    for (int i = 0; i < 3000; ++i)
        before.push_back (static_cast<float> (i + 1));
    for (int i = 0; i < 200; ++i)
        before.push_back (0.0f);

    std::vector<float> after;
    for (int i = 0; i < 300; ++i)
        after.push_back (0.0f);
    for (int i = 3000; i < 7000; ++i)
        after.push_back (static_cast<float> (i + 1));

    a.process (0, before.data(), before.size());
    a.setOffset (0, 2000 - 500);
    a.process (0, after.data(), after.size());

    std::vector<float> out (before);
    out.insert (out.end(), after.begin(), after.end());

    REQUIRE (a.getSamplesDropped (0) == 500u);
    for (size_t i = 2000; i < out.size(); ++i)
        REQUIRE (out[i] == static_cast<float> (i - 2000 + 1));
}

TEST_CASE (StemAligner_AudioIsTakenOutOnlyWhereNoSilenceIsFound)
{
    // Nothing silent either side of the change: the newest 100 samples
    // waiting go, and from then on the stem is 100 samples further on.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 400);

    auto first = ramp (1, 1000);
    a.process (0, first.data(), first.size());
    a.setOffset (0, 300);

    auto second = ramp (1001, 1000);
    a.process (0, second.data(), second.size());

    REQUIRE (a.getSamplesDropped (0) == 100u);

    // Up to the change: the starting silence and then the ramp, held 400 back.
    REQUIRE (first[399] == 0.0f);
    REQUIRE (first[400] == 1.0f);
    REQUIRE (first[999] == 600.0f);

    // After it: samples 601..900 were waiting and still come out; 901..1000
    // (the newest) were taken out; then the ramp goes on from 1001.
    REQUIRE (second[0] == 601.0f);
    REQUIRE (second[299] == 900.0f);
    REQUIRE (second[300] == 1001.0f);
    REQUIRE (second[999] == 1700.0f);
}

TEST_CASE (StemAligner_AShorterOffsetFirstCancelsSilenceNotYetWritten)
{
    // A longer offset and then a shorter one before the silence for the first
    // has all gone out: the rest of it is simply not written, and nothing is
    // taken out of the audio.
    StemAligner a;
    a.prepare (1);

    auto first = ramp (1, 100);
    a.process (0, first.data(), first.size());
    a.setOffset (0, 1000);

    auto second = ramp (101, 300);
    a.process (0, second.data(), second.size()); // 300 of the 1000 written
    a.setOffset (0, 0);                           // 700 never written, 300 taken back

    auto third = ramp (401, 100);
    a.process (0, third.data(), third.size());

    REQUIRE (a.getSilenceInserted (0) == 300u);
    REQUIRE (a.getSamplesDropped (0) == 300u);
    REQUIRE (a.getOffset (0) == 0);

    // Back to passing straight through.
    for (int i = 0; i < 100; ++i)
        REQUIRE (third[static_cast<size_t> (i)] == static_cast<float> (401 + i));
}

TEST_CASE (StemAligner_AnOffsetPastItsBoundIsClampedAndSaidSo)
{
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, StemAligner::kMaxOffsetSamples + 5);

    REQUIRE (! a.isExact());
    REQUIRE (a.getOffset (0) == StemAligner::kMaxOffsetSamples);

    // Still a working line at its bound.
    std::vector<float> block (static_cast<size_t> (StemAligner::kMaxOffsetSamples) + 10, 1.0f);
    a.process (0, block.data(), block.size());
    REQUIRE (block[static_cast<size_t> (StemAligner::kMaxOffsetSamples) - 1] == 0.0f);
    REQUIRE (block[static_cast<size_t> (StemAligner::kMaxOffsetSamples)] == 1.0f);
}

TEST_CASE (StemOffsetQueue_HandsChangesOverInOrderAndSaysWhenFull)
{
    StemOffsetQueue q;
    q.clear();

    for (size_t i = 0; i < StemOffsetQueue::kCapacity; ++i)
        REQUIRE (q.push ({ i, 0, static_cast<int> (i) }));

    REQUIRE (! q.push ({ 9999, 0, 1 }));

    StemOffsetEvent e;
    REQUIRE (q.peek (e));
    REQUIRE (e.frame == 0u);
    q.pop();
    REQUIRE (q.peek (e));
    REQUIRE (e.frame == 1u);

    // Room again for one.
    REQUIRE (q.push ({ 5000, 1, 7 }));
}
