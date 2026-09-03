#include "FfmpegCommand.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mma {

std::string formatSecondsForFfmpeg (double seconds)
{
    if (! (seconds > 0.0)) // also catches NaN
        return "0.000";

    // std::snprintf with "%.3f" honours the C locale, which a GUI app on a
    // French or German machine may well have set to one that writes a comma.
    // The digits are assembled by hand so the separator cannot be anything but
    // a dot.
    const auto totalMs = static_cast<long long> (std::llround (seconds * 1000.0));
    const auto whole = totalMs / 1000;
    const auto millis = totalMs % 1000;

    char buffer[32];
    std::snprintf (buffer, sizeof (buffer), "%lld.%03lld", whole, millis);
    return buffer;
}

std::vector<std::string> buildFfmpegArguments (const std::string& ffmpegExecutable,
                                               const std::string& videoPath,
                                               const std::string& audioPath,
                                               const std::string& outputPath,
                                               double audioLeadSeconds,
                                               int audioBitDepth)
{
    std::vector<std::string> args;
    args.reserve (24);

    args.push_back (ffmpegExecutable);

    // Never wait for a keypress. Without this, ffmpeg asked to overwrite a file
    // reads from a stdin that is not attached to anything and the process hangs
    // for as long as the app is running.
    args.push_back ("-nostdin");
    args.push_back ("-loglevel");
    args.push_back ("error");

    // The output is derived from the take's own name, so anything already there
    // is a leftover from combining this same take before.
    args.push_back ("-y");

    // The picture, whole.
    args.push_back ("-i");
    args.push_back (videoPath);

    // The sound, started where the picture did. -ss *before* -i seeks the input
    // rather than decoding and discarding, which matters on a four-hour take:
    // the same trim after the input reads the whole file to throw the front of
    // it away.
    if (audioLeadSeconds > 0.0)
    {
        args.push_back ("-ss");
        args.push_back (formatSecondsForFfmpeg (audioLeadSeconds));
    }

    args.push_back ("-i");
    args.push_back (audioPath);

    // One video stream from the first input, one audio stream from the second.
    // Spelled out because ffmpeg's default mapping picks one stream per type
    // across all inputs by a rule that is not this one.
    args.push_back ("-map");
    args.push_back ("0:v:0");
    args.push_back ("-map");
    args.push_back ("1:a:0");

    // The picture is copied bit for bit. Re-encoding it would cost quality on
    // the one thing that cannot be recaptured, and minutes per take.
    args.push_back ("-c:v");
    args.push_back ("copy");

    // The sound is kept, not re-encoded.
    //
    // This was AAC at 256k, which is transparent enough for most listening and
    // still throws the take away: the stems are 24-bit PCM, and a lossy codec
    // is a one-way door. Nobody records at 24 bits in order to deliver a
    // generation-loss copy of it, and a combined file that is worse than the
    // parts it was made from is not worth making.
    //
    // Matroska cannot carry raw PCM as dependably as it carries FLAC, so there
    // it gets FLAC -- which is lossless, so the samples that come back out are
    // bit-for-bit the ones that went in.
    const bool matroska = outputPath.size() >= 4
                       && outputPath.compare (outputPath.size() - 4, 4, ".mkv") == 0;

    args.push_back ("-c:a");

    if (matroska)
    {
        args.push_back ("flac");
    }
    else
    {
        // At the depth the take was actually recorded at. Writing 24-bit audio
        // into a 16-bit stream would quietly discard the bottom eight bits,
        // which is the exact loss this change exists to avoid.
        args.push_back (audioBitDepth >= 24 ? "pcm_s24le" : "pcm_s16le");
    }

    // The two streams are the same length to within a frame, but a camera that
    // stopped early must not leave the file running on silence -- or, worse,
    // padded, which is what some players do with a video track that ends first.
    args.push_back ("-shortest");

    args.push_back (outputPath);

    return args;
}

} // namespace mma
