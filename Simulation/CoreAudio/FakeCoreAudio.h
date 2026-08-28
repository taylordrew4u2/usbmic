#pragma once

// The virtual CoreAudio HAL behind the shim headers.
//
// A harness describes devices the way real hardware behaves -- a stereo
// microphone that hands over one interleaved buffer, an interface that
// advertises a continuous 44.1-96 kHz range, a card whose hog mode another
// process already owns -- then drives the backend's IOProc and inspects what
// the backend did with it. The backend source is compiled unmodified.

#include <CoreAudio/CoreAudio.h>

#include <string>
#include <utility>
#include <vector>

namespace fakeca {

/// How the HAL presents a device's channels to the IOProc. Both shapes occur on
/// real hardware and the backend has to handle both; this is the single most
/// important axis the simulation varies, because getting it wrong is silent.
enum class BufferShape
{
    oneChannelPerBuffer, ///< N buffers of 1 channel each.
    interleaved          ///< 1 buffer carrying N interleaved channels.
};

struct DeviceSpec
{
    std::string name;
    std::string uid;

    int inputChannels = 0;
    int outputChannels = 0;
    BufferShape shape = BufferShape::oneChannelPerBuffer;

    /// Rates the device reports. A discrete rate is a range whose ends are
    /// equal; a continuous range has them different, which is what a device
    /// with a sample-rate converter advertises.
    std::vector<std::pair<double, double>> rateRanges { { 48000.0, 48000.0 } };

    double currentRate = 48000.0;

    /// false models a device whose rate is fixed, or one another process holds:
    /// the property write fails. The backend must still succeed when the device
    /// already runs at the requested rate.
    bool allowRateChange = true;

    /// false models a device that will not grant exclusive use -- Bluetooth
    /// output, or a card another process has hogged.
    bool allowHogMode = true;

    int bufferFrameSize = 256;
    bool allowBufferSizeChange = true;
};

/// Clears every device, listener and IOProc. Call between scenarios.
void reset();

AudioObjectID addDevice (const DeviceSpec& spec);

/// Removes a device and fires the system device-list listener, which is what an
/// unplug looks like to the backend (§2: the OS tells us, we never poll).
void removeDevice (AudioObjectID device);

/// True once the backend has called AudioDeviceStart on this device.
bool isRunning (AudioObjectID device);

/// What the device's nominal rate property actually holds now.
double nominalRate (AudioObjectID device);

/// Whether this process currently owns hog mode on the device.
bool hogModeHeld (AudioObjectID device);

/// The buffer frame size the device settled on.
int bufferFrameSize (AudioObjectID device);

/// Delivers one input callback carrying `channels` (per-channel, equal length),
/// packed in whatever shape the device was configured with. Returns false if no
/// IOProc is running on the device.
bool pumpInput (AudioObjectID device, const std::vector<std::vector<float>>& channels);

/// Runs one output callback for `frames` and returns what the backend wrote,
/// de-interleaved back into per-channel vectors. This is the mirror of
/// pumpInput: it proves the backend's playback packing is right, not just that
/// it did not crash.
bool pumpOutput (AudioObjectID device, int frames, std::vector<std::vector<float>>& out);

} // namespace fakeca
