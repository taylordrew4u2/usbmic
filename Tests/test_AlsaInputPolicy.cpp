#include "TestFramework.h"
#include "Platform/AlsaInputPolicy.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace mma;

namespace {

struct TemporaryTree
{
    TemporaryTree()
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path()
             / ("sobstage-alsa-policy-" + std::to_string (nonce));
        std::filesystem::create_directories (root);
    }

    ~TemporaryTree()
    {
        std::error_code ignored;
        std::filesystem::remove_all (root, ignored);
    }

    std::filesystem::path root;
};

void writeRemovable (const std::filesystem::path& directory,
                     const std::string& value)
{
    std::filesystem::create_directories (directory);
    std::ofstream (directory / "removable") << value << '\n';
}

} // namespace

TEST_CASE (AlsaInputPolicy_RemovableAncestorIsAccepted)
{
    TemporaryTree tree;
    const auto usbDevice = tree.root / "devices" / "usb" / "1-2";
    // The exact sysfs component spelling is irrelevant to this policy test.
    // Avoid ':' so the same synthetic tree is legal on Windows runners too.
    const auto audioInterface = usbDevice / "interface-1.0" / "sound" / "card4";

    std::filesystem::create_directories (audioInterface);
    writeRemovable (usbDevice, "removable");

    REQUIRE (alsa_detail::deviceAncestryIsUserRemovable (audioInterface));
}

TEST_CASE (AlsaInputPolicy_FixedUnknownAndMissingEvidenceFailClosed)
{
    TemporaryTree tree;
    const auto fixed = tree.root / "fixed" / "sound";
    const auto unknown = tree.root / "unknown" / "sound";
    const auto missing = tree.root / "missing" / "sound";

    writeRemovable (fixed.parent_path(), "fixed");
    writeRemovable (unknown.parent_path(), "unknown");
    std::filesystem::create_directories (fixed);
    std::filesystem::create_directories (unknown);
    std::filesystem::create_directories (missing);

    REQUIRE_FALSE (alsa_detail::deviceAncestryIsUserRemovable (fixed));
    REQUIRE_FALSE (alsa_detail::deviceAncestryIsUserRemovable (unknown));
    REQUIRE_FALSE (alsa_detail::deviceAncestryIsUserRemovable (missing));
    REQUIRE_FALSE (alsa_detail::deviceAncestryIsUserRemovable (tree.root / "absent"));
}

TEST_CASE (AlsaInputPolicy_OnlyTheExactKernelValueIsPositiveEvidence)
{
    TemporaryTree tree;
    const auto malformed = tree.root / "malformed";

    writeRemovable (malformed, "removable unknown");

    REQUIRE_FALSE (alsa_detail::deviceAncestryIsUserRemovable (malformed));
}

TEST_CASE (AlsaInputPolicy_KernelCardLookupRejectsVirtualAndUnknownCards)
{
    TemporaryTree tree;
    const auto soundClass = tree.root / "class" / "sound";
    const auto externalCard = soundClass / "card7" / "device";

    writeRemovable (externalCard, "removable");

    REQUIRE (alsa_detail::isDirectExternalHardwareCard (7, soundClass));
    REQUIRE_FALSE (alsa_detail::isDirectExternalHardwareCard (8, soundClass));
    REQUIRE_FALSE (alsa_detail::isDirectExternalHardwareCard (-1, soundClass));
}

TEST_CASE (AlsaInputPolicy_VirtualInputsNeedTheCompileTimeTestPath)
{
    // Shipping input enumeration never visits ALSA's plugin hints. A dedicated
    // test build may visit them so the file-backed fixture remains useful.
    REQUIRE_FALSE (alsa_detail::shouldUseHintEnumeration (true, false));
    REQUIRE (alsa_detail::shouldUseHintEnumeration (true, true));
}

TEST_CASE (AlsaInputPolicy_OutputEnumerationIsUnaffected)
{
    REQUIRE (alsa_detail::shouldUseHintEnumeration (false, false));
    REQUIRE (alsa_detail::shouldUseHintEnumeration (false, true));
}

TEST_CASE (AlsaInputPolicy_GenericWiredUsbAudioCannotRevealItsFormFactor)
{
    TemporaryTree tree;
    const auto genericUsbAudio = tree.root / "phone-or-interface" / "audio";

    // Linux exposes transport and physical removability, not whether a USB
    // Audio Class endpoint happens to be a phone. A model/VID denylist would be
    // guesswork and would reject legitimate interfaces, so this policy makes
    // only the supportable claim: physically removable hardware is external.
    writeRemovable (genericUsbAudio, "removable");

    REQUIRE (alsa_detail::deviceAncestryIsUserRemovable (genericUsbAudio));
}
