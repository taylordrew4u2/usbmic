// Does the ALSA worker end a stream whose PCM fails and recovers forever?
//
// snd_pcm_recover succeeding says the PCM was put back into a runnable state,
// not that the device works. A PCM that fails and recovers on every read
// reaches neither of the worker loop's exits, so it used to spin there for the
// rest of the take: the microphone written as silence, and only a rising
// dropped-frame count to show for it.
//
// The rule that ends it is unit-tested on every platform. Its WIRING into the
// loop -- the counter, and the successful read that resets it -- could not be,
// because the ALSA `file` plugin the Linux fixture is built on does not xrun,
// and there is no plugin that can be asked to. So this drives the real backend
// against a real libasound with one function interposed: snd_pcm_readi, made to
// fail the way a sick PCM fails. Everything else in the path is the shipping
// code, including snd_pcm_open and the worker thread.
//
//   Tools/alsa_recovery_death.sh
//
// Passes when the stream is ended and reported instead of spinning.

#include "Platform/AlsaBackend.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

} // namespace

int main()
{
    const bool expectDeath = std::getenv ("MMA_EXPECT_STREAM_DEATH") != nullptr;

    std::printf ("%s\n", expectDeath
                             ? "A PCM that fails and recovers on every read"
                             : "Control: the same fixture with no interposed failure");

    mma::AlsaBackend backend;

    const auto devices = backend.enumerateInputDevices();

    if (devices.empty())
    {
        std::printf ("  FAIL  no fixture microphone enumerated -- "
                     "run Tools/setup_alsa_fixture.sh and build with "
                     "MMA_ALLOW_TEST_INPUTS=ON\n");
        return 1;
    }

    const auto deviceId = devices.front().usbLocationId;
    std::printf ("  Device: %s\n", deviceId.c_str());

    if (! backend.openInputStream (deviceId, 48000.0, 256,
                                   [] (const float* const*, int, float* const*, int, int) {}))
    {
        std::printf ("  FAIL  the stream would not open: %s\n",
                     backend.getLastOpenError().c_str());
        return 1;
    }

    check (true, "the stream opens");

    // Generous, and deliberately so: the rule needs an unbroken run of
    // recoveries, and a test that gave up before the loop could reach one
    // would report the wrong answer for the right-looking reason.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
    std::vector<mma::StreamFailure> reported;

    while (std::chrono::steady_clock::now() < deadline)
    {
        auto batch = backend.takeStreamFailures();
        reported.insert (reported.end(), batch.begin(), batch.end());

        if (! reported.empty())
            break;

        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }

    if (expectDeath)
    {
        check (! reported.empty(),
               "the endlessly-recovering PCM is reported rather than spun on");

        if (! reported.empty())
            std::printf ("  Reported: %s %s\n", reported.front().deviceId.c_str(),
                         reported.front().reason.c_str());
    }
    else
    {
        // The control matters as much as the case: if a healthy fixture mic
        // also reported a failure, the rule above would be firing on load and
        // the passing test would be proving nothing.
        check (reported.empty(), "a healthy PCM is not reported as dead");
    }

    backend.closeAllStreams();

    std::printf ("%s (%d failing)\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
