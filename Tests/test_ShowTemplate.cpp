#include "TestFramework.h"
#include "Core/ShowTemplate.h"
#include <algorithm>

using namespace mma;

namespace {

AppSettings podcastRig()
{
    AppSettings s;
    s.destinationFolder = "/Volumes/CARD/PODCAST";
    s.confirmedSaveLocation = "/Volumes/CARD/PODCAST";
    s.mirrorEnabled = false;
    s.masterVolume = 55.0;
    s.monitorThroughCombinedDevice = false;
    s.rememberedOutputDeviceId = "studio-headphones";
    s.combineVideoAndAudio = true;
    s.deliveryTarget = "Spotify";
    s.sampleRateOverride = 48000;
    s.bitDepthOverride = 32;

    // Machine and window choices a show must not carry.
    s.aggregateName = "Podcast Rig";
    s.cameraTileScale = 2;
    s.cameraPreviewFullQuality = true;
    s.bufferSizeOverride = 512;

    PersistedPort alice;
    alice.key = "usb-1|A";
    alice.settings.assignedName = "Alice";
    alice.settings.trimDb = -4.0f;
    alice.settings.hasChannelLayoutDecision = true;
    alice.settings.channelLayoutIsMono = true;
    s.ports.push_back (alice);

    PersistedPort box;
    box.key = "usb-2|IFACE";
    box.settings.inputNames[0] = "Bob";
    box.settings.inputNames[1] = "Carol";
    box.settings.inputTrimDb[1] = 3.0f;
    box.settings.disabledInputs = { 2, 3 };
    s.ports.push_back (box);

    s.disabledMicKeys = { "usb-3|SPARE" };
    s.headphonesOffKeys = { "usb-1|A" };
    s.cameras.push_back ({ "cam-wide", true, "Wide", "1080p" });
    return s;
}

bool has (const std::vector<std::string>& keys, const std::string& key)
{
    return std::find (keys.begin(), keys.end(), key) != keys.end();
}

} // namespace

TEST_CASE (ShowTemplate_extractThenApplyRestoresTheShow)
{
    const auto saved = ShowTemplate::extract ("Tuesday podcast", podcastRig());

    // Through the file, as it really travels.
    const auto loaded = ShowTemplate::fromJsonString (saved.toJsonString());
    REQUIRE (loaded.has_value());
    REQUIRE (loaded->name == std::string ("Tuesday podcast"));

    // A different evening: everything moved around.
    AppSettings now;
    now.destinationFolder = "/Users/me/Music";
    now.confirmedSaveLocation = "/Users/me/Music";
    now.masterVolume = 90.0;
    now.ports.push_back ({ "usb-1|A", {} });
    now.ports.back().settings.assignedName = "Guest";
    now.headphonesOffKeys = {};
    now.disabledMicKeys = { "usb-1|A" };
    now.cameras.push_back ({ "cam-wide", false, "" });

    const auto applied = loaded->applyTo (now);

    REQUIRE (applied.destinationFolder == std::string ("/Volumes/CARD/PODCAST"));
    REQUIRE_FALSE (applied.mirrorEnabled);
    REQUIRE (applied.masterVolume == 55.0);
    REQUIRE_FALSE (applied.monitorThroughCombinedDevice);
    REQUIRE (applied.rememberedOutputDeviceId == std::string ("studio-headphones"));
    REQUIRE (applied.combineVideoAndAudio);
    REQUIRE (applied.deliveryTarget == std::string ("Spotify"));
    REQUIRE (applied.sampleRateOverride == 48000u);
    REQUIRE (applied.bitDepthOverride == 32);

    const auto* alice = applied.findPort ("usb-1|A");
    REQUIRE (alice != nullptr);
    REQUIRE (alice->settings.assignedName == std::string ("Alice"));
    REQUIRE (alice->settings.trimDb == -4.0f);

    const auto* box = applied.findPort ("usb-2|IFACE");
    REQUIRE (box != nullptr);
    REQUIRE (box->settings.inputNames.at (1) == std::string ("Carol"));
    REQUIRE (box->settings.trimDbForInput (1) == 3.0f);
    REQUIRE (box->settings.disabledInputs.size() == 2u);

    // Alice was switched off this evening; the show has her on.
    REQUIRE_FALSE (applied.isMicDisabled ("usb-1|A"));
    REQUIRE (applied.isMicDisabled ("usb-3|SPARE"));
    REQUIRE (applied.areHeadphonesOff ("usb-1|A"));

    const auto* cam = applied.findCamera ("cam-wide");
    REQUIRE (cam != nullptr);
    REQUIRE (cam->enabled);
    REQUIRE (cam->assignedName == std::string ("Wide"));
    REQUIRE (cam->quality == std::string ("1080p"));
}

TEST_CASE (ShowTemplate_leavesMachineSettingsOut)
{
    const auto json = ShowTemplate::extract ("Live", podcastRig()).toJsonString();

    for (const char* key : { "aggregateName", "cameraTileScale", "cameraPreviewFullQuality",
                             "bufferSizeOverride", "confirmedSaveLocation" })
        REQUIRE (json.find (std::string ("\"") + key + "\"") == std::string::npos);

    // §2.1's verdict is the hardware's, not the show's.
    REQUIRE (json.find ("\"hasChannelLayoutDecision\": true") == std::string::npos);
}

TEST_CASE (ShowTemplate_unknownAndOutOfScopeKeysAreIgnored)
{
    const auto loaded = ShowTemplate::fromJsonString (R"({
        "name": "From the future",
        "formatVersion": 9,
        "aggregateName": "Hijacked",
        "cameraTileScale": 1,
        "bufferSizeOverride": 64,
        "windowWidth": 2000,
        "somethingNew": { "nested": [1, 2, 3] },
        "deliveryTarget": "YouTube",
        "mirrorEnabled": "yes please",
        "bitDepthOverride": 20
    })");

    REQUIRE (loaded.has_value());

    AppSettings now;
    now.aggregateName = "Mine";
    now.cameraTileScale = 4;
    now.bufferSizeOverride = 256;
    now.mirrorEnabled = true;
    now.bitDepthOverride = 16;

    const auto applied = loaded->applyTo (now);

    REQUIRE (applied.deliveryTarget == std::string ("YouTube"));
    REQUIRE (applied.aggregateName == std::string ("Mine"));
    REQUIRE (applied.cameraTileScale == 4);
    REQUIRE (applied.bufferSizeOverride == 256);

    // A value of the wrong kind, or one the app cannot record, is skipped
    // rather than turned into a default and applied.
    REQUIRE (applied.mirrorEnabled);
    REQUIRE (applied.bitDepthOverride == 16);
}

TEST_CASE (ShowTemplate_applyingDoesNotClobberWhatItDoesNotMention)
{
    // A template that only knows about one microphone and one setting.
    const auto loaded = ShowTemplate::fromJsonString (R"({
        "name": "Minimal",
        "deliveryTarget": "Apple Podcasts",
        "ports": [ { "key": "usb-1|A", "assignedName": "Alice" } ]
    })");
    REQUIRE (loaded.has_value());

    AppSettings now = podcastRig();
    now.ports[0].settings.assignedName = "Somebody";
    now.ports.push_back ({ "usb-9|NEW", {} });
    now.ports.back().settings.assignedName = "Bought last week";
    now.disabledMicKeys.push_back ("usb-9|NEW");
    now.cameras.push_back ({ "cam-close", true, "Close" });

    const auto applied = loaded->applyTo (now);

    REQUIRE (applied.deliveryTarget == std::string ("Apple Podcasts"));
    REQUIRE (applied.findPort ("usb-1|A")->settings.assignedName == std::string ("Alice"));

    // Everything else exactly as it was.
    REQUIRE (applied.destinationFolder == now.destinationFolder);
    REQUIRE (applied.confirmedSaveLocation == now.confirmedSaveLocation);
    REQUIRE (applied.masterVolume == now.masterVolume);
    REQUIRE (applied.sampleRateOverride == now.sampleRateOverride);
    REQUIRE (applied.findPort ("usb-9|NEW")->settings.assignedName == std::string ("Bought last week"));
    REQUIRE (applied.isMicDisabled ("usb-9|NEW"));
    REQUIRE (applied.isMicDisabled ("usb-3|SPARE"));
    REQUIRE (applied.findCamera ("cam-close")->enabled);
    REQUIRE (applied.findCamera ("cam-wide")->assignedName == std::string ("Wide"));
    REQUIRE (applied.findPort ("usb-2|IFACE")->settings.inputNames.at (0) == std::string ("Bob"));

    // The port the template names keeps its hardware verdict.
    REQUIRE (applied.findPort ("usb-1|A")->settings.hasChannelLayoutDecision);
}

TEST_CASE (ShowTemplate_connectedMicWithNoEntryIsSwitchedBackOnAndUnnamed)
{
    AppSettings rig;
    const auto saved = ShowTemplate::extract ("Plain", rig, { "usb-5|PLAIN" });
    REQUIRE (has (saved.microphoneKeys, "usb-5|PLAIN"));

    AppSettings later;
    later.ports.push_back ({ "usb-5|PLAIN", {} });
    later.ports.back().settings.assignedName = "Other show's name";
    later.disabledMicKeys = { "usb-5|PLAIN" };

    const auto applied = ShowTemplate::fromJsonString (saved.toJsonString())->applyTo (later);

    REQUIRE_FALSE (applied.isMicDisabled ("usb-5|PLAIN"));
    REQUIRE (applied.findPort ("usb-5|PLAIN")->settings.assignedName.empty());
}

TEST_CASE (ShowTemplate_newDestinationIsAskedAboutAgain)
{
    ShowTemplate t;
    t.destinationFolder = "/Volumes/OTHER";

    AppSettings now;
    now.destinationFolder = "/Volumes/CARD";
    now.confirmedSaveLocation = "/Volumes/CARD";

    REQUIRE (t.applyTo (now).confirmedSaveLocation.empty());

    // The same folder stays agreed to.
    t.destinationFolder = "/Volumes/CARD";
    REQUIRE (t.applyTo (now).confirmedSaveLocation == std::string ("/Volumes/CARD"));
}

TEST_CASE (ShowTemplate_unreadableTextIsNotATemplate)
{
    REQUIRE_FALSE (ShowTemplate::fromJsonString ("").has_value());
    REQUIRE_FALSE (ShowTemplate::fromJsonString ("   \n").has_value());
    REQUIRE_FALSE (ShowTemplate::fromJsonString ("[1, 2]").has_value());
    REQUIRE_FALSE (ShowTemplate::fromJsonString ("{}").has_value());
    REQUIRE_FALSE (ShowTemplate::fromJsonString ("not json at all").has_value());
}

TEST_CASE (ShowTemplate_fileNamesAreSanitized)
{
    REQUIRE (ShowTemplate::fileNameFor ("Tuesday podcast") == std::string ("Tuesday-podcast.json"));
    REQUIRE (ShowTemplate::fileNameFor ("  Live stage  ") == std::string ("Live-stage.json"));

    // Nothing that could leave the templates folder or upset a filesystem.
    const auto escaping = ShowTemplate::fileNameFor ("../../etc/passwd");
    REQUIRE (escaping.find ('/') == std::string::npos);
    REQUIRE (escaping.find ("..") == std::string::npos);
    REQUIRE (escaping == std::string ("etcpasswd.json"));

    const auto colon = ShowTemplate::fileNameFor ("Live: stage\\2*?");
    REQUIRE (colon.find_first_of (":\\*?") == std::string::npos);

    // Nothing usable left: no file at all, rather than "Session.json".
    REQUIRE (ShowTemplate::fileNameFor ("").empty());
    REQUIRE (ShowTemplate::fileNameFor ("   ").empty());
    REQUIRE (ShowTemplate::fileNameFor ("!!!").empty());
}

TEST_CASE (ShowTemplate_namesAreTidiedButKept)
{
    REQUIRE (ShowTemplate::cleanName ("  Live: stage!  ") == std::string ("Live: stage!"));
    REQUIRE (ShowTemplate::cleanName ("Two\nlines") == std::string ("Two lines"));
    REQUIRE (ShowTemplate::cleanName (std::string (200, 'x')).size() == 60u);

    // Cut on a character boundary, never inside one.
    std::string accented;
    for (int i = 0; i < 40; ++i)
        accented += "\xc3\xa9"; // é
    const auto cut = ShowTemplate::cleanName (accented);
    REQUIRE (cut.size() == 60u);
    REQUIRE ((static_cast<unsigned char> (cut.back()) & 0xC0) == 0x80);
    REQUIRE ((static_cast<unsigned char> (cut[cut.size() - 2]) & 0xE0) == 0xC0);
}

TEST_CASE (ShowTemplate_namesThatSanitizeAlikeGetTheirOwnFiles)
{
    using F = ShowTemplate::StoredFile;
    std::vector<F> folder;

    // The first takes the plain file.
    REQUIRE (ShowTemplate::fileNameForSaving ("Live stage", folder) == std::string ("Live-stage.json"));
    folder.push_back ({ "Live-stage.json", std::string ("Live stage") });

    // A different name that sanitizes the same way is not written over it.
    REQUIRE (ShowTemplate::fileNameForSaving ("Live-stage", folder) == std::string ("Live-stage-2.json"));
    folder.push_back ({ "Live-stage-2.json", std::string ("Live-stage") });
    REQUIRE (ShowTemplate::fileNameForSaving ("live stage", folder) == std::string ("live-stage-3.json"));

    // Saving the same name again replaces its own file, whichever it is.
    REQUIRE (ShowTemplate::fileNameForSaving ("Live stage", folder) == std::string ("Live-stage.json"));
    REQUIRE (ShowTemplate::fileNameForSaving ("  Live-stage ", folder) == std::string ("Live-stage-2.json"));

    // Listed, loaded and deleted by the stored name.
    REQUIRE (ShowTemplate::listShows (folder).size() == 2u);
    REQUIRE (ShowTemplate::findFileFor ("Live stage", folder) == std::string ("Live-stage.json"));
    REQUIRE (ShowTemplate::findFileFor ("Live-stage", folder) == std::string ("Live-stage-2.json"));
    REQUIRE (ShowTemplate::findFileFor ("live stage", folder).empty());

    // Nothing usable in the name: no file.
    REQUIRE (ShowTemplate::fileNameForSaving ("!!!", folder).empty());
}

TEST_CASE (ShowTemplate_strayFilesAreNeverOverwrittenOrLost)
{
    using F = ShowTemplate::StoredFile;

    // Not a template: never offered, never written over. Taken ignores case,
    // because the Mac's disk does.
    std::vector<F> folder { { "tuesday.json", std::nullopt } };
    REQUIRE (ShowTemplate::listShows (folder).empty());
    REQUIRE (ShowTemplate::fileNameForSaving ("Tuesday", folder) == std::string ("Tuesday-2.json"));

    // A template with no name, and a copy made by hand, are listed by their
    // file names so they can still be loaded or deleted.
    folder = { { "Tuesday.json", std::string ("Tuesday") },
               { "Tuesday-copy.json", std::string ("Tuesday") },
               { "Old.json", std::string() } };
    REQUIRE (ShowTemplate::findFileFor ("Tuesday", folder) == std::string ("Tuesday.json"));
    REQUIRE (ShowTemplate::findFileFor ("Tuesday-copy", folder) == std::string ("Tuesday-copy.json"));
    REQUIRE (ShowTemplate::findFileFor ("Old", folder) == std::string ("Old.json"));
    REQUIRE (ShowTemplate::listShows (folder).size() == 3u);

    // The name's own file wins over a copy that sorts first.
    folder = { { "A-copy.json", std::string ("Show") }, { "Show.json", std::string ("Show") } };
    REQUIRE (ShowTemplate::findFileFor ("Show", folder) == std::string ("Show.json"));
    REQUIRE (ShowTemplate::findFileFor ("A-copy", folder) == std::string ("A-copy.json"));
}
