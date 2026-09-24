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
 *
 * Real-time clocks (the microphone simulator):
 *
 *   MMA_SIM_REALTIME      1 = every PCM runs at its sample rate in wall-clock time
 *   MMA_SIM_PPM           per-device crystal error, e.g. "mma_mic1=+150,mma_mic2=-150"
 *   MMA_SIM_BUFFER_FRAMES the driver ring a real card would have; default: what
 *                         the PCM was configured with. A read that comes later
 *                         than that ring can hold gets -EPIPE, the way a real
 *                         capture xrun arrives, instead of a burst of the
 *                         missed blocks that no hardware could have kept.
 *
 * The fixture's `file` plugin sits on the `null` slave, which has no clock: a
 * read returns as fast as the file can be copied, so every fixture "microphone"
 * delivered audio many times faster than real time. Every take overflowed,
 * the dropped-sound card fired on every take, and drift -- the problem this app
 * exists to solve -- could not be exercised at all, because nothing ran on a
 * clock. With MMA_SIM_REALTIME each PCM is paced the way hardware paces it:
 * a read or write of N frames returns when N frames of that device's own
 * clock have elapsed, and a device given +150 ppm runs 150 ppm fast, like an
 * independent USB microphone's crystal.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

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

    /// False only for the microphone simulator on its own: MMA_SIM_REALTIME
    /// with none of the failure settings. Every older caller loads this shim to
    /// make reads fail and relies on that being the default, so the default
    /// stays exactly as it was for them.
    bool injectFailures = true;

    Config()
    {
        const bool anyFailureSetting = std::getenv ("MMA_SHIM_MODE") != nullptr
                                    || std::getenv ("MMA_SHIM_DEVICE") != nullptr
                                    || std::getenv ("MMA_SHIM_STREAM") != nullptr
                                    || std::getenv ("MMA_SHIM_FAIL_AFTER") != nullptr
                                    || std::getenv ("MMA_SHIM_FAIL_AFTER_MS") != nullptr;
        const char* realtime = std::getenv ("MMA_SIM_REALTIME");
        injectFailures = anyFailureSetting || realtime == nullptr || std::strcmp (realtime, "0") == 0;

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

/// Real-time pacing. Keyed by PCM handle; a handle is paced from its first
/// transfer, and re-anchored whenever it falls far behind (a stream that was
/// stopped, drained or re-prepared), so a restart is not "caught up" in one
/// burst that would look like the very overflow this exists to remove.
struct PacingState
{
    std::chrono::steady_clock::time_point anchor {};
    double framesSinceAnchor = 0.0;
    double framesPerSecond = 0.0;

    // Diagnostics, dumped to MMA_SIM_STATS_FILE at exit: the device's name,
    // the latest a read finished after the block's due time, how many blocks
    // finished a whole block late (the thread was descheduled, so the reads
    // that follow come as a burst, as they would from a kernel period buffer),
    // and how often the clock had to be re-anchored.
    std::string name;
    long long maxLateNs = 0;
    long long lateBlocks = 0;
    long long reanchors = 0;
    long long blocks = 0;
    long long bufferFrames = 0;
    long long xruns = 0;
};

std::mutex& pacingMutex() { static std::mutex m; return m; }
std::map<snd_pcm_t*, PacingState>& pacingStates() { static std::map<snd_pcm_t*, PacingState> s; return s; }

void dumpPacingStats()
{
    const char* path = std::getenv ("MMA_SIM_STATS_FILE");
    if (path == nullptr || *path == '\0')
        return;

    FILE* f = std::fopen (path, "w");
    if (f == nullptr)
        return;

    const std::lock_guard<std::mutex> lock (pacingMutex());

    for (const auto& [pcm, st] : pacingStates())
        std::fprintf (f, "%s blocks=%lld max_late_ms=%.2f late_blocks=%lld reanchors=%lld buffer_frames=%lld xruns=%lld\n",
                      st.name.c_str(), st.blocks, st.maxLateNs / 1.0e6, st.lateBlocks, st.reanchors,
                      st.bufferFrames, st.xruns);

    std::fclose (f);
}

bool realtimeEnabled()
{
    static const bool on = [] {
        const char* v = std::getenv ("MMA_SIM_REALTIME");
        return v != nullptr && std::strcmp (v, "0") != 0;
    }();
    return on;
}

double ppmFor (const char* name)
{
    const char* spec = std::getenv ("MMA_SIM_PPM");
    if (spec == nullptr || name == nullptr)
        return 0.0;

    const std::string all (spec), want (name);
    size_t start = 0;

    while (start < all.size())
    {
        auto end = all.find (',', start);
        if (end == std::string::npos)
            end = all.size();

        const auto item = all.substr (start, end - start);
        const auto eq = item.find ('=');

        if (eq != std::string::npos && item.substr (0, eq) == want)
            return std::atof (item.c_str() + eq + 1);

        start = end + 1;
    }

    return 0.0;
}

double nominalRate (snd_pcm_t* pcm)
{
    snd_pcm_hw_params_t* params = nullptr;
    unsigned int rate = 0;
    int dir = 0;

    if (snd_pcm_hw_params_malloc (&params) != 0)
        return 0.0;

    if (snd_pcm_hw_params_current (pcm, params) == 0)
        snd_pcm_hw_params_get_rate (params, &rate, &dir);

    snd_pcm_hw_params_free (params);
    return static_cast<double> (rate);
}

// Before a read: has the emulated driver ring already overflowed? A real
// card keeps capturing while the reader sleeps, into a ring of bufferFrames;
// a reader later than that finds the oldest audio gone and gets -EPIPE. The
// fixture's file plugin would instead hand over everything missed, however
// late, which no hardware does and which overflows the app's ring in a way
// hardware never would.
bool emulatedXrun (snd_pcm_t* pcm, snd_pcm_uframes_t frames)
{
    if (! realtimeEnabled())
        return false;

    const std::lock_guard<std::mutex> lock (pacingMutex());
    auto& st = pacingStates()[pcm];

    if (st.framesPerSecond <= 0.0)
        return false; // first read: the clock is anchored after it

    if (st.bufferFrames == 0)
    {
        if (const char* v = std::getenv ("MMA_SIM_BUFFER_FRAMES"); v != nullptr && std::atoll (v) > 0)
            st.bufferFrames = std::atoll (v);
        else
        {
            snd_pcm_uframes_t buffer = 0, period = 0;
            st.bufferFrames = snd_pcm_get_params (pcm, &buffer, &period) == 0 && buffer > 0
                                  ? static_cast<long long> (buffer)
                                  : static_cast<long long> (frames) * 2;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const auto due = st.anchor + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                         std::chrono::duration<double> ((st.framesSinceAnchor + static_cast<double> (frames)) / st.framesPerSecond));
    const auto ringHolds = std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                               std::chrono::duration<double> (static_cast<double> (st.bufferFrames) / st.framesPerSecond));

    if (now - due <= ringHolds)
        return false;

    // The ring overflowed. What it still holds is the newest bufferFrames;
    // the clock restarts from now, as a real card's does after prepare().
    ++st.xruns;
    st.anchor = now;
    st.framesSinceAnchor = 0.0;
    return true;
}

void pace (snd_pcm_t* pcm, snd_pcm_sframes_t transferred)
{
    if (! realtimeEnabled() || transferred <= 0)
        return;

    // The map and its mutex are constructed before the handler is registered,
    // so they outlive it: function statics die in reverse order of
    // construction, and an atexit handler registered after a static's
    // construction runs before that static's destructor.
    static const bool statsRegistered = (pacingMutex(), pacingStates(), std::atexit (dumpPacingStats), true);
    (void) statsRegistered;

    const auto now = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point due;

    {
        const std::lock_guard<std::mutex> lock (pacingMutex());
        auto& st = pacingStates()[pcm];

        if (st.framesPerSecond <= 0.0)
        {
            const auto rate = nominalRate (pcm);
            if (rate <= 0.0)
                return;

            const char* name = snd_pcm_name (pcm);
            st.name = name != nullptr ? name : "?";
            st.framesPerSecond = rate * (1.0 + ppmFor (name) * 1.0e-6);
            st.anchor = now;
            st.framesSinceAnchor = 0.0;
        }

        st.framesSinceAnchor += static_cast<double> (transferred);
        ++st.blocks;
        due = st.anchor + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                              std::chrono::duration<double> (st.framesSinceAnchor / st.framesPerSecond));

        if (now > due)
        {
            const auto lateNs = static_cast<long long> (
                std::chrono::duration_cast<std::chrono::nanoseconds> (now - due).count());
            st.maxLateNs = std::max (st.maxLateNs, lateNs);

            const auto blockNs = static_cast<long long> (1.0e9 * static_cast<double> (transferred) / st.framesPerSecond);
            if (lateNs >= blockNs)
                ++st.lateBlocks;
        }

        // More than a quarter of a second behind: the stream was idle, not
        // slow. Start its clock again from here.
        if (now - due > std::chrono::milliseconds (250))
        {
            ++st.reanchors;
            st.anchor = now;
            st.framesSinceAnchor = 0.0;
            return;
        }
    }

    std::this_thread::sleep_until (due);
}

std::atomic<long> reads { 0 };

/// Steady-clock nanoseconds of the first read that could fail, or 0 for "not
/// yet". Set by whichever thread gets there first and left alone after that.
std::atomic<long long> firstReadNanos { 0 };

std::atomic<snd_pcm_sframes_t (*) (snd_pcm_t*, void*, snd_pcm_uframes_t)> realReadi { nullptr };

bool shouldFail (snd_pcm_t* pcm, Stream direction)
{
    const auto& c = config();

    if (! c.injectFailures)
        return false;

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
    {
        if (emulatedXrun (pcm, frames))
            return -EPIPE;

        const auto got = passThrough (pcm, buffer, frames);
        pace (pcm, got);
        return got;
    }

    // -EPIPE is an xrun: recoverable, which is what makes the endless-recovery
    // case endless. -ENODEV is a device that is no longer there.
    return config().mode == Mode::dead ? -ENODEV : -EPIPE;
}

snd_pcm_sframes_t snd_pcm_writei (snd_pcm_t* pcm, const void* buffer, snd_pcm_uframes_t frames)
{
    if (! shouldFail (pcm, Stream::playback))
    {
        const auto put = passThroughWrite (pcm, buffer, frames);
        pace (pcm, put);
        return put;
    }

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
