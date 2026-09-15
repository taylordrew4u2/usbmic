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
 *
 * MMA_SHIM_DEVICE is what makes a mid-take unplug expressible: one microphone
 * of several dies while the rest of the rig keeps working, which is the case
 * §0.1 is actually about.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace {

enum class Mode { xrun, dead };

Mode mode = Mode::xrun;
const char* onlyDevice = nullptr;
long failAfterReads = 0;
long failAfterMs = 0;
long reads = 0;
bool configured = false;
std::chrono::steady_clock::time_point firstRead {};

snd_pcm_sframes_t (*realReadi) (snd_pcm_t*, void*, snd_pcm_uframes_t) = nullptr;

void configure()
{
    if (configured)
        return;

    configured = true;

    if (const char* m = std::getenv ("MMA_SHIM_MODE"); m != nullptr && std::strcmp (m, "dead") == 0)
        mode = Mode::dead;

    onlyDevice = std::getenv ("MMA_SHIM_DEVICE");

    if (const char* n = std::getenv ("MMA_SHIM_FAIL_AFTER"); n != nullptr)
        failAfterReads = std::atol (n);

    if (const char* n = std::getenv ("MMA_SHIM_FAIL_AFTER_MS"); n != nullptr)
        failAfterMs = std::atol (n);
}

bool shouldFail (snd_pcm_t* pcm)
{
    configure();

    if (onlyDevice != nullptr)
    {
        const char* name = snd_pcm_name (pcm);

        if (name == nullptr || std::strcmp (name, onlyDevice) != 0)
            return false;
    }

    if (reads++ < failAfterReads)
        return false;

    if (failAfterMs > 0)
    {
        const auto now = std::chrono::steady_clock::now();

        if (firstRead.time_since_epoch().count() == 0)
            firstRead = now;

        if (std::chrono::duration_cast<std::chrono::milliseconds> (now - firstRead).count() < failAfterMs)
            return false;
    }

    return true;
}

snd_pcm_sframes_t passThrough (snd_pcm_t* pcm, void* buffer, snd_pcm_uframes_t frames)
{
    if (realReadi == nullptr)
        realReadi = reinterpret_cast<decltype (realReadi)> (dlsym (RTLD_NEXT, "snd_pcm_readi"));

    return realReadi != nullptr ? realReadi (pcm, buffer, frames) : -EPIPE;
}

} // namespace

extern "C" {

snd_pcm_sframes_t snd_pcm_readi (snd_pcm_t* pcm, void* buffer, snd_pcm_uframes_t frames)
{
    if (! shouldFail (pcm))
        return passThrough (pcm, buffer, frames);

    // -EPIPE is an xrun: recoverable, which is what makes the endless-recovery
    // case endless. -ENODEV is a device that is no longer there.
    return mode == Mode::dead ? -ENODEV : -EPIPE;
}

int snd_pcm_recover (snd_pcm_t* pcm, int err, int silent)
{
    configure();

    // In xrun mode recovery always succeeds, which is the whole point: the PCM
    // is runnable again and the loop has no reason to stop. A device that is
    // gone cannot be recovered, and must not pretend otherwise.
    if (mode == Mode::dead && err == -ENODEV)
        return -ENODEV;

    if (mode == Mode::xrun)
        return 0;

    static int (*realRecover) (snd_pcm_t*, int, int) = nullptr;

    if (realRecover == nullptr)
        realRecover = reinterpret_cast<decltype (realRecover)> (dlsym (RTLD_NEXT, "snd_pcm_recover"));

    return realRecover != nullptr ? realRecover (pcm, err, silent) : err;
}

} // extern "C"
