#pragma once
#include <string>
#include <optional>
#include <map>

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
