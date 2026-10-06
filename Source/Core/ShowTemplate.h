#pragma once
#include "AppSettings.h"
#include "Json.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mma {

/// A named rig setup -- "Tuesday podcast", "Live stage" -- saved once and
/// brought back with one click, so a show that recurs is not set up by hand
/// every week.
///
/// It holds the user's choices, never the hardware's state: who each
/// microphone is, its trim, which are switched off and which headphone jacks
/// are quiet, the cameras and their names, where takes go, the format and the
/// delivery target. Things that describe the machine or the window -- tile
/// size, preview quality, buffer size, the name other apps see, a §2.1 layout
/// verdict -- are left out, because loading a show must not undo a decision
/// that had nothing to do with the show.
///
/// Every top-level field is optional. A template only changes what it holds,
/// and microphones and cameras it does not mention keep whatever they had --
/// a template saved before a fourth mic was bought must not switch that mic
/// off or forget its name.
///
/// Pure data and JSON like AppSettings, with no file I/O: where templates
/// live is the App layer's business.
struct ShowTemplate
{
    static constexpr int kFormatVersion = 1;

    std::string name;

    std::optional<std::string> destinationFolder;
    std::optional<bool> askWhereToSaveEveryTime;
    std::optional<bool> mirrorEnabled;
    std::optional<double> masterVolume;
    std::optional<bool> monitorThroughCombinedDevice;
    std::optional<std::string> rememberedOutputDeviceId;
    std::optional<bool> combineVideoAndAudio;
    std::optional<std::string> deliveryTarget;
    std::optional<uint32_t> sampleRateOverride;
    std::optional<int> bitDepthOverride;

    /// §2.4 keys of every microphone this template has an answer for. A mic
    /// listed here and absent from disabledMicKeys is switched ON by the
    /// template; one not listed here is left exactly as it is.
    std::vector<std::string> microphoneKeys;
    std::vector<PersistedPort> ports;
    std::vector<std::string> disabledMicKeys;
    std::vector<std::string> headphonesOffKeys;
    std::vector<PersistedCamera> cameras;

    /// Takes the show-shaped part of `settings`. `connectedMicKeys` are the
    /// microphones plugged in right now: a mic with no name, trim or switch of
    /// its own has no entry in settings, and without this the template could
    /// never switch it back on.
    static ShowTemplate extract (const std::string& name, const AppSettings& settings,
                                 const std::vector<std::string>& connectedMicKeys = {});

    /// `settings` with this template laid over it. Fields the template does
    /// not hold, and microphones and cameras it does not mention, come back
    /// unchanged. A port's §2.1 layout verdict is the hardware's, so an
    /// existing one is kept; only the user's name, trims and inputs move.
    AppSettings applyTo (const AppSettings& settings) const;

    JsonValue toJson() const;
    std::string toJsonString() const { return toJson().dump (2); }

    /// Empty when the text is not a template at all. Keys this version does
    /// not know are ignored, so a template written by a newer version still
    /// loads everything this one understands.
    static std::optional<ShowTemplate> fromJsonString (const std::string& text);

    /// The name as typed, tidied for display: surrounding whitespace trimmed
    /// and capped at 60 characters. Empty means there was no name.
    static std::string cleanName (const std::string& rawName);

    /// The file a template of this name is kept in: §6.2's sanitizing, so a
    /// name like "../../etc" or "Live: stage/2" can never leave the templates
    /// folder or fail on a filesystem that refuses the character. Empty when
    /// nothing usable is left in the name.
    static std::string fileNameFor (const std::string& name);

    /// A .json file already in the templates folder, as the App layer found
    /// it: its file name and the name stored inside it. `name` is empty for a
    /// template that holds no name, and nullopt for a file that is not a
    /// template at all.
    struct StoredFile
    {
        std::string fileName;
        std::optional<std::string> name;
    };

    /// Every saved show as {listed name, file name}. A show is listed, loaded
    /// and deleted by the name stored in it, because two names can sanitize to
    /// the same file name -- "Live stage" and "Live-stage" -- and the second is
    /// then kept in "Live-stage-2.json". A file with no name of its own, or
    /// one whose name another file already answers to (a copy made by hand),
    /// is listed by its file name instead, so it can still be reached.
    static std::vector<std::pair<std::string, std::string>> listShows (const std::vector<StoredFile>& files);

    /// The file the show listed as `name` is kept in. Empty when there is none.
    static std::string findFileFor (const std::string& name, const std::vector<StoredFile>& files);

    /// The file saving `name` writes: the show's own file when one is already
    /// listed under that name, so saving it again replaces it; otherwise
    /// fileNameFor(), with "-2", "-3"... added while that file is taken by
    /// something else. Taken is judged ignoring case, since the Mac's disk
    /// does. Empty when nothing usable is left in the name.
    static std::string fileNameForSaving (const std::string& name, const std::vector<StoredFile>& files);
};

} // namespace mma
