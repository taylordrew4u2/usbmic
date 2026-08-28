#pragma once

// The virtual audio endpoint layer behind the WASAPI shim.
//
// A harness describes endpoints the way real hardware behaves -- a microphone
// that accepts only 24-bit PCM in exclusive mode, an interface that rejects the
// requested period and names its own, a card another process already holds --
// then drives the backend's own worker thread and inspects the audio that comes
// out. The backend source is compiled unmodified.

#include <string>
#include <vector>

namespace fakewasapi {

/// A format the endpoint will accept in exclusive mode. Exclusive mode performs
/// no conversion, so this list is the whole of what the device can do.
struct Format
{
    int channels = 2;
    int containerBits = 32;   ///< 16, 24 or 32
    bool isFloat = false;
    double sampleRate = 48000.0;

    static Format floatFormat (int channels, double rate) { return { channels, 32, true, rate }; }
    static Format pcm (int channels, int bits, double rate) { return { channels, bits, false, rate }; }
};

struct EndpointSpec
{
    std::string id;            ///< the endpoint id string the backend stores (§2.4)
    std::string friendlyName;
    bool isCapture = true;

    /// Everything this endpoint accepts exclusively. Empty means it accepts
    /// nothing, which is what a device already held by another process looks
    /// like to IsFormatSupported.
    std::vector<Format> exclusiveFormats;

    /// What GetMixFormat reports. The backend uses its channel count as a last
    /// negotiation candidate.
    Format mixFormat = Format::floatFormat (2, 48000.0);

    /// false makes Activate fail, which models a device Windows will not hand
    /// over at all.
    bool allowActivate = true;

    /// When set, the first Initialize returns AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
    /// and reports this size, which the backend must retry at.
    int alignedFrames = 0;

    int bufferFrames = 256;
};

void reset();

void addEndpoint (const EndpointSpec& spec);

/// Removes an endpoint and fires the registered IMMNotificationClient, which is
/// what an unplug looks like to the backend.
void removeEndpoint (const std::string& id);

/// Fires OnDeviceAdded without changing the device list, for testing that the
/// backend is driven by the OS rather than by a timer.
void notifyDeviceAdded (const std::string& id);

/// True once the backend has called IAudioClient::Start on this endpoint.
bool isRunning (const std::string& id);

/// The format the backend actually negotiated. Container bits of 0 means no
/// stream was opened.
Format negotiatedFormat (const std::string& id);

/// Whether the endpoint was opened in exclusive mode. §5.4 makes anything else
/// a defect, so this is asserted rather than assumed.
bool openedExclusive (const std::string& id);

/// Delivers one capture packet carrying `channels` (per-channel, equal length),
/// encoded in the endpoint's negotiated format, and blocks until the backend's
/// worker thread has consumed it. Returns false on timeout.
bool pushCapture (const std::string& id, const std::vector<std::vector<float>>& channels);

/// Delivers a packet the device marks AUDCLNT_BUFFERFLAGS_SILENT, whose
/// contents are undefined and must be treated as silence rather than read.
bool pushSilentCapture (const std::string& id, int frames);

/// Runs one render period and returns what the backend wrote, decoded from the
/// negotiated format back into per-channel floats. Returns false on timeout.
bool pullRender (const std::string& id, std::vector<std::vector<float>>& out);

/// Makes RegOpenKeyExA report an ASIO driver as present or absent, which is
/// what decides the backend's ASIO-versus-WASAPI preference.
void setAsioDriverInstalled (bool installed);

} // namespace fakewasapi
