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

// A PCM that fails and recovers on every read is not glitching under load, it
// has stopped working -- and until this rule existed the ALSA worker had no way
// to tell the two apart, so it spun on the second case for the rest of the take.
TEST_CASE (AlsaInputPolicy_AnOccasionalRecoveryIsLoadRatherThanDeath)
{
    REQUIRE_FALSE (alsa_detail::alsaRecoveryRunMeansDeviceIsDead (0));
    REQUIRE_FALSE (alsa_detail::alsaRecoveryRunMeansDeviceIsDead (1));
    REQUIRE_FALSE (alsa_detail::alsaRecoveryRunMeansDeviceIsDead (10));

    // A busy machine can xrun a lot without the device being broken, so the
    // rule has to sit well clear of anything load alone produces between two
    // successful reads.
    REQUIRE_FALSE (alsa_detail::alsaRecoveryRunMeansDeviceIsDead (
                       alsa_detail::kRecoveriesBeforeGivingUp - 1));
}

TEST_CASE (AlsaInputPolicy_AnUnbrokenRunOfRecoveriesIsADeadDevice)
{
    REQUIRE (alsa_detail::alsaRecoveryRunMeansDeviceIsDead (
                 alsa_detail::kRecoveriesBeforeGivingUp));
    REQUIRE (alsa_detail::alsaRecoveryRunMeansDeviceIsDead (
                 alsa_detail::kRecoveriesBeforeGivingUp + 1));
    REQUIRE (alsa_detail::alsaRecoveryRunMeansDeviceIsDead (100000));
}

// §5.4 monitoring was off on Linux for the hardware this app is named for.
//
// The picker is filled from snd_device_name_hint, and no pcm definition
// alsa-lib ships emits a bare "hw:" hint -- a USB interface's outputs come
// through as "front:CARD=...", "hdmi:CARD=...", "iec958:CARD=...". The old
// predicate accepted "hw:" and nothing else, so every one of them was refused
// with a message telling the user to choose a specific sound card, which is
// what they had just done.
//
// These names are not invented for the test: they are the forms alsa-lib's own
// front.conf, hdmi.conf and iec958.conf produce, and USB-Audio.conf defines
// pcm.front.0 as `type hw`.
TEST_CASE (AlsaInputPolicy_AnInterfacesOwnOutputCanBeMonitoredExclusively)
{
    REQUIRE (alsa_detail::alsaOutputNameIsExclusiveCapable ("front:CARD=USB,DEV=0"));
    REQUIRE (alsa_detail::alsaOutputNameIsExclusiveCapable ("hdmi:CARD=HDMI,DEV=0"));
    REQUIRE (alsa_detail::alsaOutputNameIsExclusiveCapable ("iec958:CARD=USB,DEV=0"));
    REQUIRE (alsa_detail::alsaOutputNameIsExclusiveCapable ("surround51:CARD=PCH,DEV=0"));
    REQUIRE (alsa_detail::alsaOutputNameIsExclusiveCapable ("rear:CARD=PCH,DEV=0"));

    // The two that always worked, kept so widening the list cannot drop them.
    REQUIRE (alsa_detail::alsaOutputNameIsExclusiveCapable ("hw:CARD=USB,DEV=0"));
    REQUIRE (alsa_detail::alsaOutputNameIsExclusiveCapable ("plughw:CARD=USB,DEV=0"));
}

// The half that matters more, because widening an allowlist is how a real
// shared device gets opened and 40 ms of delay reaches someone's headphones
// with nothing said about it. Every name here routes through a mixing or
// resampling plugin and must still be refused.
TEST_CASE (AlsaInputPolicy_ASharedOutputIsStillRefused)
{
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("default"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("sysdefault:CARD=USB"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("dmix:CARD=USB,DEV=0"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("dsnoop:CARD=USB,DEV=0"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("plug:dmix"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("pulse"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("pipewire"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("jack"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("null"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("modem:CARD=PCH"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable (""));

    // A user-defined .asoundrc alias says nothing about what it wraps, so it
    // stays refused rather than opened hopefully -- this is still an allowlist.
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("mma_out"));
}

// Prefix matching, not substring matching. "surround51" must not be reached by
// anything that merely contains it, and a device whose name begins with an
// accepted word but is a different PCM must not slip through on the strength of
// the letters alone -- which is why every prefix carries its colon.
TEST_CASE (AlsaInputPolicy_TheExclusiveNamesAreMatchedAsPrefixesWithTheirSeparator)
{
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("myhw:CARD=USB"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("front"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("fronting:CARD=USB"));
    REQUIRE_FALSE (alsa_detail::alsaOutputNameIsExclusiveCapable ("not-hw:CARD=USB"));
}
