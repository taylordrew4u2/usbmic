#include "PortIdentity.h"

namespace mma {

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
