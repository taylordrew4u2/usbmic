/* One ALSA function, replaced: snd_pcm_readi, made to fail the way a sick PCM
 * fails -- every read, forever, with snd_pcm_recover putting the PCM back into
 * a runnable state every time.
 *
 * This is the one thing the Linux fixture cannot express on its own. The ALSA
 * `file` plugin it is built on free-runs, loops its infile, and never xruns, so
 * the worker loop's recovery path was unreachable from any test. Interposing a
 * single symbol reaches it while leaving every other call -- snd_pcm_open, the
 * hw_params negotiation, the worker thread itself -- as the shipping code.
 *
 * MMA_SHIM_FAIL_AFTER lets the first N reads succeed, so the counter's reset on
 * a successful read can be exercised rather than assumed.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <cstdlib>

// extern "C" so the symbols keep the names the dynamic loader has to match:
// a mangled snd_pcm_readi interposes nothing at all, silently.
extern "C" {

static long failAfter = -1;
static long reads = 0;

static snd_pcm_sframes_t (*realReadi) (snd_pcm_t*, void*, snd_pcm_uframes_t) = nullptr;

snd_pcm_sframes_t snd_pcm_readi (snd_pcm_t* pcm, void* buffer, snd_pcm_uframes_t frames)
{
    if (failAfter < 0)
    {
        const char* n = std::getenv ("MMA_SHIM_FAIL_AFTER");
        failAfter = n != nullptr ? std::atol (n) : 0;
    }

    if (reads++ < failAfter)
    {
        if (realReadi == nullptr)
            realReadi = reinterpret_cast<decltype (realReadi)> (dlsym (RTLD_NEXT, "snd_pcm_readi"));

        if (realReadi != nullptr)
            return realReadi (pcm, buffer, frames);
    }

    /* -EPIPE is an xrun: recoverable, which is the whole point. A device that
     * returned something unrecoverable was already handled.  */
    return -EPIPE;
}

int snd_pcm_recover (snd_pcm_t* pcm, int err, int silent)
{
    (void) pcm; (void) err; (void) silent;
    return 0; /* always succeeds, which is what makes the loop endless */
}

} // extern "C"
