#include "AlsaBackend.h"

#if defined(__linux__) && ! defined(MMA_NO_ALSA)

#include <alsa/asoundlib.h>
#include <sys/inotify.h>
#include <climits>   // NAME_MAX, for the inotify read buffer
#include <unistd.h>
#include <pthread.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

namespace mma {

namespace {

/// §11: the callback must not allocate, so every buffer a stream needs is
/// sized once at open time and reused for the life of the stream.
struct ConversionBuffers
{
    std::vector<unsigned char> interleaved;    // raw frames in the device's format
    std::vector<float> interleavedFloat;       // the same frames as float, still interleaved
    std::vector<float> planar;                 // per-channel, what the callback contract wants
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;
};

int bytesPerSampleFor (snd_pcm_format_t format) noexcept
{
    switch (format)
    {
        case SND_PCM_FORMAT_S16_LE:   return 2;
        case SND_PCM_FORMAT_S32_LE:   return 4;
        case SND_PCM_FORMAT_FLOAT_LE: return 4;
        default:                      return 0;
    }
}

void toFloat (const unsigned char* src, float* dest, snd_pcm_format_t format, size_t count) noexcept
{
    switch (format)
    {
        case SND_PCM_FORMAT_S16_LE:
        {
            const auto* s = reinterpret_cast<const int16_t*> (src);
            for (size_t i = 0; i < count; ++i)
                dest[i] = static_cast<float> (s[i]) / 32768.0f;
            break;
        }
        case SND_PCM_FORMAT_S32_LE:
        {
            const auto* s = reinterpret_cast<const int32_t*> (src);
            for (size_t i = 0; i < count; ++i)
                dest[i] = static_cast<float> (s[i]) / 2147483648.0f;
            break;
        }
        case SND_PCM_FORMAT_FLOAT_LE:
            std::memcpy (dest, src, count * sizeof (float));
            break;
        default:
            std::fill (dest, dest + count, 0.0f);
            break;
    }
}

void fromFloat (const float* src, unsigned char* dest, snd_pcm_format_t format, size_t count) noexcept
{
    switch (format)
    {
        case SND_PCM_FORMAT_S16_LE:
        {
            auto* d = reinterpret_cast<int16_t*> (dest);
            for (size_t i = 0; i < count; ++i)
                d[i] = static_cast<int16_t> (std::max (-1.0f, std::min (1.0f, src[i])) * 32767.0f);
            break;
        }
        case SND_PCM_FORMAT_S32_LE:
        {
            auto* d = reinterpret_cast<int32_t*> (dest);
            for (size_t i = 0; i < count; ++i)
                d[i] = static_cast<int32_t> (std::max (-1.0f, std::min (1.0f, src[i])) * 2147483647.0);
            break;
        }
        case SND_PCM_FORMAT_FLOAT_LE:
            std::memcpy (dest, src, count * sizeof (float));
            break;
        default:
            std::memset (dest, 0, count * bytesPerSampleFor (format));
            break;
    }
}

/// §5.4: only a `hw:` device is handed to one client by the kernel. Everything
/// else routes through dmix/plug, which resamples and mixes -- exactly the
/// silent 40 ms path the spec refuses.
bool isExclusiveCapableName (const std::string& name) noexcept
{
    return name.rfind ("hw:", 0) == 0;
}

/// Best effort: RT scheduling needs privileges this process may not have.
/// Failing is not fatal -- it costs latency headroom, not correctness -- so it
/// is neither retried nor reported as an error.
void requestRealtimePriority() noexcept
{
    sched_param param {};
    param.sched_priority = std::min (80, sched_get_priority_max (SCHED_FIFO));
    pthread_setschedparam (pthread_self(), SCHED_FIFO, &param);
}

} // namespace

struct AlsaStream
{
    snd_pcm_t* pcm = nullptr;
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
    unsigned int channels = 1;
    snd_pcm_uframes_t periodFrames = 0;
    bool isInput = true;

    AudioCallback callback;
    ConversionBuffers buffers;

    std::thread worker;
    std::atomic<bool> running { false };

    ~AlsaStream()
    {
        running.store (false, std::memory_order_release);

        if (worker.joinable())
            worker.join();

        if (pcm != nullptr)
        {
            snd_pcm_drop (pcm);
            snd_pcm_close (pcm);
            pcm = nullptr;
        }
    }
};

/// §2: hotplug from the kernel, never a timer. inotify on /dev/snd fires when
/// a card's device nodes appear or disappear, which is what a USB mic being
/// plugged in actually does.
struct AlsaHotplugWatcher
{
    int fd = -1;
    std::thread worker;
    std::atomic<bool> running { false };

    ~AlsaHotplugWatcher()
    {
        running.store (false, std::memory_order_release);

        // Closing the descriptor is what wakes the blocking read.
        if (fd >= 0)
        {
            ::close (fd);
            fd = -1;
        }

        if (worker.joinable())
            worker.join();
    }
};

AlsaBackend::AlsaBackend() = default;

AlsaBackend::~AlsaBackend()
{
    hotplug.reset();
    closeAllStreams();
}

std::vector<AudioDeviceDescriptor> AlsaBackend::enumerate (bool wantInput) const
{
    std::vector<AudioDeviceDescriptor> result;

    void** hints = nullptr;

    if (snd_device_name_hint (-1, "pcm", &hints) != 0)
        return result;

    const char* wanted = wantInput ? "Input" : "Output";

    for (void** hint = hints; *hint != nullptr; ++hint)
    {
        char* name = snd_device_name_get_hint (*hint, "NAME");
        char* desc = snd_device_name_get_hint (*hint, "DESC");
        char* ioid = snd_device_name_get_hint (*hint, "IOID");

        // A null IOID means the PCM serves both directions.
        const bool matches = (ioid == nullptr) || (std::strcmp (ioid, wanted) == 0);

        if (name != nullptr && matches && std::strcmp (name, "null") != 0)
        {
            AudioDeviceDescriptor d;
            d.name = (desc != nullptr) ? std::string (desc) : std::string (name);

            // The first line of DESC is the human name; the rest is detail that
            // would make a skull label unreadable.
            const auto newline = d.name.find ('\n');
            if (newline != std::string::npos)
                d.name.erase (newline);

            // The PCM name IS the stable identifier on ALSA, and it encodes the
            // card and device, so it survives replug of the same port (§2.4).
            d.usbLocationId = name;
            d.isMicrophone = wantInput;
            d.hasPhysicalHeadphoneJack = ! wantInput;
            d.maxInputChannels = wantInput ? 2 : 0;
            d.supportedSampleRates = { 44100, 48000 };
            d.supportedBitDepths = { 16, 24, 32 };

            result.push_back (std::move (d));
        }

        free (name);
        free (desc);
        free (ioid);
    }

    snd_device_name_free_hint (hints);
    return result;
}

std::vector<AudioDeviceDescriptor> AlsaBackend::enumerateInputDevices()  { return enumerate (true); }
std::vector<AudioDeviceDescriptor> AlsaBackend::enumerateOutputDevices() { return enumerate (false); }

void AlsaBackend::setDeviceChangeCallback (DeviceChangeCallback callback)
{
    deviceChangeCallback = std::move (callback);
    hotplug.reset();

    if (! deviceChangeCallback)
        return;

    auto watcher = std::make_unique<AlsaHotplugWatcher>();
    watcher->fd = inotify_init1 (IN_CLOEXEC);

    if (watcher->fd < 0)
        return; // no inotify: enumeration still works, hotplug simply won't fire

    if (inotify_add_watch (watcher->fd, "/dev/snd", IN_CREATE | IN_DELETE) < 0)
    {
        ::close (watcher->fd);
        watcher->fd = -1;
        return;
    }

    watcher->running.store (true, std::memory_order_release);

    auto* raw = watcher.get();
    watcher->worker = std::thread ([this, raw]
    {
        // Sized for the documented worst case: one event plus a NAME_MAX name.
        alignas (struct inotify_event) char buffer[sizeof (struct inotify_event) + NAME_MAX + 1];

        while (raw->running.load (std::memory_order_acquire))
        {
            const auto bytes = ::read (raw->fd, buffer, sizeof (buffer));

            if (bytes <= 0)
                break; // descriptor closed on teardown, or an unrecoverable error

            if (raw->running.load (std::memory_order_acquire) && deviceChangeCallback)
                deviceChangeCallback();
        }
    });

    hotplug = std::move (watcher);
}

ExclusiveModeCapability AlsaBackend::checkExclusiveModeCapability (const std::string& outputDeviceId,
                                                                   double sampleRate, int bufferSizeSamples)
{
    ExclusiveModeCapability cap;

    if (! isExclusiveCapableName (outputDeviceId))
    {
        // §5.4: name the cause. "default" is the common case and the message
        // has to make sense to someone who has never heard of dmix.
        cap.unavailableReason =
            "This sound output is shared with other apps, which adds too much delay for live monitoring. "
            "Choose a specific sound card in Advanced.";
        return cap;
    }

    snd_pcm_t* pcm = nullptr;

    // Opening it is the only honest test: another client may already hold it.
    if (snd_pcm_open (&pcm, outputDeviceId.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
    {
        cap.unavailableReason =
            "Another app is using this sound output. Close it, or choose a different output in Advanced.";
        return cap;
    }

    snd_pcm_close (pcm);

    cap.exclusiveModeAvailable = true;
    cap.measuredOrEstimatedLatencyMs = (sampleRate > 0.0)
        ? (static_cast<double> (bufferSizeSamples) / sampleRate) * 1000.0 : 0.0;
    return cap;
}

bool AlsaBackend::openStream (const std::string& deviceId, double sampleRate, int bufferSizeSamples,
                              bool isInput, AudioCallback callback)
{
    auto stream = std::make_unique<AlsaStream>();
    stream->isInput = isInput;
    stream->callback = std::move (callback);

    const auto direction = isInput ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;

    if (snd_pcm_open (&stream->pcm, deviceId.c_str(), direction, 0) < 0)
        return false;

    // Float first because it needs no conversion; the integer formats are the
    // fallbacks real hardware actually offers.
    const snd_pcm_format_t candidates[] = { SND_PCM_FORMAT_FLOAT_LE,
                                            SND_PCM_FORMAT_S32_LE,
                                            SND_PCM_FORMAT_S16_LE };

    const unsigned int channels = isInput ? 1u : 2u; // §6.1 mono stems, stereo monitor
    const auto rate = static_cast<unsigned int> (sampleRate);
    const auto latencyMicroseconds = static_cast<unsigned int> (
        (static_cast<double> (bufferSizeSamples) / std::max (1.0, sampleRate)) * 1.0e6 * 2.0);

    bool configured = false;

    for (auto format : candidates)
    {
        if (snd_pcm_set_params (stream->pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED,
                                channels, rate, 1 /* allow resampling */,
                                latencyMicroseconds) == 0)
        {
            stream->format = format;
            stream->channels = channels;
            configured = true;
            break;
        }
    }

    if (! configured)
        return false;

    stream->periodFrames = static_cast<snd_pcm_uframes_t> (std::max (1, bufferSizeSamples));

    // §11: sized once, here, and never touched again from the audio thread.
    const size_t sampleCount = static_cast<size_t> (stream->periodFrames) * channels;
    stream->buffers.interleaved.assign (sampleCount * static_cast<size_t> (bytesPerSampleFor (stream->format)), 0);
    stream->buffers.interleavedFloat.assign (sampleCount, 0.0f);
    stream->buffers.planar.assign (sampleCount, 0.0f);
    stream->buffers.inputPointers.assign (channels, nullptr);
    stream->buffers.outputPointers.assign (channels, nullptr);

    for (unsigned int ch = 0; ch < channels; ++ch)
    {
        float* base = stream->buffers.planar.data() + static_cast<size_t> (ch) * stream->periodFrames;
        stream->buffers.inputPointers[ch] = base;
        stream->buffers.outputPointers[ch] = base;
    }

    stream->running.store (true, std::memory_order_release);

    auto* raw = stream.get();
    stream->worker = std::thread ([raw]
    {
        requestRealtimePriority();

        const auto frames = raw->periodFrames;
        const auto channelCount = raw->channels;

        while (raw->running.load (std::memory_order_acquire))
        {
            if (raw->isInput)
            {
                const auto got = snd_pcm_readi (raw->pcm, raw->buffers.interleaved.data(), frames);

                if (got < 0)
                {
                    // An xrun is recoverable and expected under load; anything
                    // else ends the stream rather than spinning on a dead PCM.
                    if (snd_pcm_recover (raw->pcm, static_cast<int> (got), 1) < 0)
                        break;

                    continue;
                }

                if (got == 0)
                    break; // end of a file-backed device: nothing more will arrive

                const auto count = static_cast<size_t> (got) * channelCount;

                // Two buffers, not one: deinterleaving through the same array it
                // reads from would alias and corrupt every channel after the
                // first. Mono skips the second pass entirely.
                if (channelCount == 1)
                {
                    toFloat (raw->buffers.interleaved.data(), raw->buffers.planar.data(), raw->format, count);
                }
                else
                {
                    toFloat (raw->buffers.interleaved.data(), raw->buffers.interleavedFloat.data(),
                             raw->format, count);

                    for (unsigned int ch = 0; ch < channelCount; ++ch)
                    {
                        float* dest = raw->buffers.planar.data() + static_cast<size_t> (ch) * frames;

                        for (snd_pcm_sframes_t f = 0; f < got; ++f)
                            dest[f] = raw->buffers.interleavedFloat[static_cast<size_t> (f) * channelCount + ch];
                    }
                }

                if (raw->callback)
                    raw->callback (raw->buffers.inputPointers.data(), static_cast<int> (channelCount),
                                   nullptr, 0, static_cast<int> (got));
            }
            else
            {
                for (auto& sample : raw->buffers.planar)
                    sample = 0.0f;

                if (raw->callback)
                    raw->callback (nullptr, 0, raw->buffers.outputPointers.data(),
                                   static_cast<int> (channelCount), static_cast<int> (frames));

                // Interleave from the planar layout the callback filled -- again
                // through a second buffer, for the same aliasing reason.
                const float* source = raw->buffers.planar.data();

                if (channelCount > 1)
                {
                    for (snd_pcm_uframes_t f = 0; f < frames; ++f)
                        for (unsigned int ch = 0; ch < channelCount; ++ch)
                            raw->buffers.interleavedFloat[static_cast<size_t> (f) * channelCount + ch] =
                                raw->buffers.outputPointers[ch][f];

                    source = raw->buffers.interleavedFloat.data();
                }

                fromFloat (source, raw->buffers.interleaved.data(),
                           raw->format, static_cast<size_t> (frames) * channelCount);

                const auto put = snd_pcm_writei (raw->pcm, raw->buffers.interleaved.data(), frames);

                if (put < 0 && snd_pcm_recover (raw->pcm, static_cast<int> (put), 1) < 0)
                    break;
            }
        }

        raw->running.store (false, std::memory_order_release);
    });

    openStreams.push_back (std::move (stream));
    return true;
}

bool AlsaBackend::openExclusiveOutputStream (const std::string& outputDeviceId, double sampleRate,
                                             int bufferSizeSamples, AudioCallback callback)
{
    return openStream (outputDeviceId, sampleRate, bufferSizeSamples, false, std::move (callback));
}

bool AlsaBackend::openInputStream (const std::string& inputDeviceId, double sampleRate,
                                   int bufferSizeSamples, AudioCallback callback)
{
    return openStream (inputDeviceId, sampleRate, bufferSizeSamples, true, std::move (callback));
}

void AlsaBackend::closeAllStreams()
{
    openStreams.clear(); // each AlsaStream stops and joins its worker in its destructor
}

} // namespace mma

#endif // __linux__ && ! MMA_NO_ALSA
