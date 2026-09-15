#pragma once

#include <filesystem>

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

} // namespace mma::alsa_detail
