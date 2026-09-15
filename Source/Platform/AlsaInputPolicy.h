#pragma once

#include <filesystem>
#include <string>

namespace mma::alsa_detail {

// Linux exposes whether hardware can physically be removed by the user in the
// device hierarchy's `removable` attribute. The kernel's other values are
// `fixed` and `unknown`; both deliberately fail closed here.
bool deviceAncestryIsUserRemovable (const std::filesystem::path& devicePath);

// Resolves /sys/class/sound/cardN/device before applying the ancestry rule.
// A negative card number represents an ALSA plugin/virtual PCM, not a kernel
// sound card, and is always rejected by the shipping policy.
bool isDirectExternalHardwareCard (
    int cardNumber,
    const std::filesystem::path& soundClassRoot = "/sys/class/sound");

// Output enumeration intentionally retains ALSA's existing hint-based path.
// Inputs may use it only in a binary compiled with the test-only opt-in. The
// argument is always a compile-time constant at the production call site; it
// is exposed here solely so the small platform-neutral policy can be tested.
bool shouldUseHintEnumeration (bool wantInput, bool testInputsCompiledIn) noexcept;

// A run this long with no successful read in between means the PCM is refusing
// everything, not glitching under load.
inline constexpr int kRecoveriesBeforeGivingUp = 200;

// Whether an unbroken run of recovered read failures should be called a dead
// device rather than an xrun.
//
// snd_pcm_recover succeeding says the PCM was put back into a runnable state,
// not that the device is working: a PCM that fails and recovers on every read
// forever kept this loop spinning, counting a period of loss each time and
// never ending the stream. The user got a microphone written as silence for
// the rest of the take with only a rising dropped-frame number to show for it
// -- which is word for word the case the Windows worker's capture counter was
// added to stop, left open on Linux.
//
// An occasional xrun is load and must not end anything, so the rule is the
// same three-strikes shape the other two backends use: only an unbroken run,
// with no successful read to reset it, is a death.
bool alsaRecoveryRunMeansDeviceIsDead (int consecutiveRecoveries) noexcept;

// Whether an ALSA output name denotes a card this app can open exclusively, as
// §5.4 monitoring requires, or one shared through a mixing plugin.
//
// This used to accept only names beginning "hw:", and that rejected almost
// every output the app itself lists. The picker is filled from
// snd_device_name_hint, and no pcm definition alsa-lib ships emits a bare
// "hw:" hint -- what it emits for a card is "front:CARD=...", "hdmi:CARD=...",
// "iec958:CARD=...", "surround51:CARD=..." and so on. Every one of those
// resolves to `type hw`: a direct hardware PCM with no dmix in front of it.
// USB-Audio.conf, the definition covering the interfaces this app exists for,
// defines pcm.front.0 as `type hw` itself.
//
// So the user plugged in their interface, picked it in Advanced, and was told
// the output was "shared with other apps" and to choose a specific sound card
// -- which is exactly what they had just done, and there was no other entry
// that would have worked. Monitoring was off on Linux for the app's own
// hardware.
//
// It stays an allowlist, so an unrecognised name is still refused rather than
// opened hopefully; the list is simply the one alsa-lib actually produces.
// default, sysdefault, dmix, dsnoop, plug, pulse, pipewire and jack are all
// genuinely shared and are still refused.
bool alsaOutputNameIsExclusiveCapable (const std::string& outputName) noexcept;

} // namespace mma::alsa_detail
