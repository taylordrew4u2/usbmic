#include "TestFramework.h"
#include "Core/AppSettings.h"

using namespace mma;

namespace {

AppSettings populated()
{
    AppSettings s;
    s.destinationFolder = "/Volumes/CARD/RECORDINGS";
    s.confirmedSaveLocation = "/Volumes/CARD/RECORDINGS";
    s.askWhereToSaveEveryTime = true;
    s.mirrorEnabled = false;
    s.aggregateName = "Kitchen Table";
    s.masterVolume = 42.0;
    s.cameraPreviewFullQuality = true;
    s.cameraTileScale = 3;

    PersistedPort port;
    port.key = "usb-1-2|SERIAL9";
    port.settings.assignedName = "Alice";
    port.settings.trimDb = -3.5f;
    port.settings.channelLayoutIsMono = false;
    port.settings.hasChannelLayoutDecision = true;
    s.ports.push_back (port);

    s.disabledMicKeys.push_back ("usb-9-9");
    s.cameras.push_back ({ "Logitech C920", true, "Wide shot" });

    return s;
}

} // namespace

TEST_CASE (AppSettings_everythingSurvivesARoundTrip)
{
    const auto restored = AppSettings::fromJsonString (populated().toJsonString());

    REQUIRE (restored.destinationFolder == std::string ("/Volumes/CARD/RECORDINGS"));
    REQUIRE (restored.confirmedSaveLocation == std::string ("/Volumes/CARD/RECORDINGS"));
    REQUIRE (restored.askWhereToSaveEveryTime);
    REQUIRE_FALSE (restored.mirrorEnabled);
    REQUIRE (restored.aggregateName == std::string ("Kitchen Table"));
    REQUIRE (restored.masterVolume == 42.0);
    REQUIRE (restored.cameraPreviewFullQuality);
    REQUIRE (restored.cameraTileScale == 3);
}

TEST_CASE (AppSettings_aMicrophoneKeepsItsNameAndTrimAcrossLaunches)
{
    const auto restored = AppSettings::fromJsonString (populated().toJsonString());
    const auto* port = restored.findPort ("usb-1-2|SERIAL9");

    // §2.4 already carried these across a replug and lost them on quit, because
    // nothing ever wrote the store to disk.
    REQUIRE (port != nullptr);
    REQUIRE (port->settings.assignedName == std::string ("Alice"));
    REQUIRE (port->settings.trimDb == -3.5f);
    REQUIRE_FALSE (port->settings.channelLayoutIsMono);
    REQUIRE (port->settings.hasChannelLayoutDecision);

    REQUIRE (restored.findPort ("usb-nothing-here") == nullptr);
}

TEST_CASE (AppSettings_switchedOffMicrophonesAreKeyedByPortNotName)
{
    const auto restored = AppSettings::fromJsonString (populated().toJsonString());

    // Four identical microphones share a product string (§14.6). Keyed by name,
    // switching one off would switch off its siblings on the next launch.
    REQUIRE (restored.isMicDisabled ("usb-9-9"));
    REQUIRE_FALSE (restored.isMicDisabled ("usb-1-2|SERIAL9"));
}

TEST_CASE (AppSettings_camerasKeepTheirAnswers)
{
    const auto restored = AppSettings::fromJsonString (populated().toJsonString());
    const auto* camera = restored.findCamera ("Logitech C920");

    REQUIRE (camera != nullptr);
    REQUIRE (camera->enabled);
    REQUIRE (camera->assignedName == std::string ("Wide shot"));
    REQUIRE (restored.findCamera ("no such camera") == nullptr);
}

TEST_CASE (AppSettings_garbageLoadsAsDefaultsRatherThanThrowing)
{
    // §10.1: the app launches to a working state. A preferences file truncated
    // by a power cut is not a reason to refuse to start, so every one of these
    // has to come back as the defaults without throwing.
    for (const auto* text : { "", "{", "not json at all", "[1,2,3", "\xff\xfe garbage" })
    {
        const auto s = AppSettings::fromJsonString (text);
        REQUIRE (s.mirrorEnabled);                  // §6.3 default on
        REQUIRE (s.masterVolume == 70.0);           // §5.1 default
        REQUIRE_FALSE (s.askWhereToSaveEveryTime);
        REQUIRE (s.destinationFolder.empty());
        REQUIRE (s.ports.empty());
    }
}

TEST_CASE (AppSettings_aFileFromAnotherVersionKeepsWhatItCanRead)
{
    // One known key, one key this version has never heard of, and none of the
    // rest. The known value has to survive and everything else fall back --
    // dropping the file whole would silently reset a rig on every upgrade.
    const auto s = AppSettings::fromJsonString (
        "{\"aggregateName\":\"From The Future\",\"somethingNewerVersionsAdded\":{\"a\":1}}");

    REQUIRE (s.aggregateName == std::string ("From The Future"));
    REQUIRE (s.mirrorEnabled);
    REQUIRE (s.masterVolume == 70.0);
}

TEST_CASE (AppSettings_entriesWithNoKeyAreDroppedRatherThanKept)
{
    // A port row with no key can never be matched to a device, and a camera row
    // with no id can never be matched to a camera. Keeping them would grow the
    // file on every launch with rows that can never apply.
    const auto s = AppSettings::fromJsonString (
        "{\"ports\":[{\"assignedName\":\"orphan\"},{\"key\":\"usb-1\",\"trimDb\":2.0}],"
        " \"cameras\":[{\"enabled\":true},{\"id\":\"cam\",\"enabled\":true}]}");

    REQUIRE (s.ports.size() == 1u);
    REQUIRE (s.ports[0].key == std::string ("usb-1"));
    REQUIRE (s.cameras.size() == 1u);
    REQUIRE (s.cameras[0].id == std::string ("cam"));
}

TEST_CASE (AppSettings_CameraTileScaleDefaultsWhenAbsent)
{
    // Every settings file written before the camera size existed has no such
    // key. Loading one must land on the default the main screen ships with
    // rather than zero, which is the smallest tile and would silently shrink
    // the pictures of everyone who upgrades.
    //
    // That default is well up the range on purpose: someone who has switched a
    // camera on wants to see the shot, and the first thing they saw was a
    // thumbnail they had to hunt for arrows to enlarge.
    const auto restored = AppSettings::fromJsonString ("{}");

    REQUIRE (restored.cameraTileScale == 3);
}
