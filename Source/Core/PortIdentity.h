#pragma once
#include <string>
#include <optional>
#include <map>
#include <vector>

namespace mma {

/// §2.4 port memory: identify a physical device by USB location ID plus serial
/// number where present, falling back to location ID alone. When a previously
/// seen identity reconnects, the caller should restore its persisted name/trim/
/// channel-layout decision without re-running §2.1 channel analysis or §14.6
/// tap-to-name.
struct PortIdentity
{
    std::string locationId;          // e.g. bus/port path, always present
    std::optional<std::string> serial; // present when the device reports one

    /// The stable key used to look up persisted settings for this physical port.
    std::string key() const
    {
        return serial.has_value() ? (locationId + "|" + *serial) : locationId;
    }

    bool operator== (const PortIdentity& other) const { return key() == other.key(); }
};

struct PersistedDeviceSettings
{
    std::string assignedName;
    float trimDb = 0.0f;
    bool channelLayoutIsMono = true;
    bool hasChannelLayoutDecision = false;

    /// Which physical side supplies a collapsed mono microphone. This matters
    /// for devices whose capsule is wired to input 1: after rebuilding from a
    /// Mono verdict, recording may start before the new analyzer sees signal,
    /// so defaulting back to input 0 would make that take silent.
    int channelLayoutMonoSource = 0;

    /// Inputs of an interface that are switched off. An eight-input box with
    /// two microphones on it recorded six files of silence and reported room
    /// for a fraction of the take it could have held; the unused sockets are
    /// simply not recorded now. Indices are physical inputs, 0-based.
    std::vector<int> disabledInputs;

    /// A name per input of an interface, keyed by physical input. The port's
    /// assignedName names the whole box; on an interface each socket is a
    /// person, and "Scarlett 2i2 2" is not who they are.
    std::map<int, std::string> inputNames;

    /// A trim per input of an interface, keyed by physical input, like
    /// inputNames. trimDb is the whole box's: a single microphone's trim, and
    /// the fallback for an input that has none of its own yet (every input of
    /// an interface saved before trim was kept per input).
    std::map<int, float> inputTrimDb;

    float trimDbForInput (int input) const
    {
        const auto own = inputTrimDb.find (input);
        return own != inputTrimDb.end() ? own->second : trimDb;
    }

    /// perInput is whether this port records as more than one channel. A
    /// single microphone's trim stays on the box, so it follows the mic even
    /// if the side it is recorded from changes.
    void setTrimDbForInput (int input, bool perInput, float db)
    {
        if (perInput)
        {
            inputTrimDb[input] = db;
            return;
        }

        trimDb = db;
        inputTrimDb.erase (input);
    }
};

/// In-memory persistence map keyed by PortIdentity::key(). Actual disk
/// serialization (e.g. to a settings JSON file) is left to the App layer;
/// this class holds the pure lookup/merge logic so it's independently testable.
class PortIdentityStore
{
public:
    void put (const PortIdentity& id, const PersistedDeviceSettings& settings);
    std::optional<PersistedDeviceSettings> get (const PortIdentity& id) const;
    bool contains (const PortIdentity& id) const;

    /// Everything remembered so far, keyed by PortIdentity::key(). The App
    /// layer needs this to write the store to disk -- which is the half of
    /// §2.4 this class's own comment says is left to it -- and there is no
    /// other way to ask what is in here.
    const std::map<std::string, PersistedDeviceSettings>& all() const { return byKey; }

private:
    std::map<std::string, PersistedDeviceSettings> byKey;
};

} // namespace mma
