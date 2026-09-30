#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../Platform/IAudioBackend.h"

namespace mma {

struct OutputDeviceCandidate
{
    std::string id;
    std::string displayName;

    /// True for a device exposing a physical headphone jack (§5.3 priority 3).
    bool hasPhysicalHeadphoneJack = false;

    /// The computer's own speakers/headphone output. This is the safe fallback
    /// when the OS default is ineligible because it is also a selected input;
    /// an arbitrary USB camera/capture-card playback endpoint is not.
    bool isBuiltIn = false;

    /// False when the backend positively reports that this output cannot run
    /// at the recording rate. Such an endpoint cannot clock the shared monitor
    /// stream and must not win merely because a capture card just appeared.
    /// Unknown capability remains true so enumeration gaps fail visibly at open.
    bool supportsRecordingSampleRate = true;

    /// True for a microphone's own playback endpoint. Never selectable at any
    /// priority (§5.2): the mic jacks carry non-defeatable analog direct
    /// monitoring, so a listener there hears themselves twice.
    bool isMicrophonePlaybackEndpoint = false;

    /// True for a device that is also a selected capture device. §5.5 refuses to
    /// route the monitor there, because output into an active input is a
    /// feedback loop by construction.
    bool isAlsoSelectedInput = false;

    /// The output the OS itself currently plays through (§5.3 priority 4).
    bool isSystemDefault = false;

    /// Bluetooth, Bluetooth LE or AirPlay. AirPods reconnecting or a paired
    /// speaker powering on "appears" without anyone plugging in headphones, and
    /// the monitor mix would then leave the performers' amp for a room speaker
    /// or a 150+ ms wireless path. Such an output is never chosen automatically
    /// while a wired one is eligible; an explicit pick is still honoured.
    bool isWireless = false;

    /// The output shares its id with a microphone that is listed but not being
    /// recorded (switched off in Settings, or past the eight-mic cap). §5.2
    /// keeps a mic's playback endpoint out of automatic selection at every
    /// priority; it stays eligible so the user can still pick an interface's
    /// headphone output by hand after unticking its inputs.
    bool belongsToUnrecordedMicrophone = false;

    /// True for a device that appeared after launch. §5.3 assumes that is the
    /// one the user just plugged in.
    bool appearedAfterLaunch = false;

    /// Order of appearance, so the most recently connected device wins a tie.
    int connectionOrder = 0;
};

enum class OutputSelectionReason
{
    None,
    RememberedFromPreviousSession, // priority 1
    NewlyConnected,                // priority 2
    PhysicalHeadphoneJack,         // priority 3
    SystemDefault,                 // priority 4
    BuiltInOutput,                 // safe fallback before arbitrary endpoints
};

struct OutputSelection
{
    bool found = false;
    std::string id;
    std::string displayName;
    OutputSelectionReason reason = OutputSelectionReason::None;

    /// Plain-language line for the user when nothing could be selected, per
    /// §10.6. Empty when a device was found.
    std::string explanation;
};

/// A microphone the device manager knows about, reduced to what output
/// selection needs: the id its duplex output would share, and whether it is
/// being recorded.
struct KnownMicrophone
{
    std::string locationId;
    bool included = false;
};

/// Keeps §5.3's "newly connected" priority tied to an actual arrival rather
/// than to an enumeration pass. Backends return snapshots, and a device-list
/// notification may be about an input, a rename, or no visible change at all;
/// treating every member of every later snapshot as new makes enumeration
/// order override the user's real choice.
class OutputDeviceTracker
{
public:
    /// Applies appearedAfterLaunch and a stable connectionOrder to a snapshot.
    /// The first snapshot is the launch baseline. A device becomes new only
    /// when its id was absent from the immediately preceding snapshot.
    std::vector<OutputDeviceCandidate> observe (std::vector<OutputDeviceCandidate> snapshot);

private:
    struct Connection
    {
        int order = 0;
        bool appearedAfterLaunch = false;
    };

    bool haveBaseline = false;
    int nextConnectionOrder = 0;
    std::unordered_map<std::string, Connection> connected;
};

/// §5.3 automatic output selection, in strict priority order. The user is never
/// asked (§10.1), so this has to reach a defensible answer on its own.
class OutputDeviceSelector
{
public:
    /// rememberedId is the device the user explicitly chose in a previous
    /// session, or empty if there is none.
    static OutputSelection select (const std::vector<OutputDeviceCandidate>& candidates,
                                   const std::string& rememberedId);

    /// A backend's advertised ranges can lag the nominal rate it is already
    /// running successfully. Either positive fact makes an output compatible;
    /// an empty capability list remains unknown/eligible.
    static bool supportsRecordingRate (uint32_t currentRate,
                                       const std::vector<uint32_t>& supportedRates,
                                       uint32_t recordingRate);

    /// A device is ineligible if it cannot run at the recording rate, is a
    /// microphone's playback endpoint, or is also a selected input. Every
    /// exclusion holds at every priority.
    static bool isEligible (const OutputDeviceCandidate& candidate);

    /// Maps one enumerated output onto a selection candidate. Every flag the
    /// backend reports has to reach the selector: a flag dropped here makes its
    /// priority silently unreachable, which is how the macOS default output and
    /// headphone jack were once never considered at all.
    static OutputDeviceCandidate candidateFromDescriptor (const AudioDeviceDescriptor& device,
                                                          uint32_t recordingRate,
                                                          const std::vector<KnownMicrophone>& microphones);
};

} // namespace mma
