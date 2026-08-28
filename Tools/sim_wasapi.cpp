// Runs the real WasapiAsioBackend against a virtual WASAPI endpoint layer.
//
// The backend source is compiled unmodified; only the OS headers are replaced
// (Simulation/Wasapi). The backend's own worker thread runs for real, so the
// event handshake, the exclusive-mode negotiation and the fixed-point
// conversion are all exercised rather than reasoned about.

#include "../Simulation/Wasapi/FakeWasapi.h"
#include "../Source/Platform/WasapiAsioBackend.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check (bool condition, const std::string& what)
{
    ++checks;
    std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
    if (! condition)
        ++failures;
}

std::vector<float> tone (int frames, float amplitude, float phase)
{
    constexpr float kTwoPi = 6.283185307179586f;
    std::vector<float> v (static_cast<size_t> (frames));

    for (int i = 0; i < frames; ++i)
        v[static_cast<size_t> (i)] =
            amplitude * std::sin (kTwoPi * (static_cast<float> (i) / 64.0f) + phase);

    return v;
}

struct Capture
{
    std::atomic<int> callbackCount { 0 };
    std::mutex mutex;
    std::vector<std::vector<float>> lastBlock;
    int lastChannelCount = 0;

    mma::AudioCallback callback()
    {
        return [this] (const float* const* inputs, int numInputs,
                       float* const*, int, int numSamples)
        {
            std::lock_guard<std::mutex> lock (mutex);
            lastChannelCount = numInputs;
            lastBlock.assign (static_cast<size_t> (numInputs), {});

            for (int ch = 0; ch < numInputs; ++ch)
                lastBlock[static_cast<size_t> (ch)].assign (inputs[ch], inputs[ch] + numSamples);

            callbackCount.fetch_add (1);
        };
    }

    std::vector<std::vector<float>> block()
    {
        std::lock_guard<std::mutex> lock (mutex);
        return lastBlock;
    }
};

fakewasapi::EndpointSpec microphone (const std::string& id, const std::string& name,
                                     std::vector<fakewasapi::Format> formats)
{
    fakewasapi::EndpointSpec spec;
    spec.id = id;
    spec.friendlyName = name;
    spec.isCapture = true;
    spec.exclusiveFormats = std::move (formats);
    return spec;
}

fakewasapi::EndpointSpec headphones (const std::string& id, const std::string& name,
                                     std::vector<fakewasapi::Format> formats)
{
    fakewasapi::EndpointSpec spec;
    spec.id = id;
    spec.friendlyName = name;
    spec.isCapture = false;
    spec.exclusiveFormats = std::move (formats);
    return spec;
}

bool closeEnough (const std::vector<float>& a, const std::vector<float>& b, float tolerance)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
        if (std::fabs (a[i] - b[i]) > tolerance)
            return false;

    return true;
}

// --- Scenarios --------------------------------------------------------------

/// The defect that stopped most USB microphones opening at all: exclusive mode
/// performs no conversion, and the backend offered only float32, so a 24-bit
/// device's IsFormatSupported refused and the mic was simply unusable.
void a24BitOnlyMicrophoneOpensAndDeliversAudio()
{
    std::printf ("\nA 24-bit-only USB microphone (what most of them are)\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-24", "24-bit Mic",
                                         { fakewasapi::Format::pcm (1, 24, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-24", 48000.0, 256, capture.callback()),
           "the stream opens instead of being refused");

    const auto negotiated = fakewasapi::negotiatedFormat ("mic-24");
    check (negotiated.containerBits == 24 && ! negotiated.isFloat,
           "24-bit PCM is what was negotiated");
    check (fakewasapi::openedExclusive ("mic-24"), "and exclusively, per §5.4");

    const auto signal = tone (128, 0.5f, 0.0f);
    check (fakewasapi::pushCapture ("mic-24", { signal }), "a packet reaches the worker thread");
    check (capture.callbackCount.load() == 1, "and the audio callback fires");

    const auto block = capture.block();
    check (block.size() == 1 && closeEnough (block[0], signal, 1.0e-6f),
           "the samples survive the 24-bit round trip");

    backend.closeAllStreams();
}

/// 16-bit is the other common case, and the one where a scale or sign error is
/// loudest.
void a16BitMicrophoneRoundTripsWithinItsQuantisation()
{
    std::printf ("\nA 16-bit microphone\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-16", "16-bit Mic",
                                         { fakewasapi::Format::pcm (1, 16, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-16", 48000.0, 256, capture.callback()), "the stream opens");
    check (fakewasapi::negotiatedFormat ("mic-16").containerBits == 16, "16-bit PCM negotiated");

    const auto signal = tone (128, 0.8f, 1.0f);
    fakewasapi::pushCapture ("mic-16", { signal });

    const auto block = capture.block();
    check (block.size() == 1 && closeEnough (block[0], signal, 2.0f / 32768.0f),
           "the samples survive within one quantisation step");

    backend.closeAllStreams();
}

/// A device that does speak float must still get float: the fallback is a
/// descent, not a downgrade.
void aFloatCapableDeviceStillGetsFloat()
{
    std::printf ("\nAn interface that accepts float32\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-f32", "Float Mic",
                                         { fakewasapi::Format::floatFormat (1, 48000.0),
                                           fakewasapi::Format::pcm (1, 16, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-f32", 48000.0, 256, capture.callback()), "the stream opens");

    const auto negotiated = fakewasapi::negotiatedFormat ("mic-f32");
    check (negotiated.isFloat && negotiated.containerBits == 32,
           "float32 is preferred over the 16-bit fallback it also offers");

    backend.closeAllStreams();
}

/// Many microphones present as stereo only. The backend asks for one channel
/// first and must fall back to the device's own count rather than give up.
void aStereoOnlyMicrophoneIsOpenedAsStereo()
{
    std::printf ("\nA microphone that will only do stereo\n");
    fakewasapi::reset();

    auto spec = microphone ("mic-stereo", "Stereo Only Mic",
                            { fakewasapi::Format::pcm (2, 24, 48000.0) });
    spec.mixFormat = fakewasapi::Format::pcm (2, 24, 48000.0);
    fakewasapi::addEndpoint (spec);

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-stereo", 48000.0, 256, capture.callback()),
           "the stream opens on the device's own channel count");
    check (fakewasapi::negotiatedFormat ("mic-stereo").channels == 2, "two channels negotiated");

    const auto left = tone (96, 0.4f, 0.0f);
    const auto right = tone (96, 0.2f, 2.0f);
    fakewasapi::pushCapture ("mic-stereo", { left, right });

    const auto block = capture.block();
    check (block.size() == 2, "both channels reach the callback");
    check (block.size() == 2 && closeEnough (block[0], left, 1.0e-6f)
                             && closeEnough (block[1], right, 1.0e-6f),
           "and neither channel carries the other's audio");

    backend.closeAllStreams();
}

/// A device that accepts nothing exclusively must fail with an explanation
/// rather than silently falling back to shared mode (§5.4).
void aDeviceThatAcceptsNothingFailsWithAReason()
{
    std::printf ("\nA device that refuses every exclusive format\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-none", "Busy Mic", {}));

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (! backend.openInputStream ("mic-none", 48000.0, 256, capture.callback()),
           "the open fails rather than dropping to shared mode");
    check (! backend.getLastOpenError().empty(), "and leaves a message naming a next step");
    check (! fakewasapi::isRunning ("mic-none"), "with no worker thread left running");
}

/// The device rejects the requested period and names its own; the backend must
/// retry at that size rather than abandon exclusive mode.
void aBufferAlignmentRejectionIsRetried()
{
    std::printf ("\nA device that rejects the period and names its own\n");
    fakewasapi::reset();

    auto spec = microphone ("mic-align", "Picky Mic", { fakewasapi::Format::pcm (1, 32, 48000.0) });
    spec.alignedFrames = 480;
    fakewasapi::addEndpoint (spec);

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-align", 48000.0, 256, capture.callback()),
           "the retry at the device's own size succeeds");
    check (fakewasapi::isRunning ("mic-align"), "and the stream starts");

    backend.closeAllStreams();
}

/// AUDCLNT_BUFFERFLAGS_SILENT means the buffer contents are undefined. Reading
/// it anyway turns a dropout into full-scale noise.
void aSilentFlaggedPacketIsTreatedAsSilence()
{
    std::printf ("\nA packet the device flags as silent\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-silent", "Silent Mic",
                                         { fakewasapi::Format::pcm (1, 16, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture;

    check (backend.openInputStream ("mic-silent", 48000.0, 256, capture.callback()), "the stream opens");
    check (fakewasapi::pushSilentCapture ("mic-silent", 64), "the packet is consumed");

    const auto block = capture.block();
    bool silent = ! block.empty();

    for (const auto& channel : block)
        for (float v : channel)
            if (std::fabs (v) > 1.0e-6f)
                silent = false;

    check (silent, "the callback sees silence, not the undefined bytes");
    backend.closeAllStreams();
}

/// The monitor path. A packing error here is a silent or crossed monitor mix,
/// which no recording test would catch.
void theMonitorMixIsWrittenInTheNegotiatedFormat()
{
    std::printf ("\nA 24-bit stereo monitor output\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (headphones ("out-24", "24-bit Out",
                                         { fakewasapi::Format::pcm (2, 24, 48000.0) }));

    mma::WasapiAsioBackend backend;

    auto writer = [] (const float* const*, int, float* const* outputs, int numOutputs, int numSamples)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            for (int i = 0; i < numSamples; ++i)
                outputs[ch][i] = 0.3f * static_cast<float> (ch + 1);
    };

    check (backend.openExclusiveOutputStream ("out-24", 48000.0, 256, writer), "the output opens");
    check (fakewasapi::openedExclusive ("out-24"), "exclusively");

    std::vector<std::vector<float>> written;
    check (fakewasapi::pullRender ("out-24", written), "a render period completes");
    check (written.size() == 2, "both channels are written");

    bool correct = written.size() == 2;
    for (size_t ch = 0; ch < written.size() && correct; ++ch)
        for (float v : written[ch])
            if (std::fabs (v - 0.3f * static_cast<float> (ch + 1)) > 1.0e-4f)
            {
                correct = false;
                break;
            }

    check (correct, "each channel carries its own signal, correctly encoded and interleaved");
    backend.closeAllStreams();
}

/// §5: an over-range monitor sum must clip rather than wrap. A wrapped integer
/// is full-scale noise in the performer's headphones.
void anOverRangeMonitorSumClipsRatherThanWraps()
{
    std::printf ("\nA monitor sum that runs over full scale\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (headphones ("out-16", "16-bit Out",
                                         { fakewasapi::Format::pcm (2, 16, 48000.0) }));

    mma::WasapiAsioBackend backend;

    auto writer = [] (const float* const*, int, float* const* outputs, int numOutputs, int numSamples)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            for (int i = 0; i < numSamples; ++i)
                outputs[ch][i] = (i % 2 == 0) ? 3.5f : -3.5f;
    };

    check (backend.openExclusiveOutputStream ("out-16", 48000.0, 256, writer), "the output opens");

    std::vector<std::vector<float>> written;
    check (fakewasapi::pullRender ("out-16", written), "a render period completes");

    bool clipped = ! written.empty();
    for (const auto& channel : written)
        for (size_t i = 0; i < channel.size(); ++i)
        {
            const float expected = (i % 2 == 0) ? 1.0f : -1.0f;
            if (std::fabs (channel[i] - expected) > 0.01f)
                clipped = false;
        }

    check (clipped, "every sample lands at the rail, none wrapped to the opposite sign");
    backend.closeAllStreams();
}

/// §2: hotplug comes from the OS. The backend registers an IMMNotificationClient,
/// so a device appearing must reach it with nothing polling.
void hotplugArrivesThroughTheNotificationClient()
{
    std::printf ("\nA mic plugged in after launch\n");
    fakewasapi::reset();

    mma::WasapiAsioBackend backend;
    std::atomic<int> notifications { 0 };
    backend.setDeviceChangeCallback ([&notifications] { notifications.fetch_add (1); });

    fakewasapi::addEndpoint (microphone ("mic-late", "Late Mic",
                                         { fakewasapi::Format::pcm (1, 16, 48000.0) }));

    check (notifications.load() >= 1, "the backend is told, with no timer involved");
    check (backend.enumerateInputDevices().size() == 1, "and the device is there when it re-enumerates");

    const auto before = notifications.load();
    fakewasapi::removeEndpoint ("mic-late");
    check (notifications.load() > before, "unplugging notifies too");

    // Registration is dropped here; the reference count must survive it.
    backend.setDeviceChangeCallback (nullptr);
    check (true, "unregistering the notification client does not crash");
}

/// Enumeration has to report what the endpoints actually say, and keep capture
/// and render apart.
void enumerationReportsNamesAndSeparatesDirections()
{
    std::printf ("\nEnumeration of a mixed set of endpoints\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-a", "Podcast Mic",
                                         { fakewasapi::Format::pcm (1, 24, 48000.0) }));
    fakewasapi::addEndpoint (headphones ("out-a", "Studio Interface",
                                         { fakewasapi::Format::pcm (2, 24, 48000.0) }));

    mma::WasapiAsioBackend backend;

    const auto inputs = backend.enumerateInputDevices();
    const auto outputs = backend.enumerateOutputDevices();

    check (inputs.size() == 1 && outputs.size() == 1, "each direction reports only its own endpoints");
    check (! inputs.empty() && inputs.front().name == "Podcast Mic", "the friendly name is read");
    check (! inputs.empty() && inputs.front().usbLocationId == "mic-a", "the endpoint id is kept as identity");
    check (! inputs.empty() && inputs.front().isMicrophone, "inputs are marked as microphones");
}

/// Eight microphones, the §1 ceiling, each on its own worker thread and each in
/// a different wire format. This is where a shared-state or channel-indexing
/// error surfaces that two devices would hide.
void eightMicrophonesInMixedFormatsStaySeparate()
{
    std::printf ("\nEight mics at once, in four different wire formats\n");
    fakewasapi::reset();

    const std::vector<fakewasapi::Format> formats {
        fakewasapi::Format::pcm (1, 16, 48000.0),
        fakewasapi::Format::pcm (1, 24, 48000.0),
        fakewasapi::Format::pcm (1, 32, 48000.0),
        fakewasapi::Format::floatFormat (1, 48000.0)
    };

    mma::WasapiAsioBackend backend;
    std::vector<std::unique_ptr<Capture>> captures;
    std::vector<std::string> ids;

    for (int i = 0; i < 8; ++i)
    {
        const auto id = "mic-" + std::to_string (i);
        ids.push_back (id);
        fakewasapi::addEndpoint (microphone (id, "Mic " + std::to_string (i),
                                             { formats[static_cast<size_t> (i % 4)] }));
        captures.push_back (std::make_unique<Capture>());

        if (! backend.openInputStream (id, 48000.0, 256, captures.back()->callback()))
        {
            check (false, "mic " + std::to_string (i) + " opens");
            return;
        }
    }

    check (true, "all eight open, each negotiating its own format");

    for (int i = 0; i < 8; ++i)
    {
        const float value = 0.1f * static_cast<float> (i + 1);
        fakewasapi::pushCapture (ids[static_cast<size_t> (i)], { std::vector<float> (64, value) });
    }

    bool separated = true;
    for (int i = 0; i < 8 && separated; ++i)
    {
        const auto block = captures[static_cast<size_t> (i)]->block();
        const float expected = 0.1f * static_cast<float> (i + 1);

        separated = block.size() == 1 && ! block[0].empty()
                 && std::fabs (block[0][0] - expected) < 2.0f / 32768.0f;
    }

    check (separated, "every mic's audio arrives on its own stream, none crossed");
    backend.closeAllStreams();
}

/// Closing must stop the worker threads and release the endpoints. A backend
/// that leaked a running thread would hang here rather than pass.
void closingStopsEveryStream()
{
    std::printf ("\nClosing the streams\n");
    fakewasapi::reset();

    fakewasapi::addEndpoint (microphone ("mic-close", "Mic",
                                         { fakewasapi::Format::pcm (1, 16, 48000.0) }));
    fakewasapi::addEndpoint (headphones ("out-close", "Out",
                                         { fakewasapi::Format::pcm (2, 16, 48000.0) }));

    mma::WasapiAsioBackend backend;
    Capture capture;

    backend.openInputStream ("mic-close", 48000.0, 256, capture.callback());
    backend.openExclusiveOutputStream ("out-close", 48000.0, 256,
                                       [] (const float* const*, int, float* const*, int, int) {});

    check (fakewasapi::isRunning ("mic-close") && fakewasapi::isRunning ("out-close"),
           "both streams are running");

    backend.closeAllStreams();

    check (! fakewasapi::isRunning ("mic-close") && ! fakewasapi::isRunning ("out-close"),
           "and both are stopped, with their worker threads joined");
}

} // namespace

int main()
{
    std::printf ("WASAPI backend, driven against a virtual endpoint layer\n");
    std::printf ("=======================================================\n");

    a24BitOnlyMicrophoneOpensAndDeliversAudio();
    a16BitMicrophoneRoundTripsWithinItsQuantisation();
    aFloatCapableDeviceStillGetsFloat();
    aStereoOnlyMicrophoneIsOpenedAsStereo();
    aDeviceThatAcceptsNothingFailsWithAReason();
    aBufferAlignmentRejectionIsRetried();
    aSilentFlaggedPacketIsTreatedAsSilence();
    theMonitorMixIsWrittenInTheNegotiatedFormat();
    anOverRangeMonitorSumClipsRatherThanWraps();
    hotplugArrivesThroughTheNotificationClient();
    enumerationReportsNamesAndSeparatesDirections();
    eightMicrophonesInMixedFormatsStaySeparate();
    closingStopsEveryStream();

    // Tear the last scenario down so a leak check sees only what the
    // backend failed to release, not what the harness never cleaned up.
    fakewasapi::reset();

    std::printf ("\n%s (%d checks, %d failing)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", checks, failures);
    return failures == 0 ? 0 : 1;
}
