#pragma once
#include <atomic>
#include <cstdint>

namespace mma {

/// The sounds SobStage makes about itself, rendered into the headphone mix
/// by the audio callback: a rising chirp when a take starts, a falling one
/// when it stops, and a siren that repeats until the mid-take card is
/// acknowledged. Generated here rather than played through the OS, because
/// the one output this app holds is the exclusive monitor stream, and a
/// second player would fight it for the device.
///
/// Real-time safe: the message thread sets an atomic, the audio thread reads
/// it and advances a phase. No allocation, no locking, no branches on state
/// that could change under it mid-block.
class AlarmTone
{
public:
    enum class Kind : int
    {
        None = 0,
        Started,   ///< three rising beeps, once
        Stopped,   ///< three falling beeps, once
        Fault,     ///< two-tone siren, repeating until cleared
    };

    /// Audible over a mix at the -3 dBFS monitor ceiling, under the clip point
    /// once summed with it, and the siren a touch louder than the chirps.
    static constexpr float kChirpLevel = 0.35f;
    static constexpr float kSirenLevel = 0.5f;

    /// Message thread. A chirp plays once from its start; a fault keeps
    /// sounding until setFault (false) or trigger (None).
    void trigger (Kind kind) noexcept;

    /// Message thread. Idempotent: repeating true does not restart the siren,
    /// so a caller can re-assert it on every UI tick and a rebuilt capture
    /// picks it up.
    void setFault (bool on) noexcept;

    Kind getKind() const noexcept { return kindOf (state.load (std::memory_order_acquire)); }
    bool isSounding() const noexcept;

    /// Audio thread. ADDS the tone to `inOut`, so it sits over whatever mix is
    /// already there. Sample rate is taken here rather than stored, because
    /// the coordinator that owns this can be rebuilt at a new rate.
    void render (float* inOut, int numSamples, double sampleRate) noexcept;

    /// Total samples of tone rendered so far; a test reads it to prove sound
    /// actually reached the output.
    uint64_t getSamplesRendered() const noexcept { return samplesRendered.load (std::memory_order_relaxed); }

    /// The shape of each pattern, so a test can check the timing without
    /// re-deriving it from the waveform.
    static constexpr double kBeepSeconds = 0.12;
    static constexpr double kGapSeconds = 0.06;
    static constexpr int kBeepsPerChirp = 3;
    static constexpr double kSirenHalfPeriodSeconds = 0.25;

private:
    /// Which pattern, and which trigger of it, in ONE word: the generation
    /// above the low byte, the kind in it. They used to be two atomics,
    /// stored one after the other, so the audio thread could read the new
    /// kind with the old generation -- and render a fresh chirp from where the
    /// previous one had ended, find it already over, and mark it finished.
    /// The chirp then played with isSounding() false, and a fault re-asserted
    /// on the next tick cut it off.
    std::atomic<uint64_t> state { 0 };
    static Kind kindOf (uint64_t packed) noexcept { return static_cast<Kind> (packed & 0xffu); }
    static uint64_t generationOf (uint64_t packed) noexcept { return packed >> 8; }
    std::atomic<uint64_t> samplesRendered { 0 };
    /// When the current pattern was triggered, by the wall clock. A chirp is
    /// over that long after it started whether or not anything rendered it,
    /// so a rig with no output cannot leave a chirp "playing" forever.
    std::atomic<int64_t> triggeredAtNs { 0 };
    /// The generation the audio thread last rendered a chirp to its end, so
    /// a rig whose output runs ahead of the clock reports it over as well.
    /// A generation rather than a flag: a flag the audio thread set for the
    /// pattern it was finishing could land after a new trigger had cleared
    /// it, marking the new chirp over before it began.
    std::atomic<uint64_t> finishedGeneration { ~uint64_t (0) };

    // Audio-thread state.
    uint64_t renderedGeneration = 0;
    double positionSeconds = 0.0;
    double phase = 0.0;

    static int64_t nowNs() noexcept;

    static float frequencyAt (Kind kind, double seconds, bool& sounding) noexcept;
};

} // namespace mma
