#include "PortIdentity.h"
#include "SessionFolderNaming.h"

namespace mma {

void PersistedDeviceSettings::setNameForInput (int input, bool perInput, const std::string& rawName)
{
    const auto clean = SessionFolderNaming::sanitizeNameOrEmpty (rawName);

    if (perInput)
    {
        if (clean.empty()) inputNames.erase (input);
        else               inputNames[input] = clean;
        return;
    }

    assignedName = clean;
}

void PortIdentityStore::put (const PortIdentity& id, const PersistedDeviceSettings& settings)
{
    byKey[id.key()] = settings;
}

std::optional<PersistedDeviceSettings> PortIdentityStore::get (const PortIdentity& id) const
{
    auto it = byKey.find (id.key());
    if (it == byKey.end())
        return std::nullopt;
    return it->second;
}

bool PortIdentityStore::contains (const PortIdentity& id) const
{
    return byKey.find (id.key()) != byKey.end();
}

} // namespace mma
