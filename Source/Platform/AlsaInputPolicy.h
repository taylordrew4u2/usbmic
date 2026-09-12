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

} // namespace mma::alsa_detail
