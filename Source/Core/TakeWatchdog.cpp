#include "TakeWatchdog.h"

namespace mma {

void TakeWatchdog::beginTake (const TakeHealth& baseline)
{
    watching = true;
    last = baseline;
    warnedTenMinutes = false;
    warnedTwoMinutes = false;
    droppedReported = false;
    droppedReportedAt = 0.0;
}

void TakeWatchdog::endTake()
{
    watching = false;
    last = {};
}

std::vector<TakeAlert> TakeWatchdog::observe (const TakeHealth& now)
{
    std::vector<TakeAlert> alerts;

    if (! watching)
        return alerts;

    // Microphones, matched by position: the channel list is fixed for the
    // whole take (§6.5), so index i is the same person throughout.
    for (size_t i = 0; i < now.mics.size(); ++i)
    {
        const bool wasLive = i < last.mics.size() ? last.mics[i].live : true;
        const auto& mic = now.mics[i];

        if (wasLive && ! mic.live)
            alerts.push_back ({ TakeAlert::Kind::MicLost,
                                mic.name + " has stopped sending sound -- unplugged, or its cable "
                                "or mixer channel has gone. The take carries on; its file gets "
                                "silence until it comes back.", false });
        else if (! wasLive && mic.live)
            alerts.push_back ({ TakeAlert::Kind::MicBack, mic.name + " is back and recording again.", true });
    }

    // Cameras, matched by name: a camera that is no longer enumerated has
    // been unplugged or switched off, and its file stops growing. One that
    // has dropped out of the list altogether is carried forward as absent,
    // so it is reported once and can be reported back.
    auto cameras = now.cameras;

    for (const auto& old : last.cameras)
    {
        bool listed = false;
        for (const auto& cam : cameras)
            if (cam.name == old.name) { listed = true; break; }

        if (! listed)
            cameras.push_back ({ old.name, false });
    }

    for (const auto& cam : cameras)
    {
        bool wasPresent = true;
        for (const auto& old : last.cameras)
            if (old.name == cam.name) { wasPresent = old.present; break; }

        if (wasPresent && ! cam.present)
            alerts.push_back ({ TakeAlert::Kind::CameraLost,
                                "Camera " + cam.name + " has gone away -- unplugged or switched off. "
                                "The sound carries on; the picture stops here unless it comes back.", false });
        else if (! wasPresent && cam.present)
            alerts.push_back ({ TakeAlert::Kind::CameraBack, "Camera " + cam.name + " is back.", true });
    }

    if (! now.cameraProblem.empty() && now.cameraProblem != last.cameraProblem)
        alerts.push_back ({ TakeAlert::Kind::CameraTrouble, now.cameraProblem, false });

    if (now.outputClockLost && ! last.outputClockLost)
        alerts.push_back ({ TakeAlert::Kind::OutputLost,
                            "The headphone output has stopped -- unplugged, or taken by another "
                            "app. The take carries on on this computer's clock; you will not hear "
                            "the mix until it comes back.", false });
    else if (! now.outputClockLost && last.outputClockLost)
        alerts.push_back ({ TakeAlert::Kind::OutputBack, "The headphone output is back.", true });

    if (now.framesDropped > last.framesDropped || now.samplesOverrun > last.samplesOverrun)
    {
        const bool due = ! droppedReported
                      || now.elapsedSeconds - droppedReportedAt >= kDroppedRepeatSeconds;

        if (due)
        {
            // Two losses of the same kind -- audio that should have been
            // recorded and was not -- so they add. The rate is the take's own;
            // a hardcoded 48000 made this line claim twice the loss at 96 kHz.
            const auto rate = now.sampleRate > 0.0 ? now.sampleRate : 48000.0;
            const auto total = now.framesDropped + now.samplesOverrun;
            const auto seconds = total / rate; // a rough figure is all this line needs

            alerts.push_back ({ TakeAlert::Kind::AudioDropped,
                                droppedReported
                                    ? "Sound is still being dropped: about " + std::to_string (static_cast<int> (seconds + 0.5))
                                          + " s lost so far. Close other apps."
                                    : "Some sound was dropped: this computer could not keep up for a moment. "
                                      "Close other apps. The take carries on.",
                                false });
            droppedReported = true;
            droppedReportedAt = now.elapsedSeconds;
        }
    }

    if (now.writerBehind && ! last.writerBehind)
        alerts.push_back ({ TakeAlert::Kind::WriterBehind,
                            "The drive is falling behind the microphones. If this keeps up the "
                            "separate tracks will be dropped and only the mix kept. A faster drive "
                            "or fewer other apps fixes it.", false });

    if (now.mixOnly && ! last.mixOnly)
        alerts.push_back ({ TakeAlert::Kind::MixOnly,
                            "The drive could not keep up. From here only the mix is being written, "
                            "not the separate tracks.", false });

    if (! now.monitorProblem.empty() && now.monitorProblem != last.monitorProblem)
        alerts.push_back ({ TakeAlert::Kind::MonitorTrouble, now.monitorProblem, false });

    // Room on the drive: each threshold once per take, on the way down.
    if (now.remainingSeconds >= 0.0)
    {
        if (! warnedTwoMinutes && now.remainingSeconds < kTwoMinutes)
        {
            warnedTwoMinutes = true;
            warnedTenMinutes = true;
            alerts.push_back ({ TakeAlert::Kind::TwoMinutesLeft,
                                "About two minutes of room left on the drive. Wrap up now.", false });
        }
        else if (! warnedTenMinutes && now.remainingSeconds < kTenMinutes)
        {
            warnedTenMinutes = true;
            alerts.push_back ({ TakeAlert::Kind::TenMinutesLeft,
                                "About ten minutes of room left on the drive.", false });
        }
    }

    last = now;
    last.cameras = cameras;
    return alerts;
}

} // namespace mma
