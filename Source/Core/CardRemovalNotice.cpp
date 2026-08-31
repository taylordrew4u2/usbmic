#include "CardRemovalNotice.h"

namespace mma {

CardRemovalNotice CardRemovalNotice::build (const std::string& mirrorFolder, bool mirrorWasRunning)
{
    CardRemovalNotice notice;
    notice.aCompleteCopySurvives = mirrorWasRunning && ! mirrorFolder.empty();

    if (notice.aCompleteCopySurvives)
    {
        notice.survivingFolder = mirrorFolder;
        notice.message = "The drive you were recording to stopped responding, so the recording "
                         "has been stopped and every file closed. A complete backup copy is safe "
                         "on this computer: " + mirrorFolder;
    }
    else
    {
        // No copy to point at, so the sentence must not imply there is one.
        // §10.6: what happened, then what to do -- and the only thing to do
        // here is check the drive before recording again.
        notice.message = "The drive you were recording to stopped responding, so the recording "
                         "has been stopped and every file closed. Check that it is plugged in "
                         "properly before recording again, and keep the backup copy switched on.";
    }

    return notice;
}

} // namespace mma
