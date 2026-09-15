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

bool alsaRecoveryRunMeansDeviceIsDead (int consecutiveRecoveries) noexcept
{
    return consecutiveRecoveries >= kRecoveriesBeforeGivingUp;
}

bool alsaOutputNameIsExclusiveCapable (const std::string& outputName) noexcept
{
    // Every prefix here resolves to `type hw` in the pcm definitions alsa-lib
    // ships: a direct hardware PCM with no mixing plugin in front of it. The
    // colon is part of each one, because alsa-lib's hints carry arguments
    // ("front:CARD=USB,DEV=0") and the bare forms are marked omit_noargs.
    //
    // Deliberately absent: default, sysdefault, dmix, dsnoop, dshare, plug,
    // pulse, pipewire, jack, null, modem. Those really are shared -- a card's
    // pcm.default routes through dmix whenever use_dmix is on -- and opening
    // one exclusively is the delay §5.4 exists to avoid.
    static constexpr const char* kDirectHardwarePrefixes[] = {
        "hw:", "plughw:",
        "front:", "rear:", "center_lfe:", "side:",
        "surround21:", "surround40:", "surround41:",
        "surround50:", "surround51:", "surround71:",
        "hdmi:", "iec958:", "spdif:"
    };

    for (const char* prefix : kDirectHardwarePrefixes)
        if (outputName.rfind (prefix, 0) == 0)
            return true;

    return false;
}

} // namespace mma::alsa_detail
