#include "AlsaInputPolicy.h"

#include <fstream>
#include <string>

namespace mma::alsa_detail {

namespace {

bool removableAttributeSaysUserRemovable (const std::filesystem::path& directory)
{
    std::ifstream attribute (directory / "removable");
    std::string value;
    std::string extra;

    // Token reads accept the newline emitted by sysfs but reject malformed
    // content such as "removable unknown" rather than guessing which wins.
    return (attribute >> value) && value == "removable" && ! (attribute >> extra);
}

} // namespace

bool deviceAncestryIsUserRemovable (const std::filesystem::path& devicePath)
{
    std::error_code error;
    auto current = std::filesystem::canonical (devicePath, error);

    if (error)
        return false;

    // A real sound-card device path is shallow, but cap the walk so a malformed
    // symlink cannot turn enumeration into an unbounded filesystem traversal.
    for (int depth = 0; depth < 32; ++depth)
    {
        if (removableAttributeSaysUserRemovable (current))
            return true;

        const auto parent = current.parent_path();
        if (parent.empty() || parent == current)
            break;

        current = parent;
    }

    return false;
}

bool isDirectExternalHardwareCard (int cardNumber,
                                   const std::filesystem::path& soundClassRoot)
{
    if (cardNumber < 0)
        return false;

    return deviceAncestryIsUserRemovable (
        soundClassRoot / ("card" + std::to_string (cardNumber)) / "device");
}

bool shouldUseHintEnumeration (bool wantInput, bool testInputsCompiledIn) noexcept
{
    return ! wantInput || testInputsCompiledIn;
}

} // namespace mma::alsa_detail
