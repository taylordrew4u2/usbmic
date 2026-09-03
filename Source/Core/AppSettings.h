#pragma once
#include "Json.h"
#include "PortIdentity.h"
#include <string>
#include <vector>

namespace mma {

/// One camera's remembered answer, keyed by the id the OS gives it.
struct PersistedCamera
{
    std::string id;
    bool enabled = false;
    std::string assignedName;
};

/// One physical port's remembered settings (§2.4), keyed by PortIdentity::key().
struct PersistedPort
{
    std::string key;
    PersistedDeviceSettings settings;
};

/// Everything the app should still know about a rig the second time it opens.
///
/// §2.4 already carries a microphone's name and trim across a replug, and
/// PortIdentityStore says in as many words that writing them to disk is the App
/// layer's job -- which the App layer had never done. So every one of them was
/// carried faithfully across an unplug and lost completely on quit, along with
/// the destination folder, the mirror setting and which microphones were
/// switched off. §10.1 wants a novice to reach a working state without being
/// asked anything; asking them the same questions again every launch is the
/// same failure spread over more days.
///
/// Pure data and JSON, with no file I/O: where this lives on disk is the App
/// layer's business, and keeping it out of here is what lets the round trip be
/// tested on a machine with no audio hardware.
struct AppSettings
{
    // §10.1/§6.2: where recordings go, and the answer to the question asked
    // before the first take -- which is about a specific folder, so both are
    // stored and compared rather than reduced to one flag.
    std::string destinationFolder;
    std::string confirmedSaveLocation;
    bool askWhereToSaveEveryTime = false;

    bool mirrorEnabled = true;                          // §6.3, default on
    std::string aggregateName = "Multi-Mic Aggregator"; // §7, what other apps see
    double masterVolume = 70.0;                         // §5.1 default
    bool cameraPreviewFullQuality = false;

    /// How large the camera tiles on the main screen are drawn, as a step into
    /// MainScreen's size table. Remembered because it is a decision about the
    /// room -- one camera across a table wants a bigger picture than four in a
    /// row -- and re-making it every launch is the §10.1 failure of asking the
    /// same question twice.
    int cameraTileScale = 1;

    /// Whether a take also writes one video-with-sound file per camera.
    ///
    /// Off by default. It costs disk and minutes of CPU after every take, and
    /// the picture and sound are already both saved and already aligned by the
    /// shared session origin -- so this is for the person who wants a file they
    /// can send without opening an editor, and nobody else pays for it.
    bool combineVideoAndAudio = false;

    std::vector<PersistedPort> ports;
    /// §2.4 keys of the microphones the user has switched off. Keyed by port
    /// rather than by display name: four identical mics share a product string,
    /// and switching one off must not switch off its three siblings on the next
    /// launch.
    std::vector<std::string> disabledMicKeys;
    std::vector<PersistedCamera> cameras;

    JsonValue toJson() const;
    static AppSettings fromJson (const JsonValue& v);

    std::string toJsonString() const { return toJson().dump (2); }

    /// Never throws and never reports failure: a settings file that is corrupt,
    /// truncated by a power cut, or written by a newer version comes back as
    /// defaults. §10.1 says the app launches to a working state, and refusing
    /// to start over a preferences file would be the opposite.
    static AppSettings fromJsonString (const std::string& text);

    /// Look-ups the App layer needs when applying these to a live rig.
    const PersistedPort* findPort (const std::string& key) const;
    const PersistedCamera* findCamera (const std::string& id) const;
    bool isMicDisabled (const std::string& key) const;
};

} // namespace mma
