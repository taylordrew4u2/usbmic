// Runs the real CoreAudioBackend against a virtual CoreAudio HAL.
//
// The backend source is compiled unmodified; only the OS headers are replaced
// (Simulation/CoreAudio). Every scenario here is a shape of real hardware that
// the backend once got wrong, or that it must keep getting right.

#include "../Simulation/CoreAudio/FakeCoreAudio.h"
#include "../Source/Core/SampleFormat.h"
#include "../Source/Platform/CoreAudioBackend.h"

#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <unistd.h>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

// ---------------------------------------------------------------------------
// The timing budgets these tests run on, as one stated relationship rather
// than the same magic numbers repeated at a dozen call sites.
//
// Every "cannot freeze launch" check answers one question: when a HAL call
// hangs, does the caller bound itself, or does it wait for the hang? It can
// only answer that if the two outcomes are far apart. They were not. The stall
// was 250 ms and the budget 200 ms, so a correct answer and a wrong one were
// 50 ms apart -- and the macOS runners add well over 100 ms of scheduling
// latency of their own. The same closeAllStreams call measured 94 ms, then
// 144 ms, then 162 ms across three runs of one commit; the third crossed the
// budget and failed a release build on a commit that had just passed CI.
//
// So the stall moved, not the budget alone. A bounded call returns in tens of
// milliseconds and is allowed the better part of a second; a call that really
// waits takes two full seconds. Nothing in between is ambiguous.
// ---------------------------------------------------------------------------

/// How long a simulated HAL call hangs for.
constexpr int kStuckHalMilliseconds = 2000;

/// What a correctly bounded call may take. kHalTransactionTimeout is 75 ms in
/// simulation, so this is roughly five times the worst latency yet seen on a
/// loaded runner, and still well under half the stall above.
constexpr auto kBoundedReturn = std::chrono::milliseconds (800);

/// A call refused without touching the HAL does no waiting at all; this only
/// has to stay far below the stall.
constexpr auto kNoHalRoundTrip = std::chrono::milliseconds (200);

/// Waiting for the worker that owns a stuck HAL call through to completion.
/// It cannot finish before the stall does, so this must exceed it.
constexpr int kWorkerSettleMilliseconds = 6000;

void check (bool condition, const std::string& what)
{
    ++checks;
    if (condition)
    {
        std::printf ("  PASS  %s\n", what.c_str());
    }
    else
    {
        ++failures;
        std::printf ("  FAIL  %s\n", what.c_str());
    }
}

std::vector<float> ramp (int frames, float scale)
{
    std::vector<float> v (static_cast<size_t> (frames));
    for (int i = 0; i < frames; ++i)
        v[static_cast<size_t> (i)] = scale * static_cast<float> (i);
    return v;
}

/// Captures whatever the backend hands its AudioCallback, so a scenario can
/// assert on the audio rather than on the absence of a crash.
struct Capture
{
    int callbackCount = 0;
    int lastChannelCount = 0;
    int lastFrameCount = 0;
    std::vector<std::vector<float>> lastBlock;

    mma::AudioCallback callback()
    {
        return [this] (const float* const* inputs, int numInputs,
                       float* const*, int, int numSamples)
        {
            ++callbackCount;
            lastChannelCount = numInputs;
            lastFrameCount = numSamples;
            lastBlock.assign (static_cast<size_t> (numInputs), {});

            for (int ch = 0; ch < numInputs; ++ch)
                if (inputs[ch] != nullptr)
                    lastBlock[static_cast<size_t> (ch)].assign (inputs[ch], inputs[ch] + numSamples);
        };
    }
};

fakeca::DeviceSpec microphone (const std::string& name, const std::string& uid,
                               int channels, fakeca::BufferShape shape)
{
    fakeca::DeviceSpec spec;
    spec.name = name;
    spec.uid = uid;
    spec.inputChannels = channels;
    spec.shape = shape;
    return spec;
}

fakeca::DeviceSpec headphones (const std::string& name, const std::string& uid,
                               int channels, fakeca::BufferShape shape)
{
    fakeca::DeviceSpec spec;
    spec.name = name;
    spec.uid = uid;
    spec.outputChannels = channels;
    spec.shape = shape;
    return spec;
}

// --- Scenarios --------------------------------------------------------------

/// The defect that made a stereo USB microphone record silence: the IOProc
/// discarded any buffer that was not exactly one channel, so the callback never
/// fired at all and nothing reported an error.
void interleavedStereoMicrophoneDeliversBothChannels()
{
    std::printf ("\nA stereo USB mic that hands over one interleaved buffer\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (microphone ("Interleaved Mic", "uid-interleaved", 2,
                                                   fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-interleaved", 48000.0, 256, capture.callback()),
           "the stream opens");
    check (fakeca::isRunning (id), "the IOProc is started");

    const std::vector<std::vector<float>> signal { ramp (128, 1.0f), ramp (128, -1.0f) };
    fakeca::pumpInput (id, signal);

    check (capture.callbackCount == 1, "the audio callback actually fires");
    check (capture.lastChannelCount == 2, "both channels reach the callback");
    check (capture.lastFrameCount == 128, "the frame count survives de-interleaving");

    bool exact = capture.lastChannelCount == 2;
    for (int ch = 0; ch < capture.lastChannelCount && exact; ++ch)
        exact = capture.lastBlock[static_cast<size_t> (ch)] == signal[static_cast<size_t> (ch)];

    check (exact, "each channel carries its own samples, not the other's");
    backend.closeAllStreams();
}

/// The shape the backend always handled. Kept so a fix for the interleaved case
/// cannot quietly break the one that used to work.
void oneChannelPerBufferStillWorks()
{
    std::printf ("\nA mic that hands over one buffer per channel\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (microphone ("Split Mic", "uid-split", 2,
                                                   fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-split", 48000.0, 256, capture.callback()), "the stream opens");

    const std::vector<std::vector<float>> signal { ramp (64, 2.0f), ramp (64, 3.0f) };
    fakeca::pumpInput (id, signal);

    check (capture.lastChannelCount == 2, "both channels reach the callback");
    check (capture.lastBlock == signal, "the samples pass through unchanged");
    backend.closeAllStreams();
}

/// Playback had the same defect, and it is the half a recording test would
/// never catch: the monitor mix would simply be silent.
void interleavedOutputCarriesTheMonitorMix()
{
    std::printf ("\nAn interface whose output is one interleaved buffer\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (headphones ("Interleaved Out", "uid-out", 2,
                                                   fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend;

    // Writes a different constant per channel, so a packing error shows up as
    // the wrong value in the wrong place rather than as silence.
    auto writer = [] (const float* const*, int, float* const* outputs, int numOutputs, int numSamples)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            for (int i = 0; i < numSamples; ++i)
                outputs[ch][i] = 0.25f * static_cast<float> (ch + 1);
    };

    check (backend.openExclusiveOutputStream ("uid-out", 48000.0, 256, writer), "the output stream opens");

    std::vector<std::vector<float>> written;
    check (fakeca::pumpOutput (id, 96, written), "the IOProc runs");
    check (written.size() == 2, "both output channels are written");

    bool correct = written.size() == 2;
    for (size_t ch = 0; ch < written.size() && correct; ++ch)
        for (float v : written[ch])
            if (std::fabs (v - 0.25f * static_cast<float> (ch + 1)) > 1.0e-6f)
            {
                correct = false;
                break;
            }

    check (correct, "each channel gets its own signal, correctly re-interleaved");
    backend.closeAllStreams();
}

/// A device with a sample-rate converter advertises one continuous range rather
/// than a list. Reading only its maximum hid every rate in between, so §2.2
/// negotiation would route around a device that supports 48 kHz perfectly well.
void continuousSampleRateRangeIsExpanded()
{
    std::printf ("\nA device advertising a continuous 44.1-96 kHz range\n");
    fakeca::reset();

    auto spec = microphone ("Range Mic", "uid-range", 1, fakeca::BufferShape::oneChannelPerBuffer);
    spec.rateRanges = { { 44100.0, 96000.0 } };
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    const auto devices = backend.enumerateInputDevices();

    check (devices.size() == 1, "the device enumerates");

    if (devices.empty())
        return;

    const auto& rates = devices.front().supportedSampleRates;
    auto has = [&rates] (uint32_t r) { return std::find (rates.begin(), rates.end(), r) != rates.end(); };

    check (has (44100) && has (48000) && has (88200) && has (96000),
           "every standard rate inside the range is reported");
    check (! has (192000), "a rate outside the range is not");

    // §2.2 stays on the rate the hardware is already using, and it can only do
    // that if the backend says what that rate is. Nothing checked this, so a
    // field left at 0 would have silently reverted the whole rule to
    // highest-common -- which is exactly how a fix for a 44.1 kHz interface
    // shipped unable to fire on it.
    std::printf ("  current rate reported: %u Hz\n", devices.front().currentSampleRate);
    check (devices.front().currentSampleRate == 48000,
           "and the rate the device is running at RIGHT NOW is reported, not left at 0");
}

/// Another process holding the device makes the rate write fail even when the
/// device already runs at exactly the rate we want. Treating that as fatal
/// turned a working microphone into one that would not open.
void aDeviceAlreadyAtTheRequestedRateStillOpens()
{
    std::printf ("\nA mic whose rate is fixed at the rate we want anyway\n");
    fakeca::reset();

    auto spec = microphone ("Fixed Mic", "uid-fixed", 1, fakeca::BufferShape::oneChannelPerBuffer);
    spec.allowRateChange = false;
    spec.currentRate = 48000.0;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-fixed", 48000.0, 256, capture.callback()),
           "the stream opens despite the refused write");
    check (fakeca::isRunning (id), "and actually starts");
    backend.closeAllStreams();
}

/// CoreAudio acknowledges a nominal-rate write before the new value is always
/// visible. The backend must wait for that asynchronous change rather than
/// rejecting a device on the first stale read-back.
void aDelayedRateChangeSettlesBeforeTheStreamOpens()
{
    std::printf ("\nA nominal-rate change that CoreAudio applies asynchronously\n");
    fakeca::reset();

    auto spec = microphone ("Slow Rate Mic", "uid-slow-rate", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.currentRate = 44100.0;
    spec.rateRanges = { { 44100.0, 44100.0 }, { 48000.0, 48000.0 } };
    // One stale read, not three.
    //
    // The claim here is that a rate the HAL applies asynchronously is waited
    // for rather than read back once and treated as a refusal -- and a single
    // stale read proves exactly that: the first read after the write still
    // returns 44.1, so an implementation without the confirmation poll still
    // fails this test. Three reads proved nothing further and cost three poll
    // intervals, which is what made this the one test that failed on macOS.
    //
    // The poll is 10 ms inside a 50 ms settle window, so five polls are
    // available -- and that window sits inside the 75 ms HAL transaction
    // deadline for the whole open, so the real budget is smaller still.
    // Needing four of those five left nothing for a sleep that overshoots,
    // which is what sleep_until does on the macOS runners: the open was
    // refused at 51 ms with the correct rate one read away. Widening the
    // settle window is not the fix -- it has to stay inside the transaction
    // that contains it, the way production's 500 ms sits inside 750 ms.
    spec.rateChangeDelayReads = 1;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;

    // Measured, not just asserted. This test is the one that fails on macOS and
    // passes everywhere else, and a bare FAIL line says nothing about which of
    // the two bounded waits ran out -- the rate-settle poll or the HAL
    // transaction deadline around the whole open. The numbers separate them.
    const auto openBegan = std::chrono::steady_clock::now();
    const bool opened = backend.openInputStream ("uid-slow-rate", 48000.0, 256,
                                                 capture.callback());
    const auto openTook = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - openBegan);

    std::printf ("  [measured] the open took %lld ms; the device now reads %.0f Hz\n",
                 (long long) openTook.count(), fakeca::nominalRate (id));

    if (! opened)
        std::printf ("  [measured] refused with: %s\n", backend.getLastOpenError().c_str());

    check (opened, "the stream waits for the delayed rate and opens");
    check (fakeca::nominalRate (id) == 48000.0,
           "the stream starts only after the requested rate is visible");
    check (fakeca::isRunning (id), "the IOProc actually starts after confirmation");
    backend.closeAllStreams();
}

/// A broken driver can acknowledge the property write without applying it.
/// Waiting forever would hang launch, so the confirmation path has a 500 ms
/// ceiling and still refuses to open at the wrong rate when that expires.
void aRateChangeThatNeverSettlesTimesOut()
{
    std::printf ("\nA nominal-rate write that never becomes visible\n");
    fakeca::reset();

    auto spec = microphone ("Never Settles", "uid-never-settles", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.currentRate = 44100.0;
    spec.rateRanges = { { 44100.0, 44100.0 }, { 48000.0, 48000.0 } };
    spec.rateChangeDelayReads = 100000;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;
    const auto started = std::chrono::steady_clock::now();
    const bool opened = backend.openInputStream ("uid-never-settles", 48000.0, 256,
                                                 capture.callback());
    const auto elapsed = std::chrono::steady_clock::now() - started;

    check (! opened, "the open is refused rather than using the stale rate");
    check (! fakeca::isRunning (id), "no IOProc starts at the wrong rate");

    // This device never applies the rate, so its worker keeps polling for the
    // whole settle window after the open has already given up. Waiting for it
    // here is not politeness: the next test calls fakeca::reset(), and the
    // harness has no lock, so a worker still reading a device while the next
    // test replaces it is a genuine race -- ThreadSanitizer reports it the
    // moment the settle window outlasts the gap between the two tests. Every
    // other test that abandons a worker already waits for it exactly here.
    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the abandoned rate-confirmation worker settles before the next test");
    check (elapsed < std::chrono::milliseconds (650),
           "confirmation returns at its 500 ms bound instead of hanging launch");
}

/// The reported rig: an interface sitting at 44.1 kHz. The backend must say so,
/// because §2.2's whole "stay put" rule is built on that one number.
void aDeviceAt44100ReportsThatAsItsCurrentRate()
{
    std::printf ("\nAn interface running at 44.1 kHz\n");
    fakeca::reset();

    auto spec = microphone ("PUP Mixer", "uid-pup", 2, fakeca::BufferShape::interleaved);
    spec.rateRanges = { { 44100.0, 48000.0 } };
    spec.currentRate = 44100.0;
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    const auto devices = backend.enumerateInputDevices();

    check (devices.size() == 1, "the interface enumerates");

    if (devices.empty())
        return;

    std::printf ("  current rate reported: %u Hz\n", devices.front().currentSampleRate);
    check (devices.front().currentSampleRate == 44100,
           "and reports 44.1 kHz, which is what lets the take stay there");
}

/// The converse: a device that cannot reach the requested rate must fail rather
/// than run at the wrong one, which would be a silent, permanent drift source.
void aDeviceThatCannotReachTheRateIsRefused()
{
    std::printf ("\nA mic stuck at 44.1 kHz when 48 kHz was negotiated\n");
    fakeca::reset();

    auto spec = microphone ("Stuck Mic", "uid-stuck", 1, fakeca::BufferShape::oneChannelPerBuffer);
    spec.allowRateChange = false;
    spec.currentRate = 44100.0;
    spec.rateRanges = { { 44100.0, 44100.0 } };
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;

    check (! backend.openInputStream ("uid-stuck", 48000.0, 256, capture.callback()),
           "the open is refused rather than silently running at the wrong rate");

    // §0.1: refusing is only half the job. "Couldn't be opened for recording"
    // sends someone hunting through cables for a fault that is one number in a
    // settings pane, and the app knew the number the whole time.
    const auto reason = backend.getLastOpenError();
    std::printf ("  reason: %s\n", reason.c_str());

    check (! reason.empty(), "and says why, rather than leaving the user to guess");
    check (reason.find ("44.1 kHz") != std::string::npos,
           "naming the rate the interface is actually running at");
    check (reason.find ("48 kHz") != std::string::npos,
           "and the rate the recording wants");
}

/// A microphone unplugged between being listed and being opened. The message
/// has to say THAT, not offer a sample-rate lecture about a device that is no
/// longer there.
void aMicrophoneThatVanishedSaysSo()
{
    std::printf ("\nA mic that was unplugged between being listed and being opened\n");
    fakeca::reset();

    mma::CoreAudioBackend backend;
    Capture capture;

    check (! backend.openInputStream ("uid-gone", 48000.0, 256, capture.callback()),
           "the open is refused");

    const auto reason = backend.getLastOpenError();
    std::printf ("  reason: %s\n", reason.c_str());

    check (reason.find ("no longer connected") != std::string::npos,
           "and says the microphone is gone, not that some rate is wrong");
}

/// §5.4: the monitor path is exclusive or it is nothing. Reporting success
/// without hog mode handed the user a shared output while the app believed
/// otherwise.
void hogModeRefusalFailsTheOpenAndExplainsItself()
{
    std::printf ("\nAn output that will not grant exclusive use\n");
    fakeca::reset();

    auto spec = headphones ("Shared Out", "uid-shared", 2, fakeca::BufferShape::oneChannelPerBuffer);
    spec.allowHogMode = false;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;

    const auto capability = backend.checkExclusiveModeCapability ("uid-shared", 48000.0, 256);
    check (! capability.exclusiveModeAvailable, "the preflight reports it as unavailable");
    check (! capability.unavailableReason.empty(), "and names a cause");

    check (! backend.openExclusiveOutputStream ("uid-shared", 48000.0, 256,
                                                [] (const float* const*, int, float* const*, int, int) {}),
           "the open fails rather than claiming an exclusive path");
    check (! backend.getLastOpenError().empty(), "and leaves a message naming a next step");
    check (! fakeca::isRunning (id), "no IOProc is left running behind the failure");
}

/// The normal case, and the one that proves the hog-mode check is a gate rather
/// than a blanket refusal.
void hogModeIsTakenAndReleased()
{
    std::printf ("\nAn output that does grant exclusive use\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (headphones ("Exclusive Out", "uid-excl", 2,
                                                   fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;

    check (backend.openExclusiveOutputStream ("uid-excl", 48000.0, 256,
                                              [] (const float* const*, int, float* const*, int, int) {}),
           "the open succeeds");
    check (fakeca::hogModeHeld (id), "hog mode is actually held while open");

    // "Somebody holds it" is not the §5.4 claim. Exclusive monitoring means
    // THIS process holds it, and a backend that wrote a bogus or stale pid
    // would satisfy the line above while owning nothing it could release.
    check (fakeca::hogOwnerPid (id) == static_cast<int> (getpid()),
           "and it is this process that holds it, not merely someone");

    backend.closeAllStreams();
    check (! fakeca::hogModeHeld (id), "and released on close, so other apps get the device back");
    check (fakeca::hogOwnerPid (id) == -1, "with the owner cleared, not just changed");
}

/// Re-checking an output this app already holds must report it as available.
/// The hog-mode property reports an owning pid, and "owned by us" and "owned by
/// someone else" are the same value shape -- so telling them apart is the
/// difference between a working monitor and a false "another app has it".
void anOutputWeAlreadyHoldIsStillReportedAsAvailable()
{
    std::printf ("\nRe-checking an output this app already holds\n");
    fakeca::reset();

    fakeca::addDevice (headphones ("Held Out", "uid-held", 2, fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;

    check (backend.openExclusiveOutputStream ("uid-held", 48000.0, 256,
                                              [] (const float* const*, int, float* const*, int, int) {}),
           "the output opens and takes hog mode");

    const auto capability = backend.checkExclusiveModeCapability ("uid-held", 48000.0, 256);
    check (capability.exclusiveModeAvailable,
           "a second check sees our own hog mode as ours, not as another app's");

    backend.closeAllStreams();
}

/// §5.4: the latency the preflight reports is the round trip, in and out.
///
/// Every backend computes this and nothing read it: CaptureCoordinator dropped
/// it, so Application::measuredLatencyMs was never assigned by anything and the
/// Advanced panel reported monitoring latency as "0.0 ms" -- not a small
/// number, an impossible one -- while every take's session.json recorded 0.0
/// for good. Now that it reaches the user, the three platforms have to agree on
/// what it means. ALSA counted a single buffer and reported half.
void theReportedLatencyIsTheRoundTrip()
{
    std::printf ("\nWhat the preflight says the monitor path costs\n");
    fakeca::reset();

    fakeca::addDevice (headphones ("Latency Out", "uid-lat", 2,
                                   fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;

    const auto cap = backend.checkExclusiveModeCapability ("uid-lat", 48000.0, 256);

    check (cap.exclusiveModeAvailable, "the output is capable");
    check (cap.measuredOrEstimatedLatencyMs > 0.0,
           "and the preflight reports a latency rather than leaving it at zero");

    const double oneBuffer = (256.0 / 48000.0) * 1000.0;
    check (std::abs (cap.measuredOrEstimatedLatencyMs - oneBuffer * 2.0) < 1e-9,
           "which is the round trip -- one buffer in, one buffer out");

    // It has to follow the buffer size, or it is a constant dressed as a
    // measurement.
    const auto small = backend.checkExclusiveModeCapability ("uid-lat", 48000.0, 128);
    const auto large = backend.checkExclusiveModeCapability ("uid-lat", 48000.0, 512);

    check (std::abs (small.measuredOrEstimatedLatencyMs
                         - cap.measuredOrEstimatedLatencyMs / 2.0) < 1e-9,
           "halving the buffer halves it");
    check (std::abs (large.measuredOrEstimatedLatencyMs
                         - cap.measuredOrEstimatedLatencyMs * 2.0) < 1e-9,
           "and doubling the buffer doubles it");
}

/// §5.4: the preflight must answer the question the open will ask.
///
/// It asked only about hog mode, so a fixed-rate 44.1 kHz interface -- ordinary
/// hardware, and most USB mics -- was reported as ready for exclusive
/// monitoring and then refused the open with a rate-mismatch message. The
/// preflight exists so the user hears that while there is still time to act on
/// it, not at the top of a take.
///
/// This is the third platform to have a defect in checkExclusiveModeCapability:
/// WASAPI probed a single channel layout and called capable devices incapable,
/// ALSA accepted only "hw:" names and refused every card its own picker
/// offered, and this one promised what it could not deliver. Different
/// polarities, one root -- the capability check not asking what the open asks.
void aFixedRateOutputIsNotPromisedForMonitoring()
{
    std::printf ("\nAn output that cannot run at the take's sample rate\n");
    fakeca::reset();

    auto spec = headphones ("Fixed 44k1 Out", "uid-44k1", 2,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.rateRanges = { { 44100.0, 44100.0 } };
    spec.currentRate = 44100.0;
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;

    const auto capability = backend.checkExclusiveModeCapability ("uid-44k1", 48000.0, 256);
    check (! capability.exclusiveModeAvailable,
           "the preflight refuses it rather than promising monitoring");
    check (capability.unavailableReason.find ("44.1 kHz") != std::string::npos,
           "and names the rate the interface is actually running at");
    check (capability.unavailableReason.find ("48 kHz") != std::string::npos,
           "and the rate the recording wants, so both halves of the fix are visible");

    // The preflight and the open must agree. A refusal that the open would have
    // allowed is its own bug, and this is what says the two are answering the
    // same question.
    check (! backend.openExclusiveOutputStream ("uid-44k1", 48000.0, 256,
                                                [] (const float* const*, int, float* const*, int, int) {}),
           "and the open refuses it too, so preflight and open agree");
}

/// The control, and the half that keeps the fix from being a blanket refusal: a
/// device that can do the rate is still offered. Without this, returning
/// "unavailable" unconditionally would pass the case above and turn monitoring
/// off for everybody.
void anOutputThatSupportsTheRateIsStillOffered()
{
    std::printf ("\nAn output that can run at the take's sample rate\n");
    fakeca::reset();

    // Discrete: the device lists exactly the rate asked for.
    auto exact = headphones ("48k Out", "uid-48k", 2, fakeca::BufferShape::oneChannelPerBuffer);
    exact.rateRanges = { { 48000.0, 48000.0 } };
    exact.currentRate = 48000.0;
    fakeca::addDevice (exact);

    // Continuous: a device with a sample-rate converter advertises a span, and
    // 48000 sits inside it without ever appearing as one of its endpoints.
    // querySupportedSampleRates walks common rates through the span for exactly
    // this reason, and a capability check that only compared endpoints would
    // refuse this device.
    auto span = headphones ("44k1-96k Out", "uid-span", 2, fakeca::BufferShape::oneChannelPerBuffer);
    span.rateRanges = { { 44100.0, 96000.0 } };
    span.currentRate = 44100.0;
    fakeca::addDevice (span);

    mma::CoreAudioBackend backend;

    const auto exactCap = backend.checkExclusiveModeCapability ("uid-48k", 48000.0, 256);
    check (exactCap.exclusiveModeAvailable, "a device listing the rate is offered");
    check (exactCap.unavailableReason.empty(), "with no reason attached to a yes");

    const auto spanCap = backend.checkExclusiveModeCapability ("uid-span", 48000.0, 256);
    check (spanCap.exclusiveModeAvailable,
           "and so is one whose continuous range merely contains the rate");

    check (backend.openExclusiveOutputStream ("uid-48k", 48000.0, 256,
                                              [] (const float* const*, int, float* const*, int, int) {}),
           "and the open agrees with the yes");
}

/// A preflight may only say no when it is sure. A device whose rate list comes
/// back empty is a property read that told us nothing, not a device that
/// supports nothing -- refusing on that would turn one unreadable property into
/// no monitoring at all, and the open is the authority either way.
void anOutputThatReportsNoRatesIsNotRefused()
{
    std::printf ("\nAn output whose rate list cannot be read\n");
    fakeca::reset();

    auto spec = headphones ("Silent About Rates", "uid-norates", 2,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.rateRanges = {};
    spec.currentRate = 48000.0;
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;

    const auto capability = backend.checkExclusiveModeCapability ("uid-norates", 48000.0, 256);
    check (capability.exclusiveModeAvailable,
           "an unreadable rate list is not treated as a refusal");
}

/// §2: hotplug arrives from the OS, never from a timer. The backend registers a
/// property listener, so adding a device must reach it without anything polling.
void hotplugArrivesThroughTheOsListener()
{
    std::printf ("\nA mic plugged in after launch\n");
    fakeca::reset();

    mma::CoreAudioBackend backend;
    int notifications = 0;
    backend.setDeviceChangeCallback ([&notifications] { ++notifications; });

    fakeca::addDevice (microphone ("Late Mic", "uid-late", 1, fakeca::BufferShape::oneChannelPerBuffer));
    check (notifications >= 1, "the backend is told, with no timer involved");

    check (backend.enumerateInputDevices().size() == 1, "and the device is there when it re-enumerates");

    const auto before = notifications;
    fakeca::removeDevice (100);
    check (notifications > before, "unplugging notifies too");
}

/// H11: another app can change a live interface's nominal rate without adding
/// or removing a device. The per-device property listener must catch it, name
/// both rates, and hand the change back to the normal re-enumeration path.
void aLiveSampleRateChangeIsReportedAndReenumerated()
{
    std::printf ("\nA live interface whose sample rate another app changes\n");
    fakeca::reset();

    auto spec = microphone ("Rate Change Mic", "uid-rate-change", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.rateRanges = { { 44100.0, 44100.0 }, { 48000.0, 48000.0 } };
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;
    int deviceChanges = 0;
    backend.setDeviceChangeCallback ([&deviceChanges] { ++deviceChanges; });

    check (backend.openInputStream ("uid-rate-change", 48000.0, 256, capture.callback()),
           "the stream opens at 48 kHz");
    check (fakeca::propertyListenerCount (id) == 3,
           "nominal-rate, device-alive, and processor-overload listeners are installed");

    check (fakeca::setNominalRateExternally (id, 44100.0),
           "Audio MIDI Setup changes the live device to 44.1 kHz");

    const auto first = backend.takeStreamFailures();
    check (first.size() == 1, "the mismatch is reported on the next message-thread poll");

    if (! first.empty())
    {
        check (first.front().deviceId == "uid-rate-change",
               "the report identifies the affected microphone");
        check (first.front().kind == mma::StreamFailureKind::sampleRateChanged,
               "the event is typed so the app can stop a take without parsing prose");
        check (first.front().reason.find ("44.1 kHz") != std::string::npos
               && first.front().reason.find ("48 kHz") != std::string::npos,
               "the report names both the new hardware rate and SobStage's rate");
    }

    check (deviceChanges == 1,
           "the property event is bridged into the ordinary re-enumeration callback");
    check (backend.takeStreamFailures().empty(),
           "an unchanged mismatch is not repeated on every status poll");

    fakeca::setNominalRateExternally (id, 48000.0);
    check (backend.takeStreamFailures().empty(),
           "returning to the stream rate clears the mismatch without a false failure");

    fakeca::setNominalRateExternally (id, 44100.0);
    check (backend.takeStreamFailures().size() == 1,
           "a later, separate rate mismatch can be reported again");

    backend.closeAllStreams();
    check (fakeca::propertyListenerCount (id) == 0,
           "closing the stream removes every per-device listener");
}

/// H11: kAudioDevicePropertyDeviceIsAlive reaches the backend before the
/// system device list necessarily drops the AudioObjectID. This must report a
/// dead stream immediately rather than waiting five seconds for the IOProc
/// watchdog, and recovery must release the one-shot latch.
void aDeviceAliveChangeIsReportedImmediately()
{
    std::printf ("\nA live AudioObject that becomes unusable before disappearing\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (microphone ("Dying Mic", "uid-dying", 1,
                                                   fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;
    Capture capture;
    check (backend.openInputStream ("uid-dying", 48000.0, 256, capture.callback()),
           "the stream opens");

    check (fakeca::setDeviceAlive (id, false),
           "the HAL marks the still-present device unusable");

    const auto dead = backend.takeStreamFailures();
    check (dead.size() == 1, "the device-alive listener reports it without a five-second wait");

    if (! dead.empty())
    {
        check (dead.front().deviceId == "uid-dying", "the dead-device report names the mic");
        check (dead.front().kind == mma::StreamFailureKind::deviceUnavailable,
               "the event is typed for app-level dropout handling");
        check (dead.front().reason.find ("no longer available") != std::string::npos,
               "the reason describes an unavailable device, not generic silence");
    }

    check (backend.takeStreamFailures().empty(),
           "the same dead state is reported once");
    check (backend.enumerateInputDevices().empty(),
           "an alive=false AudioObject is omitted before the system list removes it");

    fakeca::setDeviceAlive (id, true);
    check (backend.takeStreamFailures().empty(),
           "the same AudioObject can recover without a false failure");
    check (backend.enumerateInputDevices().size() == 1,
           "a revived AudioObject returns to enumeration");

    fakeca::setDeviceAlive (id, false);
    check (backend.takeStreamFailures().size() == 1,
           "a second genuine death after recovery is reported again");
    backend.closeAllStreams();
}

/// Processor-overload is normally notified from CoreAudio's IO thread. Input
/// overruns must be surfaced as possible take loss; output overruns feed the
/// monitor-glitch counter and must not be mislabelled as recorded-audio loss.
void processorOverloadsAreCountedOnTheRightPath()
{
    std::printf ("\nCoreAudio processor-overload events on input and output\n");
    fakeca::reset();

    const auto input = fakeca::addDevice (microphone ("Busy Mic", "uid-busy-in", 1,
                                                       fakeca::BufferShape::oneChannelPerBuffer));
    const auto output = fakeca::addDevice (headphones ("Busy Out", "uid-busy-out", 2,
                                                        fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend;
    Capture capture;
    check (backend.openInputStream ("uid-busy-in", 48000.0, 256, capture.callback()),
           "the input stream opens");
    check (backend.openExclusiveOutputStream (
               "uid-busy-out", 48000.0, 256,
               [] (const float* const*, int, float* const*, int, int) {}),
           "the output stream opens");

    fakeca::fireProcessorOverload (input);
    fakeca::fireProcessorOverload (input);

    const auto inputFailures = backend.takeStreamFailures();
    check (inputFailures.size() == 1,
           "one poll reports the accumulated input deadline misses once");
    check (! inputFailures.empty()
           && inputFailures.front().kind == mma::StreamFailureKind::processorOverload
           && inputFailures.front().reason.find ("audio processing deadline") != std::string::npos,
           "the input warning is typed and explains possible recording loss");
    check (backend.getOutputGlitchCount() == 0,
           "input overloads do not inflate the monitor-glitch counter");
    check (backend.takeStreamFailures().empty(),
           "input overloads are not repeated without a new event");

    fakeca::fireProcessorOverload (output);
    check (backend.getOutputGlitchCount() == 1,
           "an output deadline miss increments the monitor-glitch counter");
    check (backend.takeStreamFailures().empty(),
           "an output glitch is not also reported as microphone loss");

    backend.closeAllStreams();
    check (fakeca::propertyListenerCount (input) == 0
           && fakeca::propertyListenerCount (output) == 0,
           "both streams remove their listeners during teardown");
}

/// Processor-overload listeners normally execute on CoreAudio's IO thread
/// while the message thread reads the counter. Exercise that actual overlap so
/// ThreadSanitizer verifies the listener shares only atomics with the reader.
void processorOverloadNotificationIsThreadSafe()
{
    std::printf ("\nProcessor-overload callbacks racing the message-thread reader\n");
    fakeca::reset();

    const auto output = fakeca::addDevice (headphones ("Threaded Out", "uid-threaded-out", 2,
                                                        fakeca::BufferShape::interleaved));

    mma::CoreAudioBackend backend;
    check (backend.openExclusiveOutputStream (
               "uid-threaded-out", 48000.0, 256,
               [] (const float* const*, int, float* const*, int, int) {}),
           "the output stream opens");

    constexpr uint64_t eventCount = 2000;
    std::atomic<bool> producerDone { false };

    std::thread producer ([&]
    {
        for (uint64_t i = 0; i < eventCount; ++i)
            fakeca::fireProcessorOverload (output);

        producerDone.store (true, std::memory_order_release);
    });

    uint64_t observed = 0;
    while (! producerDone.load (std::memory_order_acquire))
        observed = std::max (observed, backend.getOutputGlitchCount());

    producer.join();
    observed = std::max (observed, backend.getOutputGlitchCount());

    check (observed == eventCount,
           "every IO-thread overload is retained while the message thread reads");
    backend.closeAllStreams();
}

/// A third-party driver may refuse one or every listener. Continuing in total
/// silence would recreate H11 on that hardware, so the open remains compatible
/// but the missing safety watch is reported once.
void aDeviceThatRefusesSafetyListenersSaysSo()
{
    std::printf ("\nA device driver that refuses per-device safety listeners\n");
    fakeca::reset();

    fakeca::addDevice (microphone ("Opaque Mic", "uid-opaque", 1,
                                   fakeca::BufferShape::oneChannelPerBuffer));
    fakeca::setPropertyListenersAllowed (false);

    mma::CoreAudioBackend backend;
    Capture capture;
    check (backend.openInputStream ("uid-opaque", 48000.0, 256, capture.callback()),
           "the unusual device remains usable");

    const auto problem = backend.takeStreamFailures();
    check (problem.size() == 1, "the missing safety listeners are not silent");
    check (! problem.empty()
           && problem.front().kind == mma::StreamFailureKind::safetyMonitoringUnavailable
           && problem.front().reason.find ("can't report every sample-rate") != std::string::npos,
           "the report is typed and names the safety information that is unavailable");
    check (backend.takeStreamFailures().empty(),
           "the listener problem is reported once rather than on every poll");
    backend.closeAllStreams();
}

/// A microphone that stops sending audio after it was opened -- the HAL keeps
/// the stream, the IOProc simply never runs again. Nothing about that is
/// visible from anywhere else in the app, so if the watchdog does not report
/// it, a take goes on recording silence for that channel and says nothing.
void aMicrophoneThatGoesQuietAfterOpeningIsReported()
{
    std::printf ("\nA mic whose IOProc stops running after it opened\n");
    fakeca::reset();

    const auto id = fakeca::addDevice (microphone ("Quiet Mic", "uid-quiet", 1,
                                                   fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-quiet", 48000.0, 256, capture.callback()),
           "the mic opens");

    // One callback, so the stream is known to have been alive; then nothing.
    fakeca::pumpInput (id, { std::vector<float> (256, 0.25f) });

    check (backend.takeStreamFailures().empty(), "a mic that just delivered audio is not accused");

    // The watchdog waits 5 s before calling a stream dead, because the HAL may
    // legitimately pause around a device or format change.
    std::this_thread::sleep_for (std::chrono::milliseconds (5400));

    const auto reported = backend.takeStreamFailures();
    check (reported.size() == 1, "the silent stream is reported once");

    if (! reported.empty())
    {
        std::printf ("  reason: %s\n", reported.front().reason.c_str());
        check (reported.front().deviceId == "uid-quiet", "and names the microphone it happened to");
        check (reported.front().reason.find ("stopped sending audio") != std::string::npos,
               "and says what the user can do about it");
    }

    check (backend.takeStreamFailures().empty(),
           "and is not repeated on every poll for as long as it stays dead");

    // Audio arriving again clears the latch, so a device that recovers and
    // dies a second time is reported a second time.
    fakeca::pumpInput (id, { std::vector<float> (256, 0.25f) });
    std::this_thread::sleep_for (std::chrono::milliseconds (5400));

    check (backend.takeStreamFailures().size() == 1, "a second death is reported again");
}

/// AudioDeviceStart can return success without the driver ever invoking its
/// IOProc. A watchdog based only on the most recent callback sees the initial
/// zero timestamp and skips that stream forever, leaving a take silently empty.
void aMicrophoneWhoseFirstCallbackNeverArrivesIsReported()
{
    std::printf ("\nA mic whose first IOProc never arrives\n");
    fakeca::reset();

    fakeca::addDevice (microphone ("Never Started Mic", "uid-never-callback", 1,
                                  fakeca::BufferShape::oneChannelPerBuffer));

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-never-callback", 48000.0, 256, capture.callback()),
           "the HAL reports that the mic started");
    check (backend.takeStreamFailures().empty(),
           "the first callback receives the same five-second grace period");

    std::this_thread::sleep_for (std::chrono::milliseconds (5400));

    const auto reported = backend.takeStreamFailures();
    check (reported.size() == 1, "a missing first callback is reported once");

    if (! reported.empty())
    {
        check (reported.front().deviceId == "uid-never-callback",
               "the first-callback failure names the microphone");
        check (reported.front().reason.find ("stopped sending audio") != std::string::npos,
               "the first-callback failure gives the same recovery guidance");
    }

    check (backend.takeStreamFailures().empty(),
           "the missing first callback is not repeated on every poll");
}

/// Some class-compliant USB drivers block the caller inside AudioDeviceStart
/// for minutes. The app window is created only after audio initialisation, so
/// an unbounded call makes the whole application appear never to launch.
void aStuckInputStartIsBoundedAndCleanedUp()
{
    std::printf ("\nA USB mic whose HAL start call stalls\n");
    fakeca::reset();

    const auto outputId = fakeca::addDevice (headphones (
        "Working Output", "uid-working-output", 2,
        fakeca::BufferShape::oneChannelPerBuffer));
    const auto workingInputId = fakeca::addDevice (microphone (
        "Working Mic", "uid-working-input", 1,
        fakeca::BufferShape::oneChannelPerBuffer));

    auto spec = microphone ("Stuck Start Mic", "uid-stuck-start", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.startDelayMilliseconds = kStuckHalMilliseconds;
    spec.callbackBeforeStartReturns = true;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture, workingCapture;
    std::atomic<int> callbacksAfterOwnerRelease { 0 };
    auto callbackOwner = std::make_shared<int> (1);
    std::weak_ptr<int> weakCallbackOwner = callbackOwner;
    auto lifetimeCheckedCallback = [&callbacksAfterOwnerRelease, weakCallbackOwner] (
        const float* const*, int, float* const*, int, int)
    {
        if (weakCallbackOwner.expired())
            callbacksAfterOwnerRelease.fetch_add (1, std::memory_order_relaxed);
    };
    check (backend.openExclusiveOutputStream (
               "uid-working-output", 48000.0, 256,
               [] (const float* const*, int, float* const*, int, int) {}),
           "a monitor output is already live before the failure");
    check (backend.openInputStream (
               "uid-working-input", 48000.0, 256, workingCapture.callback()),
           "another microphone is already live before the failure");

    const auto began = std::chrono::steady_clock::now();

    check (! backend.openInputStream (spec.uid, 48000.0, 256, lifetimeCheckedCallback),
           "the backend stops waiting for the stalled input");
    callbackOwner.reset();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - began);
    check (elapsed < kBoundedReturn,
           "the simulated launch remains bounded well below the HAL delay");
    check (backend.getLastOpenError().find ("took too long") != std::string::npos,
           "the failure explains that macOS timed out rather than blaming a cable silently");

    const auto retryBegan = std::chrono::steady_clock::now();
    check (! backend.openInputStream (spec.uid, 48000.0, 256, capture.callback()),
           "device churn does not launch a second stuck attempt for the same input");
    check (std::chrono::steady_clock::now() - retryBegan < kNoHalRoundTrip,
           "the duplicate attempt is rejected without touching the HAL again");

    const auto closeBegan = std::chrono::steady_clock::now();
    backend.closeAllStreams();
    const auto closeTook = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - closeBegan);

    // 200 ms, matching every sibling bound in this file rather than the 100 ms
    // this check alone carried. closeAllStreams() is DESIGNED to wait out
    // kHalTransactionTimeout (75 ms in simulation) whenever the HAL is stuck,
    // so 100 ms left 25 ms for scheduling jitter -- and this check has been
    // passing and failing run to run on the macOS runners because of it.
    // What the check is really for is unchanged: 200 ms is still far below the
    // 250 ms stall, so a close that genuinely waits for the stuck HAL to finish
    // still fails here.
    std::printf ("  [measured] closeAllStreams returned in %lld ms (stall is %d ms)\n",
                 (long long) closeTook.count(), spec.startDelayMilliseconds);
    check (closeTook < kBoundedReturn,
           "previously-open streams are quarantined without blocking behind the stuck HAL");

    // Synchronizes with the detached owner after Start returns and it performs
    // Stop/listener removal/Destroy. This is also what makes the TSan scenario
    // a real lifetime proof rather than a sleep that merely tends to pass.
    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the abandoned worker eventually completes its own cleanup");
    check (! fakeca::isRunning (id), "the late-started IOProc is stopped");
    check (callbacksAfterOwnerRelease.load (std::memory_order_relaxed) == 0,
           "a callback that begins behind the timeout gate cannot reach its former owner");
    check (! fakeca::isRunning (workingInputId) && ! fakeca::isRunning (outputId),
           "the quarantined healthy streams are stopped by their owning worker");
    check (fakeca::propertyListenerCount (id) == 0,
           "no listener points at the retired stream");
    check (fakeca::propertyListenerCount (workingInputId) == 0
           && fakeca::propertyListenerCount (outputId) == 0,
           "the quarantined streams also remove every listener");
}

void aDriverThatRetainsListenerClientDataIsQuarantined()
{
    std::printf ("\nA driver that refuses to release listener clientData\n");
    fakeca::reset();

    auto spec = microphone ("Unsafe Teardown Mic", "uid-unsafe-teardown", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.allowPropertyListenerRemoval = false;
    fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;
    check (backend.openInputStream (spec.uid, 48000.0, 256, capture.callback()),
           "the unusual device opens before teardown");
    backend.closeAllStreams();

    const auto retryBegan = std::chrono::steady_clock::now();
    check (! backend.openInputStream (spec.uid, 48000.0, 256, capture.callback()),
           "a retained listener keeps the device quarantined");
    check (std::chrono::steady_clock::now() - retryBegan < kNoHalRoundTrip,
           "the quarantine refuses a retry without touching the unsafe driver");
}

/// This is the ordering that matters after a launch timeout: the abandoned
/// input discovers that CoreAudio retained listener clientData, while an
/// already-live stream is queued for asynchronous teardown. Successful cleanup
/// of that other stream must never erase the abandoned input's sticky quarantine.
void aTimedOutUnsafeInputCannotBeUnquarantinedByOtherCleanup()
{
    std::printf ("\nA timed-out input whose listener clientData is retained\n");
    fakeca::reset();

    fakeca::addDevice (headphones (
        "Working Output", "uid-sticky-output", 2,
        fakeca::BufferShape::oneChannelPerBuffer));

    auto spec = microphone ("Stuck Unsafe Mic", "uid-stuck-unsafe", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.startDelayMilliseconds = kStuckHalMilliseconds;
    spec.allowPropertyListenerRemoval = false;
    const auto inputId = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;
    check (backend.openExclusiveOutputStream (
               "uid-sticky-output", 48000.0, 256,
               [] (const float* const*, int, float* const*, int, int) {}),
           "a healthy stream is live before the unsafe timeout");
    check (! backend.openInputStream (spec.uid, 48000.0, 256, capture.callback()),
           "the unsafe input still observes the bounded open deadline");

    backend.closeAllStreams();

    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the timed-out worker and queued healthy cleanup both settle behind a sticky quarantine");
    check (fakeca::propertyListenerCount (inputId) == 3,
           "retained listener clientData keeps its inert stream storage alive");

    const auto retryBegan = std::chrono::steady_clock::now();
    check (! backend.openInputStream (spec.uid, 48000.0, 256, capture.callback()),
           "successful cleanup of another stream cannot reopen the unsafe device gate");
    check (std::chrono::steady_clock::now() - retryBegan < kNoHalRoundTrip,
           "the sticky quarantine rejects the retry without another HAL transaction");
}

void anInputPropertyCallCannotFreezeLaunch()
{
    std::printf ("\nA USB mic whose UID property call stalls\n");
    fakeca::reset();

    auto spec = microphone ("Stuck Property Mic", "uid-stuck-property", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.uidReadDelayMilliseconds = kStuckHalMilliseconds;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;
    const auto began = std::chrono::steady_clock::now();
    check (! backend.openInputStream (spec.uid, 48000.0, 256, capture.callback()),
           "a property call before IOProc creation observes the same open deadline");
    check (std::chrono::steady_clock::now() - began < kBoundedReturn,
           "the stalled property query cannot hold the launch thread");
    check (backend.getLastOpenError().find ("took too long") != std::string::npos,
           "the bounded property failure explains that macOS timed out");
    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the property worker eventually settles under its own lifetime");
    check (! fakeca::isRunning (id) && fakeca::propertyListenerCount (id) == 0,
           "a late property result cannot leave an IOProc or listener behind");
}

void stuckOutputCreateAndStartCallsAreBounded()
{
    std::printf ("\nOutput IOProc creation and start calls that stall\n");

    for (const bool stallCreate : { true, false })
    {
        fakeca::reset();
        auto spec = headphones (stallCreate ? "Stuck Create Out" : "Stuck Start Out",
                                stallCreate ? "uid-stuck-create-out" : "uid-stuck-start-out", 2,
                                fakeca::BufferShape::interleaved);
        if (stallCreate)
            spec.createDelayMilliseconds = kStuckHalMilliseconds;
        else
            spec.startDelayMilliseconds = kStuckHalMilliseconds;
        const auto id = fakeca::addDevice (spec);

        mma::CoreAudioBackend backend;
        const auto began = std::chrono::steady_clock::now();
        check (! backend.openExclusiveOutputStream (
                   spec.uid, 48000.0, 256,
                   [] (const float* const*, int, float* const*, int, int) {}),
               stallCreate ? "a stalled output CreateIOProc is bounded"
                           : "a stalled output Start is bounded");
        check (std::chrono::steady_clock::now() - began < kBoundedReturn,
               "the bad output cannot freeze launch");
        check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
               "the abandoned output worker owns cleanup through completion");
        check (! fakeca::isRunning (id),
               "the abandoned output has no running IOProc");
        check (! fakeca::hogModeHeld (id),
               "the abandoned output gives hog mode back");
        check (fakeca::propertyListenerCount (id) == 0,
               "the abandoned output removes all clientData listeners");
    }
}

void stuckStopAndDestroyCannotFreezeClose()
{
    std::printf ("\nOutput Stop and Destroy calls that stall\n");

    for (const bool stallStop : { true, false })
    {
        fakeca::reset();
        auto spec = headphones (stallStop ? "Stuck Stop Out" : "Stuck Destroy Out",
                                stallStop ? "uid-stuck-stop-out" : "uid-stuck-destroy-out", 2,
                                fakeca::BufferShape::interleaved);
        if (stallStop)
            spec.stopDelayMilliseconds = kStuckHalMilliseconds;
        else
            spec.destroyDelayMilliseconds = kStuckHalMilliseconds;
        const auto id = fakeca::addDevice (spec);

        mma::CoreAudioBackend backend;
        check (backend.openExclusiveOutputStream (
                   spec.uid, 48000.0, 256,
                   [] (const float* const*, int, float* const*, int, int) {}),
               "the output opens before the teardown fault");

        const auto began = std::chrono::steady_clock::now();
        backend.closeAllStreams();
        check (std::chrono::steady_clock::now() - began < kBoundedReturn,
               stallStop ? "a stalled Stop cannot freeze close"
                         : "a stalled Destroy cannot freeze close");
        check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
               "the detached close owner eventually finishes the HAL teardown");
        check (! fakeca::isRunning (id) && ! fakeca::hogModeHeld (id),
               "late teardown stops the IOProc and releases hog mode");
        check (fakeca::propertyListenerCount (id) == 0,
               "late teardown removes every clientData listener");
    }
}

void cleanupThreadCreationFailureRetainsInertClientData()
{
    std::printf ("\nA timed-out rig cannot create its detached cleanup owner\n");
    fakeca::reset();

    auto spec = microphone ("Cleanup Thread Failure", "uid-cleanup-thread-failure", 1,
                            fakeca::BufferShape::oneChannelPerBuffer);
    const auto id = fakeca::addDevice (spec);
    auto stuckSpec = microphone ("Timed-out Before Cleanup", "uid-timeout-before-cleanup", 1,
                                 fakeca::BufferShape::oneChannelPerBuffer);
    stuckSpec.startDelayMilliseconds = kStuckHalMilliseconds;
    fakeca::addDevice (stuckSpec);

    mma::CoreAudioBackend backend;
    std::atomic<int> callbacksAfterOwnerRelease { 0 };
    auto owner = std::make_shared<int> (1);
    std::weak_ptr<int> weakOwner = owner;
    check (backend.openInputStream (
               spec.uid, 48000.0, 256,
               [&callbacksAfterOwnerRelease, weakOwner] (
                   const float* const*, int, float* const*, int, int)
               {
                   if (weakOwner.expired())
                       callbacksAfterOwnerRelease.fetch_add (1, std::memory_order_relaxed);
               }),
           "the input is live before cleanup ownership cannot be transferred");

    Capture stuckCapture;
    check (! backend.openInputStream (
               stuckSpec.uid, 48000.0, 256, stuckCapture.callback()),
           "another interface times out before the rig is retired");

    backend.failNextCleanupWorkerStartForTesting();
    backend.closeAllStreams();
    owner.reset();

    check (backend.waitForPendingInputAttemptsForTesting (kWorkerSettleMilliseconds),
           "the original timed-out worker eventually completes independently");

    check (fakeca::isRunning (id) && fakeca::propertyListenerCount (id) == 3,
           "live HAL registrations retain their closed-gate storage instead of dangling");
    check (fakeca::pumpInput (id, { { 0.5f, 0.25f } }),
           "the simulated HAL can still call the retained IOProc");
    check (callbacksAfterOwnerRelease.load (std::memory_order_relaxed) == 0,
           "the retained IOProc is inert after its callback owner is gone");

    Capture retryCapture;
    check (! backend.openInputStream (spec.uid, 48000.0, 256, retryCapture.callback()),
           "the failed cleanup permanently quarantines further HAL transactions");
}

void systemListenerClientDataSurvivesFailedRemoval()
{
    std::printf ("\nA HAL that retains system-listener clientData\n");
    fakeca::reset();
    fakeca::setSystemPropertyListenerRemovalAllowed (false);

    std::atomic<int> callsAfterOwnerRelease { 0 };
    auto owner = std::make_shared<int> (1);
    std::weak_ptr<int> weakOwner = owner;
    {
        mma::CoreAudioBackend backend;
        backend.setDeviceChangeCallback ([&callsAfterOwnerRelease, weakOwner]
        {
            if (weakOwner.expired())
                callsAfterOwnerRelease.fetch_add (1, std::memory_order_relaxed);
        });
        check (fakeca::systemPropertyListenerCount() == 1,
               "the system hot-plug listener is installed");
        backend.setDeviceChangeCallback (nullptr);
        check (! backend.getHotplugProblem().empty(),
               "the refused listener removal is checked and reported");
    }

    owner.reset();
    fakeca::fireDeviceListChange();
    check (fakeca::systemPropertyListenerCount() == 1,
           "the broken HAL really did retain the raw clientData");
    check (callsAfterOwnerRelease.load (std::memory_order_relaxed) == 0,
           "a retained system listener is inert after backend destruction");
}

void destructionDrainsAnActiveSystemListener()
{
    std::printf ("\nBackend destruction racing an active hot-plug callback\n");
    fakeca::reset();

    auto backend = std::make_unique<mma::CoreAudioBackend>();
    std::atomic<bool> callbackEntered { false };
    std::atomic<bool> releaseCallback { false };
    std::atomic<bool> destructionFinished { false };

    backend->setDeviceChangeCallback ([&]
    {
        callbackEntered.store (true, std::memory_order_release);
        while (! releaseCallback.load (std::memory_order_acquire))
            std::this_thread::yield();
    });

    std::thread notifier ([] { fakeca::fireDeviceListChange(); });
    while (! callbackEntered.load (std::memory_order_acquire))
        std::this_thread::yield();

    std::thread destroyer ([&]
    {
        backend.reset();
        destructionFinished.store (true, std::memory_order_release);
    });

    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    check (! destructionFinished.load (std::memory_order_acquire),
           "destruction waits for the callback lease already in flight");
    releaseCallback.store (true, std::memory_order_release);
    notifier.join();
    destroyer.join();
    check (destructionFinished.load (std::memory_order_acquire),
           "destruction completes after the active callback drains");

    fakeca::fireDeviceListChange();
    check (fakeca::systemPropertyListenerCount() == 0,
           "successful removal leaves no callback for later notifications");
}

/// A Mac that refuses the device-list listener leaves the app deaf to the rig:
/// a microphone plugged in is never noticed, and one pulled out MID-TAKE is
/// never reported, so a take that lost a channel looks like a clean one. The
/// result of installing the listener used to be discarded entirely.
void aMacThatWillNotWatchTheRigSaysSo()
{
    std::printf ("\nA Mac that refuses the device-list listener\n");
    fakeca::reset();

    {
        mma::CoreAudioBackend backend;
        backend.setDeviceChangeCallback ([] {});

        check (backend.getHotplugProblem().empty(),
               "a Mac that CAN watch the rig reports no problem");
    }

    fakeca::setPropertyListenersAllowed (false);

    {
        mma::CoreAudioBackend backend;
        backend.setDeviceChangeCallback ([] {});

        check (! backend.getHotplugProblem().empty(),
               "a Mac that cannot watch the rig says so rather than going quiet");
    }

    fakeca::setPropertyListenersAllowed (true);
}

/// The HAL is allowed to hand the IOProc a larger slice than the nominal buffer.
/// The scratch is sized at open time (§11 forbids allocating in the callback),
/// so this checks the headroom is real rather than nominal.
void aLargerThanRequestedCallbackIsStillDelivered()
{
    std::printf ("\nA device that delivers more frames than the nominal buffer\n");
    fakeca::reset();

    auto spec = microphone ("Big Block Mic", "uid-big", 2, fakeca::BufferShape::interleaved);
    spec.bufferFrameSize = 256;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-big", 48000.0, 256, capture.callback()), "the stream opens");

    fakeca::pumpInput (id, { ramp (512, 1.0f), ramp (512, 2.0f) });
    check (capture.lastFrameCount == 512, "a double-size block is de-interleaved rather than dropped");
    backend.closeAllStreams();
}

/// If one interleaved buffer is too large for the preallocated scratch, the
/// backend must not let a later valid buffer impersonate one of its channels.
void anOversizedBufferKeepsItsPhysicalChannelSlots()
{
    std::printf ("\nAn oversized interleaved buffer followed by a valid mono buffer\n");
    fakeca::reset();

    auto spec = microphone ("Mixed Buffer Mic", "uid-mixed-big", 3,
                            fakeca::BufferShape::oneChannelPerBuffer);
    spec.bufferFrameSize = 256;
    const auto id = fakeca::addDevice (spec);

    mma::CoreAudioBackend backend;
    Capture capture;

    check (backend.openInputStream ("uid-mixed-big", 48000.0, 256, capture.callback()),
           "the mixed-layout stream opens");

    constexpr int frames = 4097; // one beyond the backend's fixed scratch capacity
    std::vector<float> oversizedStereo (static_cast<size_t> (frames) * 2, 0.5f);
    std::vector<float> trailingMono (static_cast<size_t> (frames), 0.75f);

    check (fakeca::pumpInputBuffers (id, { { 2, std::move (oversizedStereo) },
                                           { 1, trailingMono } }),
           "the virtual HAL delivers the mixed AudioBufferList");
    check (capture.lastChannelCount == 3,
           "the dropped stereo buffer still occupies its two physical slots");
    check (capture.lastBlock.size() == 3
           && capture.lastBlock[0].empty()
           && capture.lastBlock[1].empty(),
           "the oversized channels are explicit null placeholders");
    check (capture.lastBlock.size() == 3
           && capture.lastBlock[2] == trailingMono,
           "the later mono buffer remains physical channel three");
    check (backend.getFramesDroppedByBackend() == frames,
           "the oversized buffer is still reported as dropped");
    backend.closeAllStreams();
}

/// Eight microphones is the §1 ceiling, and the shape most likely to expose a
/// scratch-sizing or channel-indexing error that two devices would not.
void eightMicrophonesEachKeepTheirOwnAudio()
{
    std::printf ("\nEight interleaved stereo mics at once (the §1 ceiling)\n");
    fakeca::reset();

    mma::CoreAudioBackend backend;
    std::vector<AudioObjectID> ids;
    std::vector<std::unique_ptr<Capture>> captures;

    for (int i = 0; i < 8; ++i)
    {
        const auto uid = "uid-mic-" + std::to_string (i);
        ids.push_back (fakeca::addDevice (microphone ("Mic " + std::to_string (i), uid, 2,
                                                      fakeca::BufferShape::interleaved)));
        captures.push_back (std::make_unique<Capture>());

        if (! backend.openInputStream (uid, 48000.0, 256, captures.back()->callback()))
        {
            check (false, "mic " + std::to_string (i) + " opens");
            return;
        }
    }

    check (true, "all eight open");

    // Each mic gets a distinct constant; a crossed pointer shows up as the
    // wrong mic's value rather than as silence.
    for (int i = 0; i < 8; ++i)
    {
        const float value = 0.1f * static_cast<float> (i + 1);
        fakeca::pumpInput (ids[static_cast<size_t> (i)],
                           { std::vector<float> (64, value), std::vector<float> (64, -value) });
    }

    bool separated = true;
    for (int i = 0; i < 8 && separated; ++i)
    {
        const auto& c = *captures[static_cast<size_t> (i)];
        const float expected = 0.1f * static_cast<float> (i + 1);

        separated = c.callbackCount == 1 && c.lastChannelCount == 2
                 && std::fabs (c.lastBlock[0][0] - expected) < 1.0e-6f
                 && std::fabs (c.lastBlock[1][0] + expected) < 1.0e-6f;
    }

    check (separated, "every mic's audio arrives on its own stream, none crossed");
    backend.closeAllStreams();
}

} // namespace

void onlyDirectlyAttachedHardwareEnumeratesAsInput()
{
    std::printf ("\nOnly directly attached hardware appears as a microphone\n");
    fakeca::reset();

    const auto addInput = [] (const char* name, const char* uid, UInt32 transport)
    {
        auto spec = microphone (name, uid, 1, fakeca::BufferShape::oneChannelPerBuffer);
        spec.transportType = transport;
        fakeca::addDevice (spec);
    };

    addInput ("USB interface", "usb", kAudioDeviceTransportTypeUSB);
    addInput ("FireWire interface", "firewire", kAudioDeviceTransportTypeFireWire);
    addInput ("Thunderbolt interface", "thunderbolt", kAudioDeviceTransportTypeThunderbolt);

    addInput ("Mac microphone", "built-in", kAudioDeviceTransportTypeBuiltIn);
    addInput ("iPhone wired", "phone-wired", kAudioDeviceTransportTypeContinuityCaptureWired);
    addInput ("iPhone wireless", "phone-wireless", kAudioDeviceTransportTypeContinuityCaptureWireless);
    addInput ("Older iPhone", "phone-legacy", kAudioDeviceTransportTypeContinuityCapture);
    addInput ("Bluetooth microphone", "bluetooth", kAudioDeviceTransportTypeBluetooth);
    addInput ("Bluetooth LE microphone", "bluetooth-le", kAudioDeviceTransportTypeBluetoothLE);
    addInput ("AirPlay input", "airplay", kAudioDeviceTransportTypeAirPlay);
    addInput ("Network microphone", "avb", kAudioDeviceTransportTypeAVB);
    addInput ("Internal sound card", "pci", kAudioDeviceTransportTypePCI);
    addInput ("Virtual cable", "virtual", kAudioDeviceTransportTypeVirtual);
    addInput ("User aggregate", "aggregate", kAudioDeviceTransportTypeAggregate);
    addInput ("Unknown source", "unknown", 0);
    addInput ("SobStage aggregate", "com.multimicaggregator.combined",
              kAudioDeviceTransportTypeAggregate);

    mma::CoreAudioBackend backend;
    const auto inputs = backend.enumerateInputDevices();

    check (inputs.size() == 3, "built-in, phone, wireless, virtual, and unknown inputs are excluded");

    bool usb = false, firewire = false, thunderbolt = false, unexpected = false;
    for (const auto& d : inputs)
    {
        usb |= d.usbLocationId == "usb";
        firewire |= d.usbLocationId == "firewire";
        thunderbolt |= d.usbLocationId == "thunderbolt";
        unexpected |= d.usbLocationId != "usb"
                   && d.usbLocationId != "firewire"
                   && d.usbLocationId != "thunderbolt";
    }

    check (usb && firewire && thunderbolt && ! unexpected,
           "USB, FireWire, and Thunderbolt inputs remain available");

    auto speakers = headphones ("Mac speakers", "speakers", 2,
                                fakeca::BufferShape::oneChannelPerBuffer);
    speakers.transportType = kAudioDeviceTransportTypeBuiltIn;
    fakeca::addDevice (speakers);

    auto wirelessHeadphones = headphones ("Bluetooth headphones", "headphones", 2,
                                          fakeca::BufferShape::interleaved);
    wirelessHeadphones.transportType = kAudioDeviceTransportTypeBluetooth;
    fakeca::addDevice (wirelessHeadphones);

    const auto outputs = backend.enumerateOutputDevices();
    check (outputs.size() == 2,
           "the input policy does not hide built-in or wireless monitor outputs");
}

void openingAnInputRechecksTheExternalHardwarePolicy()
{
    std::printf ("\nOpening a disallowed CoreAudio input directly\n");
    fakeca::reset();

    auto phone = microphone ("Continuity phone", "phone-direct-open", 1,
                             fakeca::BufferShape::oneChannelPerBuffer);
    phone.transportType = kAudioDeviceTransportTypeContinuityCaptureWired;
    const auto id = fakeca::addDevice (phone);

    mma::CoreAudioBackend backend;
    Capture capture;
    check (! backend.openInputStream (phone.uid, 48000.0, 256, capture.callback()),
           "a caller cannot bypass discovery and open a Continuity input by UID");
    check (! fakeca::isRunning (id),
           "the rejected input never starts an IOProc");
    check (backend.getLastOpenError().find ("directly connected external") != std::string::npos,
           "the refusal explains the external-hardware policy");
}

void bitDepthFollowsWhatTheDeviceCanActuallyDeliver()
{
    // §2.3, which had no implementation at all until the depth was made to
    // follow the hardware: "do not upconvert -- it adds file size and no
    // information". §14.1's own hardware is the case that costs, and it is the
    // case a file-backed ALSA fixture cannot express, because that plugin
    // accepts every format asked of it. A simulated HAL can simply say 16.
    std::printf ("\nBit depth follows the device (§2.3)\n");
    fakeca::reset();

    auto yeti = microphone ("Blue Yeti", "uid-yeti", 1, fakeca::BufferShape::oneChannelPerBuffer);
    yeti.bitDepths = { 16 };                 // §14.1: verified 16-bit hardware
    fakeca::addDevice (yeti);

    auto iface = microphone ("Scarlett", "uid-iface", 2, fakeca::BufferShape::oneChannelPerBuffer);
    iface.bitDepths = { 16, 24 };
    fakeca::addDevice (iface);

    auto silentOnTheSubject = microphone ("No Streams", "uid-nostream", 1, fakeca::BufferShape::oneChannelPerBuffer);
    silentOnTheSubject.bitDepths = {};       // a device with nothing to ask
    fakeca::addDevice (silentOnTheSubject);

    mma::CoreAudioBackend backend;
    const auto devices = backend.enumerateInputDevices();

    const auto depthsOf = [&devices] (const std::string& name) {
        for (const auto& d : devices)
            if (d.name == name)
                return d.supportedBitDepths;
        return std::vector<int>{};
    };

    const auto yetiDepths = depthsOf ("Blue Yeti");
    check (yetiDepths == std::vector<int> { 16 },
           "a 16-bit microphone reports 16 and not the old hardcoded list");
    check (mma::SampleFormat::chooseRecordingBitDepth (yetiDepths, 24) == 16,
           "so its stem is written at 16, not padded out to 24 for nothing");

    const auto ifaceDepths = depthsOf ("Scarlett");
    check (mma::SampleFormat::chooseRecordingBitDepth (ifaceDepths, 24) == 24,
           "an interface that can do 24 still gets 24");

    // The safe direction. A device the HAL cannot be asked about must not be
    // read as a limited one: empty means "not reported", and guessing low here
    // would quietly halve the depth of a recording.
    check (depthsOf ("No Streams").empty(),
           "a device with no stream to ask reports nothing rather than guessing");
    check (mma::SampleFormat::chooseRecordingBitDepth (depthsOf ("No Streams"), 24) == 24,
           "and that falls back to the take's depth, unchanged");
}

int main()
{
    std::printf ("CoreAudio backend, driven against a virtual HAL\n");
    std::printf ("===============================================\n");

    onlyDirectlyAttachedHardwareEnumeratesAsInput();
    openingAnInputRechecksTheExternalHardwarePolicy();
    interleavedStereoMicrophoneDeliversBothChannels();
    oneChannelPerBufferStillWorks();
    interleavedOutputCarriesTheMonitorMix();
    continuousSampleRateRangeIsExpanded();
    aDeviceAlreadyAtTheRequestedRateStillOpens();
    aDelayedRateChangeSettlesBeforeTheStreamOpens();
    aRateChangeThatNeverSettlesTimesOut();
    aDeviceThatCannotReachTheRateIsRefused();
    aMicrophoneThatVanishedSaysSo();
    aDeviceAt44100ReportsThatAsItsCurrentRate();
    hogModeRefusalFailsTheOpenAndExplainsItself();
    hogModeIsTakenAndReleased();
    theReportedLatencyIsTheRoundTrip();
    aFixedRateOutputIsNotPromisedForMonitoring();
    anOutputThatSupportsTheRateIsStillOffered();
    anOutputThatReportsNoRatesIsNotRefused();
    anOutputWeAlreadyHoldIsStillReportedAsAvailable();
    hotplugArrivesThroughTheOsListener();
    aLiveSampleRateChangeIsReportedAndReenumerated();
    aDeviceAliveChangeIsReportedImmediately();
    processorOverloadsAreCountedOnTheRightPath();
    processorOverloadNotificationIsThreadSafe();
    aDeviceThatRefusesSafetyListenersSaysSo();
    aMacThatWillNotWatchTheRigSaysSo();
    aMicrophoneThatGoesQuietAfterOpeningIsReported();
    aMicrophoneWhoseFirstCallbackNeverArrivesIsReported();
    aStuckInputStartIsBoundedAndCleanedUp();
    aDriverThatRetainsListenerClientDataIsQuarantined();
    aTimedOutUnsafeInputCannotBeUnquarantinedByOtherCleanup();
    anInputPropertyCallCannotFreezeLaunch();
    stuckOutputCreateAndStartCallsAreBounded();
    stuckStopAndDestroyCannotFreezeClose();
    cleanupThreadCreationFailureRetainsInertClientData();
    systemListenerClientDataSurvivesFailedRemoval();
    destructionDrainsAnActiveSystemListener();
    aLargerThanRequestedCallbackIsStillDelivered();
    anOversizedBufferKeepsItsPhysicalChannelSlots();
    bitDepthFollowsWhatTheDeviceCanActuallyDeliver();
    eightMicrophonesEachKeepTheirOwnAudio();

    // Tear the last scenario down so a leak check sees only what the
    // backend failed to release, not what the harness never cleaned up.
    fakeca::reset();

    std::printf ("\n%s (%d checks, %d failing)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", checks, failures);
    return failures == 0 ? 0 : 1;
}
