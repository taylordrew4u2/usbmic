// A multi-input interface, on Linux, while the app is holding it.
//
// A capture device on real hardware opens once. Every later open -- this
// app's own included -- is -EBUSY. Two places asked the device a question by
// opening it again:
//
//   * openStream opened the PCM and then asked how many inputs it has by
//     opening it a second time. On hardware that second open was -EBUSY, the
//     answer fell back to one, and a four-input interface recorded one track.
//
//   * enumeration, run again on any device-list change, probed every device
//     by opening it. The one the app was recording from was busy, the probe
//     said "one input", and the strips already built for four collapsed.
//
// The fixture's `file` plugin opens as often as it is asked, so neither
// failure could happen against it. The shim's MMA_SHIM_EXCLUSIVE gives the
// fixture a real device's exclusivity; everything else is the shipping code.
//
//   Tools/alsa_busy_probe.sh

#include "Platform/AlsaBackend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

int failures = 0;

void check (bool condition, const std::string& what)
{
    std::printf (condition ? "  PASS  %s\n" : "  FAIL  %s\n", what.c_str());
    if (! condition)
        ++failures;
}

const mma::AudioDeviceDescriptor* find (const std::vector<mma::AudioDeviceDescriptor>& devices,
                                        const std::string& id)
{
    for (const auto& d : devices)
        if (d.usbLocationId == id)
            return &d;

    return nullptr;
}

} // namespace

int main (int argc, char** argv)
{
    // The fixture's four-input device; the harness defines it.
    const std::string interfaceId = (argc > 1) ? argv[1] : "mma_quad";
    constexpr int kInputs = 4;

    mma::AlsaBackend backend;

    std::printf ("A %d-input interface nobody is holding\n", kInputs);
    const auto before = backend.enumerateInputDevices();
    const auto* idle = find (before, interfaceId);
    check (idle != nullptr, "the interface is listed");
    check (idle != nullptr && idle->maxInputChannels == kInputs,
           "it reports " + std::to_string (kInputs) + " inputs (got "
           + std::to_string (idle != nullptr ? idle->maxInputChannels : -1) + ")");

    std::atomic<int> deliveredChannels { 0 };

    std::printf ("\nOpened for recording\n");
    const bool opened = backend.openInputStream (
        interfaceId, 48000.0, 256,
        [&deliveredChannels] (const float* const*, int numInputChannels, float* const*, int, int)
        {
            int seen = deliveredChannels.load();
            while (numInputChannels > seen
                   && ! deliveredChannels.compare_exchange_weak (seen, numInputChannels)) {}
        });
    check (opened, "the stream opens (" + backend.getLastOpenError() + ")");

    for (int i = 0; i < 200 && deliveredChannels.load() == 0; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    check (deliveredChannels.load() == kInputs,
           "every input is captured, one track each (got "
           + std::to_string (deliveredChannels.load()) + ")");

    std::printf ("\nThe device list changes while it is open\n");
    const auto during = backend.enumerateInputDevices();
    const auto* held = find (during, interfaceId);
    check (held != nullptr, "the interface is still listed");

    // Busy is not "one input". The probe cannot ask, so it must say it does
    // not know -- zero, the descriptor's "not reported" -- and leave the count
    // already known alone, rather than hand the app a smaller rig.
    check (held != nullptr && held->maxInputChannels != 1,
           "a probe that could not open the device does not report one input (got "
           + std::to_string (held != nullptr ? held->maxInputChannels : -1) + ")");
    check (held != nullptr && (held->maxInputChannels == 0 || held->maxInputChannels == kInputs),
           "it reports unknown or the true count");

    backend.closeAllStreams();

    std::printf ("\nReleased\n");
    const auto after = backend.enumerateInputDevices();
    const auto* released = find (after, interfaceId);
    check (released != nullptr && released->maxInputChannels == kInputs,
           "once released it reports " + std::to_string (kInputs) + " inputs again");

    std::printf ("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
