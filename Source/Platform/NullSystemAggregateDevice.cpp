#include "SystemAggregateDevice.h"
#include "PlatformMacros.h"

#if ! JUCE_MAC

namespace mma {

/// Windows and Linux have no public API for a system-wide combined device --
/// that gap is exactly what the §7 driver backends (B/C/D) exist to fill, and
/// each is blocked on something outside this codebase (ASIO SDK, a driver
/// licence, an EV certificate). Saying so beats pretending.
class NullSystemAggregateDevice : public SystemAggregateDevice
{
public:
    bool publish (const std::string&, const std::vector<std::string>& uids, const std::string&) override
    {
        count = static_cast<int> (uids.size());
        return false;
    }

    void remove() override { count = 0; }

    std::string getStatus() const override
    {
        return "On this platform other apps can't see a combined device yet -- "
               "that needs the virtual-device driver described in the README. "
               "Recording and monitoring in this app are unaffected.";
    }

private:
    int count = 0;
};

std::unique_ptr<SystemAggregateDevice> createSystemAggregateDevice()
{
    return std::make_unique<NullSystemAggregateDevice>();
}

} // namespace mma

#endif // ! JUCE_MAC
