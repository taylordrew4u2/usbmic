#include "QuitGate.h"

namespace mma {

QuitGate::Decision QuitGate::onQuitRequested (bool readyToQuit, double nowMs) noexcept
{
    if (readyToQuit)
    {
        pending = false;
        return Decision::QuitNow;
    }

    if (pending)
        return nowMs - firstRequestMs >= bound ? Decision::ForceQuit : Decision::Ignore;

    pending = true;
    firstRequestMs = nowMs;
    return Decision::StartWaiting;
}

bool QuitGate::onPoll (bool readyToQuit) noexcept
{
    if (! pending || ! readyToQuit)
        return false;

    pending = false;
    return true;
}

} // namespace mma
