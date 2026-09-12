#include "RecordingProof.h"

namespace mma {

void RecordingProof::begin (const ProofReading& atArm)
{
    watching = true;
    bytesAtArm = atArm.bytesOnDisk;
    lastBytes = atArm.bytesOnDisk;
    lastGrowthSeconds = atArm.elapsedSeconds;
    everGrew = false;
    silenceReported = false;
}

ProofVerdict RecordingProof::observe (const ProofReading& now)
{
    if (! watching)
        return ProofVerdict::TooEarly;

    if (now.diskObservationAvailable && now.bytesOnDisk > lastBytes)
    {
        lastBytes = now.bytesOnDisk;
        lastGrowthSeconds = now.elapsedSeconds;
        everGrew = true;
    }

    // Grace period: files are created, headers are written, the first block
    // reaches the disk. Nothing is judged until it is over.
    if (now.elapsedSeconds < kGraceSeconds)
        return ProofVerdict::TooEarly;

    // The one that ruined a day: a "recording" that never wrote a byte. The
    // header alone does not count as growth -- the files must be larger than
    // they were the moment record was pressed by more than a header's worth.
    if (now.diskObservationAvailable
        && (! everGrew || now.bytesOnDisk <= bytesAtArm + 8192 * 4))
    {
        if (now.framesAccepted == 0 || ! everGrew)
            return ProofVerdict::NothingWritten;
    }

    if (now.diskObservationAvailable
        && now.elapsedSeconds - lastGrowthSeconds >= kStallSeconds)
        return ProofVerdict::Stalled;

    // Sound is a separate question from bytes: a take can write silence
    // perfectly. Said once, after enough time that a quiet start is not
    // mistaken for a dead rig.
    if (! silenceReported && now.elapsedSeconds >= kSilenceSeconds
        && now.peakArrived >= 0.0f && now.peakArrived < kSilenceFloorLinear)
    {
        silenceReported = true;
        return ProofVerdict::NoSoundArriving;
    }

    return ProofVerdict::Healthy;
}

std::string RecordingProof::message (ProofVerdict verdict)
{
    switch (verdict)
    {
        case ProofVerdict::NothingWritten:
            return "Nothing was recorded. The take was stopped after three seconds because "
                   "the files on the drive were not growing. Check the microphones' skulls "
                   "move when you speak, then try again -- and if they do not, open Help.";
        case ProofVerdict::Stalled:
            return "The files have stopped growing. The drive may have been removed or filled, "
                   "or has stopped responding. What was recorded so far is on the drive; "
                   "stop and check it now rather than trusting the rest of the take.";
        case ProofVerdict::NoSoundArriving:
            return "No sound has reached the app for twenty seconds. The files are being "
                   "written, but they hold silence. Check the mixer's USB send (LOOPBACK), "
                   "the channel's fader and mute, and the microphone's cable.";
        case ProofVerdict::TooEarly:
        case ProofVerdict::Healthy:
            break;
    }

    return {};
}

} // namespace mma
