#pragma once
#include <juce_core/juce_core.h>

namespace mma {

/// Replaces `file` with `text`, and only ever with all of it.
///
/// juce::File::replaceWithText writes to a hidden temporary file and renames
/// it over the target -- but it never looks at whether the write worked. On a
/// full disk the temporary file is created, the write fails (or lands short),
/// and the empty or truncated file is renamed over the good one anyway. That
/// is how a Mac with a full startup disk came back from one volume nudge with
/// a zero-byte settings.json: read on the next launch as a first launch, every
/// microphone name, trim and the destination silently gone and the setup guide
/// back on screen. A full card did the same to a take's session.json.
///
/// So the temporary file is written, flushed and measured here, and renamed
/// over the target only when it holds every byte. On any failure the target is
/// left exactly as it was and the temporary file is removed.
///
/// Verbatim: no line-ending conversion, so the size check compares like with
/// like.
inline bool replaceWithTextChecked (const juce::File& file, const juce::String& text)
{
    const auto expectedBytes = static_cast<juce::int64> (text.getNumBytesAsUTF8());

    juce::TemporaryFile temp (file, juce::TemporaryFile::useHiddenFile);

    {
        juce::FileOutputStream out (temp.getFile());

        if (out.failedToOpen())
            return false;

        if (! out.writeText (text, false, false, nullptr))
            return false;

        out.flush();

        if (out.getStatus().failed())
            return false;
    }

    // A short write is not an error FileOutputStream records, so the bytes on
    // disk are what decide.
    if (temp.getFile().getSize() != expectedBytes)
        return false;

    if (! temp.overwriteTargetFileWithTemporary())
        return false;

    return file.getSize() == expectedBytes;
}

} // namespace mma
