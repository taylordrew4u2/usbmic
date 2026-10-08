#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mma {

/// One channel's alignment changing: from `frame` of the take on (counted in
/// frames the writer accepted), channel `channel` is to be held back by
/// `offset` samples.
struct StemOffsetEvent
{
    uint64_t frame = 0;
    int channel = 0;
    int offset = 0;
};

/// Carries alignment changes from the audio thread to the writer thread,
/// beside the audio itself. Single producer (the callback handing blocks to
/// WritePipeline), single consumer (the writer). Fixed storage, no locks and
/// no allocation on either side (§11).
class StemOffsetQueue
{
public:
    /// Changes come only when a device's IO size or the rig's slowest device
    /// changes -- a handful a take -- so this is far more than is ever queued.
    static constexpr size_t kCapacity = 1024;

    /// Not concurrent with push or pop: before the take's audio starts.
    void clear() noexcept;

    /// Producer. False when full; nothing is queued then.
    bool push (const StemOffsetEvent& event) noexcept;

    /// Consumer: the oldest change, without taking it.
    bool peek (StemOffsetEvent& event) const noexcept;

    /// Consumer: takes the oldest change. Only after a successful peek.
    void pop() noexcept;

private:
    std::array<StemOffsetEvent, kCapacity> slots {};
    std::atomic<size_t> head { 0 }; // next slot the producer writes
    std::atomic<size_t> tail { 0 }; // next slot the consumer reads
};

/// Lines the stems up on the writer thread, where it costs the headphones
/// nothing (§5.4, §6.1).
///
/// Every microphone reaches the headphone mix as early as its own device
/// allows: a device that reports more input latency, or runs at a larger IO
/// block, hands its audio over later, and nothing on the monitor path waits
/// for it. So the samples the writer receives are not lined up -- a clap two
/// interfaces heard at once arrives in their channels that many samples
/// apart. Each channel is delayed here by its offset (the slowest device's
/// latency less its own), through a delay line per channel, so the clap lands
/// on the same frame in every stem and the mix summed from them.
///
/// A channel's offset can change mid-take, when a device's IO block grows:
///  - Longer: the channel is held back further by writing silence into it
///    at once while its audio waits in the delay line -- nothing it handed
///    over is lost, it comes out later.
///  - Shorter: the channel's own device has grown its block and its stream
///    has just moved later by putting exactly that much silence in
///    (DeviceInputStream moves once, by the growth). That silence is what
///    is taken out: the run of it already waiting at the newest end of the
///    line, then whatever of it is still arriving. Only what neither finds
///    -- audio arrived first -- comes out of the newest samples waiting, so
///    a growth on a device that is not the slowest leaves its stem
///    seamless.
///  - The offset set before a channel's first sample is the take's starting
///    alignment: the leading channels start with that much silence.
///
/// What is still in a line when the take stops is audio from after the
/// slowest device's last sample, past the aligned end of the take, and is not
/// written: every stem holds exactly the frames the take accepted.
///
/// Memory is allocated once, in prepare(), and bounded by kMaxOffsetSamples a
/// channel. Not real-time: process() and setOffset() run on the writer thread
/// and are cheap enough never to hold it up. The getters are for any thread.
class StemAligner
{
public:
    /// The most a channel is ever held back: a quarter of a second at 96 kHz
    /// of input latency plus the largest IO block a device can run at, far
    /// past any wired interface's figures. An offset past it is clamped, and
    /// the take's alignment then reported as not exact. 112 KB a channel.
    static constexpr int kMaxOffsetSamples = 24000 + 4096;

    /// Writer side, before the take: allocates every channel's line.
    void prepare (int numChannels);

    /// Writer thread: from the next sample process() takes for `channel`, it
    /// is held back by `samples`.
    void setOffset (int channel, int samples) noexcept;

    /// Writer thread: `frames` samples of one channel, in place, through its
    /// line.
    void process (int channel, float* samples, size_t frames) noexcept;

    int getNumChannels() const noexcept { return static_cast<int> (lines.size()); }

    /// The offset in force now, and the one the take started with.
    int getOffset (int channel) const noexcept;
    int getStartOffset (int channel) const noexcept;

    /// Silence written into this channel after its first sample to hold it
    /// back further, and samples taken out to bring it forward.
    uint64_t getSilenceInserted (int channel) const noexcept;
    uint64_t getSamplesDropped (int channel) const noexcept;

    /// False once any offset had to be clamped.
    bool isExact() const noexcept { return exact.load (std::memory_order_relaxed); }

private:
    struct Line
    {
        std::vector<float> buffer;
        size_t head = 0;  // oldest sample waiting
        size_t size = 0;  // samples waiting
        size_t paddingOwed = 0; // silence still to write for the starting offset
        size_t silenceOwed = 0; // silence still to write for a later, longer one
        size_t skipOwed = 0;    // silence still to take out as it arrives, for a shorter one
        bool started = false;

        std::atomic<int> offset { 0 };
        std::atomic<int> startOffset { 0 };
        std::atomic<uint64_t> silenceInserted { 0 };
        std::atomic<uint64_t> dropped { 0 };
    };

    std::vector<std::unique_ptr<Line>> lines;
    std::atomic<bool> exact { true };

    static void dropNewest (Line& line, size_t count) noexcept;
};

} // namespace mma
