#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mma {

/// One file in a finished session folder, as it stands on disk.
struct TakeFile
{
    std::string name;
    int64_t sizeBytes = 0;
};

/// True when a finished take produced files but no audio worth the name.
///
/// A WAV header alone is 44 bytes plus the BWF chunk, so anything under a
/// kilobyte per file holds nothing. session.json is excluded: it is a few
/// hundred bytes whatever happened, and counting it would make a take that
/// recorded nothing look like it recorded something.
///
/// This lives here, rather than in the panel that first needed it, because two
/// places have to agree about it. The panel warned that the files were empty
/// while the status line said "Saved to ..." in the same breath -- and the
/// status line is the one a user reads while walking away.
bool takeHoldsNoAudio (const std::vector<TakeFile>& files);

} // namespace mma
