/* ALSA failures the fixture cannot produce, injected by replacing one symbol.
 *
 * The ALSA `file` plugin the Linux fixture is built on free-runs, loops its
 * infile, and never fails: it cannot xrun, and it cannot go away mid-stream.
 * That left two of the worker loop's three branches unreachable from any test,
 * and defeated two earlier attempts at mid-take device loss.
 *
 * libasound is a shared library, so one function can be replaced and everything
 * around it left alone. snd_pcm_open, the hw_params negotiation, the worker
 * thread and the whole capture path above it stay the shipping code.
 *
 *   MMA_SHIM_MODE          xrun (recoverable, forever) | dead (unrecoverable)
 *   MMA_SHIM_DEVICE        only this PCM fails; others read normally
 *   MMA_SHIM_FAIL_AFTER    let this many reads through first
 *   MMA_SHIM_FAIL_AFTER_MS let this many milliseconds of reading through first
 *   MMA_SHIM_STREAM        capture (default) | playback | both
 *   MMA_SHIM_OPEN_MATCH    snd_pcm_open of a name with this prefix is...
 *   MMA_SHIM_OPEN_AS       ...redirected to this name instead
 *   MMA_SHIM_REFUSE_RATE   hw_params_test_rate says no to this rate, always
 *
 * The last three exist for one reason. AlsaBackend's exclusive-mode capability
 * check refuses any output name that is not direct hardware, so nothing named
 * after the `file` plugin can reach the code past that gate -- and there is no
 * real card in CI. Redirecting an allowlisted hw: name onto the fixture lets
 * the REAL capability check run, against a device that then refuses the rate,
 * which is the configuration no fixture on this machine can otherwise produce.
 *
 * MMA_SHIM_STREAM reaches the monitor output. snd_pcm_writei and the whole
 * playback half of the worker loop had never been run by any test -- the Linux
 * harness passed no output device at all -- so headphones that stop accepting
 * audio mid-take was the one §0.1 door still open on the platform whose CI
 * opens a real device.
 *
 * MMA_SHIM_DEVICE is what makes a mid-take unplug expressible: one microphone
 * of several dies while the rest of the rig keeps working, which is the case
 * §0.1 is actually about.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace {

enum class Mode { xrun, dead };

/// Which direction the injected failure applies to. Capture by default, so
/// every existing caller of this shim keeps its exact behaviour.
enum class Stream { capture, playback, both };

/// Read once, on first use, and never written again. A function-local static is
/// initialised exactly once even when several threads arrive together, which
/// matters here: every open stream has its own ALSA worker thread, and the
/// mid-take case deliberately runs two of them at once. The earlier version of
/// this file set plain globals from whichever thread got here first, which is a
/// data race in the very tooling used to make claims about correctness.
struct Config
{
    Mode mode = Mode::xrun;
    Stream stream = Stream::capture;
    const char* onlyDevice = nullptr;
    long failAfterReads = 0;
    long failAfterMs = 0;
    const char* openMatch = nullptr;
    const char* openAs = nullptr;
    long refuseRate = 0;

    Config()
    {
        if (const char* m = std::getenv ("MMA_SHIM_MODE"); m != nullptr && std::strcmp (m, "dead") == 0)
            mode = Mode::dead;

        if (const char* s = std::getenv ("MMA_SHIM_STREAM"); s != nullptr)
        {
            if (std::strcmp (s, "playback") == 0)
                stream = Stream::playback;
            else if (std::strcmp (s, "both") == 0)
                stream = Stream::both;
        }

        onlyDevice = std::getenv ("MMA_SHIM_DEVICE");

        if (const char* n = std::getenv ("MMA_SHIM_FAIL_AFTER"); n != nullptr)
            failAfterReads = std::atol (n);

        if (const char* n = std::getenv ("MMA_SHIM_FAIL_AFTER_MS"); n != nullptr)
            failAfterMs = std::atol (n);

        openMatch = std::getenv ("MMA_SHIM_OPEN_MATCH");
        openAs = std::getenv ("MMA_SHIM_OPEN_AS");

        if (const char* n = std::getenv ("MMA_SHIM_REFUSE_RATE"); n != nullptr)
            refuseRate = std::atol (n);
    }
};

const Config& config()
{
    static const Config c;
    return c;
}

std::atomic<long> reads { 0 };

/// Steady-clock nanoseconds of the first read that could fail, or 0 for "not
/// yet". Set by whichever thread gets there first and left alone after that.
std::atomic<long long> firstReadNanos { 0 };

std::atomic<snd_pcm_sframes_t (*) (snd_pcm_t*, void*, snd_pcm_uframes_t)> realReadi { nullptr };

bool shouldFail (snd_pcm_t* pcm, Stream direction)
{
    const auto& c = config();

    // A capture-mode shim must leave playback completely alone, and the other
    // way round: the mid-take cases are about ONE half of the rig failing while
    // the rest keeps working, and a shim that failed both would prove less.
    if (c.stream != Stream::both && c.stream != direction)
        return false;

    if (c.onlyDevice != nullptr)
    {
        const char* name = snd_pcm_name (pcm);

        if (name == nullptr || std::strcmp (name, c.onlyDevice) != 0)
            return false;
    }

    if (reads.fetch_add (1, std::memory_order_relaxed) < c.failAfterReads)
        return false;

    if (c.failAfterMs > 0)
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        long long expected = 0;

        // Only the first thread here writes; every other one reads what it wrote.
        firstReadNanos.compare_exchange_strong (expected, static_cast<long long> (now),
                                                std::memory_order_relaxed);

        const auto began = firstReadNanos.load (std::memory_order_relaxed);
        const auto elapsedMs = (static_cast<long long> (now) - began) / 1000000;

        if (elapsedMs < c.failAfterMs)
            return false;
    }

    return true;
}

snd_pcm_sframes_t passThrough (snd_pcm_t* pcm, void* buffer, snd_pcm_uframes_t frames)
{
    auto fn = realReadi.load (std::memory_order_acquire);

    if (fn == nullptr)
    {
        fn = reinterpret_cast<decltype (fn)> (dlsym (RTLD_NEXT, "snd_pcm_readi"));
        realReadi.store (fn, std::memory_order_release);
    }

    return fn != nullptr ? fn (pcm, buffer, frames) : -EPIPE;
}

std::atomic<snd_pcm_sframes_t (*) (snd_pcm_t*, const void*, snd_pcm_uframes_t)> realWritei { nullptr };

snd_pcm_sframes_t passThroughWrite (snd_pcm_t* pcm, const void* buffer, snd_pcm_uframes_t frames)
{
    auto fn = realWritei.load (std::memory_order_acquire);

    if (fn == nullptr)
    {
        fn = reinterpret_cast<decltype (fn)> (dlsym (RTLD_NEXT, "snd_pcm_writei"));
        realWritei.store (fn, std::memory_order_release);
    }

    return fn != nullptr ? fn (pcm, buffer, frames) : -EPIPE;
}

} // namespace

extern "C" {

int snd_pcm_open (snd_pcm_t** pcm, const char* name, snd_pcm_stream_t stream, int mode)
{
    static std::atomic<int (*) (snd_pcm_t**, const char*, snd_pcm_stream_t, int)> real { nullptr };

    auto fn = real.load (std::memory_order_acquire);

    if (fn == nullptr)
    {
        fn = reinterpret_cast<decltype (fn)> (dlsym (RTLD_NEXT, "snd_pcm_open"));
        real.store (fn, std::memory_order_release);
    }

    if (fn == nullptr)
        return -ENODEV;

    const auto& c = config();

    if (c.openMatch != nullptr && c.openAs != nullptr && name != nullptr
        && std::strncmp (name, c.openMatch, std::strlen (c.openMatch)) == 0)
        return fn (pcm, c.openAs, stream, mode);

    return fn (pcm, name, stream, mode);
}

int snd_pcm_hw_params_test_rate (snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                 unsigned int rate, int dir)
{
    const auto& c = config();

    // Unconditional for the named rate. The point is a card that cannot run at
    // it at all, which is exactly what a 44100-only interface is.
    if (c.refuseRate > 0 && rate == static_cast<unsigned int> (c.refuseRate))
        return -EINVAL;

    static std::atomic<int (*) (snd_pcm_t*, snd_pcm_hw_params_t*, unsigned int, int)> real { nullptr };

    auto fn = real.load (std::memory_order_acquire);

    if (fn == nullptr)
    {
        fn = reinterpret_cast<decltype (fn)> (dlsym (RTLD_NEXT, "snd_pcm_hw_params_test_rate"));
        real.store (fn, std::memory_order_release);
    }

    return fn != nullptr ? fn (pcm, params, rate, dir) : -EINVAL;
}

snd_pcm_sframes_t snd_pcm_readi (snd_pcm_t* pcm, void* buffer, snd_pcm_uframes_t frames)
{
    if (! shouldFail (pcm, Stream::capture))
        return passThrough (pcm, buffer, frames);

    // -EPIPE is an xrun: recoverable, which is what makes the endless-recovery
    // case endless. -ENODEV is a device that is no longer there.
    return config().mode == Mode::dead ? -ENODEV : -EPIPE;
}

snd_pcm_sframes_t snd_pcm_writei (snd_pcm_t* pcm, const void* buffer, snd_pcm_uframes_t frames)
{
    if (! shouldFail (pcm, Stream::playback))
        return passThroughWrite (pcm, buffer, frames);

    return config().mode == Mode::dead ? -ENODEV : -EPIPE;
}

int snd_pcm_recover (snd_pcm_t* pcm, int err, int silent)
{
    const auto& c = config();

    // In xrun mode recovery always succeeds, which is the whole point: the PCM
    // is runnable again and the loop has no reason to stop. A device that is
    // gone cannot be recovered, and must not pretend otherwise.
    if (c.mode == Mode::dead && err == -ENODEV)
        return -ENODEV;

    if (c.mode == Mode::xrun)
        return 0;

    static std::atomic<int (*) (snd_pcm_t*, int, int)> realRecover { nullptr };

    auto fn = realRecover.load (std::memory_order_acquire);

    if (fn == nullptr)
    {
        fn = reinterpret_cast<decltype (fn)> (dlsym (RTLD_NEXT, "snd_pcm_recover"));
        realRecover.store (fn, std::memory_order_release);
    }

    return fn != nullptr ? fn (pcm, err, silent) : err;
}

} // extern "C"
