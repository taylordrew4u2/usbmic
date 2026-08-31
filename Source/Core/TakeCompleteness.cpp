#include "TakeCompleteness.h"

#include <algorithm>
#include <cctype>

namespace mma {

namespace {

bool isMetadata (const std::string& name)
{
    // Case-insensitive suffix test: the writer produces "session.json", but a
    // folder is a folder and nothing stops one holding "SESSION.JSON".
    const std::string suffix = ".json";

    if (name.size() < suffix.size())
        return false;

    return std::equal (suffix.rbegin(), suffix.rend(), name.rbegin(),
                       [] (char a, char b)
                       {
                           return std::tolower (static_cast<unsigned char> (a))
                                  == std::tolower (static_cast<unsigned char> (b));
                       });
}

} // namespace

bool takeHoldsNoAudio (const std::vector<TakeFile>& files)
{
    int64_t audioBytes = 0;
    int audioFiles = 0;

    for (const auto& f : files)
    {
        if (isMetadata (f.name))
            continue;

        audioBytes += f.sizeBytes;
        ++audioFiles;
    }

    // No audio files at all is not "an empty take" -- there is nothing to
    // judge, and saying a take is empty when the folder holds only metadata
    // would misreport a state this rule was not written for.
    return audioFiles > 0 && audioBytes < static_cast<int64_t> (audioFiles) * 1024;
}

} // namespace mma
