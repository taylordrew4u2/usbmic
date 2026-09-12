// Runs the real CoreAudioBackend against a virtual CoreAudio HAL.
//
// The backend source is compiled unmodified; only the OS headers are replaced
// (Simulation/CoreAudio). Every scenario here is a shape of real hardware that
// the backend once got wrong, or that it must keep getting right.

#include "../Simulation/CoreAudio/FakeCoreAudio.h"
#include "../Source/Platform/CoreAudioBackend.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

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

    backend.closeAllStreams();
    check (! fakeca::hogModeHeld (id), "and released on close, so other apps get the device back");
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

    const auto failures = backend.takeStreamFailures();
    check (failures.size() == 1, "the silent stream is reported once");

    if (! failures.empty())
    {
        std::printf ("  reason: %s\n", failures.front().reason.c_str());
        check (failures.front().deviceId == "uid-quiet", "and names the microphone it happened to");
        check (failures.front().reason.find ("stopped sending audio") != std::string::npos,
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

int main()
{
    std::printf ("CoreAudio backend, driven against a virtual HAL\n");
    std::printf ("===============================================\n");

    onlyDirectlyAttachedHardwareEnumeratesAsInput();
    interleavedStereoMicrophoneDeliversBothChannels();
    oneChannelPerBufferStillWorks();
    interleavedOutputCarriesTheMonitorMix();
    continuousSampleRateRangeIsExpanded();
    aDeviceAlreadyAtTheRequestedRateStillOpens();
    aDeviceThatCannotReachTheRateIsRefused();
    aMicrophoneThatVanishedSaysSo();
    aDeviceAt44100ReportsThatAsItsCurrentRate();
    hogModeRefusalFailsTheOpenAndExplainsItself();
    hogModeIsTakenAndReleased();
    anOutputWeAlreadyHoldIsStillReportedAsAvailable();
    hotplugArrivesThroughTheOsListener();
    aMacThatWillNotWatchTheRigSaysSo();
    aMicrophoneThatGoesQuietAfterOpeningIsReported();
    aLargerThanRequestedCallbackIsStillDelivered();
    eightMicrophonesEachKeepTheirOwnAudio();

    // Tear the last scenario down so a leak check sees only what the
    // backend failed to release, not what the harness never cleaned up.
    fakeca::reset();

    std::printf ("\n%s (%d checks, %d failing)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", checks, failures);
    return failures == 0 ? 0 : 1;
}
