#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace mma {

struct OutputDeviceCandidate
{
    std::string id;
    std::string displayName;

    /// True for a device exposing a physical headphone jack (§5.3 priority 3).
    bool hasPhysicalHeadphoneJack = false;

    /// True for a microphone's own playback endpoint. Never selectable at any
    /// priority (§5.2): the mic jacks carry non-defeatable analog direct
    /// monitoring, so a listener there hears themselves twice.
    bool isMicrophonePlaybackEndpoint = false;

    /// True for a device that is also a selected capture device. §5.5 refuses to
    /// route the monitor there, because output into an active input is a
    /// feedback loop by construction.
    bool isAlsoSelectedInput = false;

    bool isSystemDefault = false;

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

    /// A device is ineligible if it is a microphone's playback endpoint or is
    /// also a selected input. Both exclusions hold at every priority.
    static bool isEligible (const OutputDeviceCandidate& candidate);
};

} // namespace mma
