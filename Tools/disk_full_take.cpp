// §6.5, on a drive that genuinely runs out of room in the middle of a take.
//
// Both halves of this were unit-tested against fakes and neither had ever met
// a real ENOSPC. What that hid: when a write failed, SessionWriter set no
// account at all -- only the roll-over past 3.9 GB did -- so the take stopped
// under the card-removal notice, which tells the user the drive "stopped
// responding" and to check that it is plugged in properly. Someone whose card
// was simply full therefore spent the one moment they were still next to the
// rig re-seating a cable that was never loose.
//
// Application.cpp already guarded against exactly this, with a comment saying
// so; the guard could not fire because the account it looks for was never
// written.
//
//   Tools/e2e_disk_full.sh   (creates the small filesystem this needs)

#include "Core/CaptureCoordinator.h"
#include "Platform/AlsaBackend.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check (bool condition, const std::string& what)
{
    std::printf (condition ? "  PASS  %s\n" : "  FAIL  %s\n", what.c_str());
    if (! condition)
        ++failures;
}

} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: disk_full_take <directory on a nearly-full filesystem>\n");
        return 2;
    }

    const std::string dir = argv[1];

    mma::AlsaBackend backend;
    auto devices = backend.enumerateInputDevices();

    if (devices.size() < 2)
    {
        std::printf ("  FAIL  need two fixture microphones, found %zu\n", devices.size());
        return 1;
    }

    mma::CaptureCoordinator coordinator (backend, 48000.0, 256);

    std::vector<mma::CaptureChannel> mics;

    for (size_t i = 0; i < 2; ++i)
    {
        mma::CaptureChannel c;
        c.deviceId = devices[i].usbLocationId;
        c.deviceChannel = 0;
        c.displayName = "Mic " + std::to_string (i + 1);
        c.fileName = "0" + std::to_string (i + 1) + "_Mic-" + std::to_string (i + 1);
        c.bitDepth = 24;
        mics.push_back (c);
    }

    if (! coordinator.startMonitoring (mics, {}))
    {
        std::printf ("  FAIL  startMonitoring: %s\n", coordinator.getMonitorProblem().c_str());
        return 1;
    }

    if (! coordinator.startRecording (dir, 24, "2026-09-15T00:00:00Z"))
    {
        std::printf ("  FAIL  startRecording into %s\n", dir.c_str());
        return 1;
    }

    check (! coordinator.hasCardWriteFailed(), "the take starts with the drive still writable");

    std::vector<float> out (512, 0.0f);
    float* outs[] = { out.data(), out.data() + 256 };

    // Driven until the write actually fails rather than for a fixed number of
    // blocks: how long a given filesystem takes to fill is a property of the
    // machine, and a fixed count is the timing assumption that has broken four
    // assertions in this repository already.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);

    while (! coordinator.hasCardWriteFailed()
           && std::chrono::steady_clock::now() < deadline)
    {
        coordinator.pullOutputBlock (outs, 2, 256);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    check (coordinator.hasCardWriteFailed(),
           "the writer notices that the drive stopped accepting audio");

    const auto problem = coordinator.getCardWriteProblem();
    std::printf ("  Reported: '%s'\n", problem.c_str());

    check (! problem.empty(),
           "and gives an account of it rather than leaving the caller to guess");

    // The point of the whole exercise. Without an account the take stops under
    // the card-removal notice, whose words are "stopped responding" and "check
    // that it is plugged in properly" -- advice that cannot be followed,
    // because nothing is unplugged.
    check (problem.find ("full") != std::string::npos,
           "naming the drive as full");
    check (problem.find ("plugged in") == std::string::npos,
           "and not sending the user to check a cable that was never loose");

    // §10.6: what happened, then what to do.
    check (problem.find ("Free up space") != std::string::npos
               || problem.find ("bigger card") != std::string::npos,
           "and saying what to do about it");

    coordinator.stopRecording();
    coordinator.stopMonitoring();

    std::printf ("%s (%d failing)\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
