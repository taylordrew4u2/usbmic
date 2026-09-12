#pragma once
#include <cstdint>
#include <string>

namespace mma {

/// One reading of the evidence that a take is really being recorded: what the
/// writer says it accepted, what the disk says it holds, and the loudest
/// sample that has arrived since the take began.
struct ProofReading
{
    double elapsedSeconds = 0.0;
    uint64_t framesAccepted = 0;
    uint64_t bytesOnDisk = 0;
    /// False when the asynchronous filesystem probe has not produced a
    /// snapshot for this take yet. That is unknown evidence, not evidence that
    /// the files contain zero bytes, and must never trigger an automatic stop.
    bool diskObservationAvailable = true;
    /// Loudest sample that reached the app since record was pressed, linear
    /// 0..1; negative when nothing has measured it yet.
    float peakArrived = -1.0f;
};

enum class ProofVerdict
{
    TooEarly,        // inside the grace period; nothing to say yet
    Healthy,
    NothingWritten,  // grace period over and the files have not grown at all
    Stalled,         // the files grew, then stopped growing
    NoSoundArriving, // files grow, but nothing above the silence floor has arrived
};

/// §0.1, made a rule the app enforces on itself: a take is only a take while
/// the files on disk are growing. The app used to keep the button red and
/// the clock running for an hour over seven empty files. Now it checks, and
/// the moment the disk disagrees with the screen, the screen loses.
///
/// Pure logic so every rule here is held by a test. The caller stops the
/// take on NothingWritten; on Stalled and NoSoundArriving it alarms.
class RecordingProof
{
public:
    /// The files must have grown by the end of this, or the take is stopped.
    static constexpr double kGraceSeconds = 3.0;
    /// Files that have not grown for this long mid-take are a stalled writer.
    static constexpr double kStallSeconds = 6.0;
    /// Nothing above this, for this long, is "no sound is reaching the app".
    static constexpr float kSilenceFloorLinear = 0.001f; // -60 dBFS
    static constexpr double kSilenceSeconds = 20.0;

    void begin (const ProofReading& atArm);
    ProofVerdict observe (const ProofReading& now);

    /// §10.6: what happened and what to do, for the verdict last returned.
    static std::string message (ProofVerdict verdict);

private:
    bool watching = false;
    uint64_t bytesAtArm = 0;
    uint64_t lastBytes = 0;
    double lastGrowthSeconds = 0.0;
    bool everGrew = false;
    bool silenceReported = false;
};

} // namespace mma
