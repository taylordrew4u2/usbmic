// A sound card that cannot run at the take's sample rate, on Linux.
//
// AlsaBackend::checkExclusiveModeCapability opened the device and, if the open
// succeeded, declared exclusive monitoring available and quoted a latency —
// without ever asking whether the device would run at the rate being asked
// for. An open is not a configuration. A 44100-only interface was reported as
// ready for exclusive monitoring at 48000, and the refusal surfaced later out
// of openStream as a monitoring failure that never mentioned the rate, so the
// one setting that would have fixed it was the one thing not named.
//
// This is the same hole CoreAudio had, and it stayed open on Linux for a
// structural reason: the capability check refuses any output name that is not
// direct hardware, and there is no real card in CI, so nothing could reach the
// code past that gate. The shim closes that by redirecting an allowlisted
// hw: name onto the fixture's file-plugin device (MMA_SHIM_OPEN_MATCH /
// MMA_SHIM_OPEN_AS) and refusing one rate at hw_params_test_rate
// (MMA_SHIM_REFUSE_RATE). Everything between those two calls — the name
// policy, the bounded open, the hw_params negotiation, the message — is the
// shipping code.
//
//   Tools/alsa_rate_refusal.sh

#include "Platform/AlsaBackend.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void check (bool condition, const std::string& what)
{
    std::printf (condition ? "  PASS  %s\n" : "  FAIL  %s\n", what.c_str());
    if (! condition)
        ++failures;
}

bool mentions (const std::string& haystack, const std::string& needle)
{
    return haystack.find (needle) != std::string::npos;
}

} // namespace

int main (int argc, char** argv)
{
    // The rate the harness expects the device to refuse, passed in so the
    // script and the shim cannot drift apart silently.
    const double refusedRate = (argc > 1) ? std::atof (argv[1]) : 48000.0;
    const double acceptedRate = (argc > 2) ? std::atof (argv[2]) : 44100.0;

    // A direct-hardware name, so the capability check's name policy lets it
    // through. The shim redirects the open onto the fixture.
    const std::string output = "plughw:99,0";

    mma::AlsaBackend backend;

    std::printf ("A card that cannot run at %.0f Hz\n", refusedRate);

    const auto refused = backend.checkExclusiveModeCapability (output, refusedRate, 256);

    check (! refused.exclusiveModeAvailable,
           "the refused rate is not reported as ready for exclusive monitoring");
    check (! refused.unavailableReason.empty(), "and a reason comes back");
    check (mentions (refused.unavailableReason, std::to_string (static_cast<int> (refusedRate))),
           "which names the rate, because the rate is the setting that fixes it");
    check (! mentions (refused.unavailableReason, "Another app"),
           "and does not blame another app for a rate the hardware cannot do");

    std::printf ("\nThe same card at a rate it does support\n");

    const auto accepted = backend.checkExclusiveModeCapability (output, acceptedRate, 256);

    // The control. Without it, an implementation that refused everything would
    // pass every check above.
    check (accepted.exclusiveModeAvailable,
           "a rate the card accepts still comes back available");
    check (accepted.unavailableReason.empty(), "with nothing to report");
    check (accepted.measuredOrEstimatedLatencyMs > 0.0, "and a latency to quote");

    std::printf ("\n%s\n", failures == 0 ? "ALL CHECKS PASSED (0 failing)"
                                         : "FAILURES");
    return failures == 0 ? 0 : 1;
}
