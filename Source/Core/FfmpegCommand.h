#pragma once
#include <string>
#include <vector>
#include "CombinedTakePlan.h"

namespace mma {

/// The argument list for combining one camera's picture with the take's sound.
///
/// Built as a vector rather than a command line on purpose: a session folder
/// can hold spaces, quotes and any other character a person can type into a
/// folder name, and the moment those go through a shell they stop being part
/// of a path. Handing the argument vector straight to the process is the only
/// way that is safe, so there is deliberately no "build the command string"
/// function here for a caller to reach for by mistake.
///
/// Pure: it names files, it does not touch them.
///
/// `videoPath`, `audioPath` and `outputPath` are absolute, because the muxer
/// runs with no defined working directory.
std::vector<std::string> buildFfmpegArguments (const std::string& ffmpegExecutable,
                                               const std::string& videoPath,
                                               const std::string& audioPath,
                                               const std::string& outputPath,
                                               double audioLeadSeconds);

/// Seconds as ffmpeg wants them: fixed-point with millisecond resolution and a
/// dot, whatever the machine's locale would otherwise do to the decimal
/// separator. A comma here does not fail loudly -- ffmpeg reads "0,25" as 0 --
/// so the take would come back looking fine and sounding a quarter-second out.
std::string formatSecondsForFfmpeg (double seconds);

} // namespace mma
