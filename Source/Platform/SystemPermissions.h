#pragma once

#include "../Core/PermissionGuidance.h"

#include <string>

namespace mma {

/// The OS-level privacy answers PermissionGuidance turns into sentences.
///
/// Split deliberately into a pure mapping and a platform probe. The mapping is
/// compiled and unit-tested on every platform; only the probe around it is
/// conditional, so the part that decides what a status MEANS is never the part
/// that goes untested because the build host is the wrong OS.
namespace permissions {

/// AVAuthorizationStatus, as macOS defines it:
///   0 notDetermined, 1 restricted, 2 denied, 3 authorized.
///
/// Restricted maps to Denied on purpose. The app cannot capture either way,
/// and the honest failure is "you are not allowed to use the microphones",
/// even though a user under MDM restriction cannot act on the advice. Saying
/// nothing would leave them staring at working hardware that records silence.
///
/// An unrecognised status maps to NotApplicable — silent. A future enumerator
/// we cannot interpret is not evidence of a denial, and guessing "denied"
/// would put a blocking error in front of a user whose microphone works.
PermissionState fromAVAuthorizationStatus (long status) noexcept;

/// Windows stores per-capability consent under the CapabilityAccessManager
/// consent store as the string "Allow" or "Deny".
PermissionState fromWindowsConsentValue (const std::string& value) noexcept;

/// The errno left by a failed attempt to write to the destination.
///
/// This is how a removable-volume denial actually presents: there is no query
/// API on macOS, so the only truthful answer comes from trying. EACCES and
/// EPERM mean we are not allowed; anything else (a missing directory, a full
/// card) is a different problem the app reports through its own path, so this
/// stays quiet rather than blaming privacy settings for a pulled card.
PermissionState fromWriteProbeErrno (int probeErrno) noexcept;

} // namespace permissions

/// Asks the OS whether this app may use the microphones.
///
/// Returns NotApplicable where the platform has no such concept (Linux), which
/// PermissionGuidance treats as silence rather than as permission.
PermissionState queryMicrophonePermission() noexcept;

/// Asks whether this app may write to `destinationPath`, by trying.
///
/// An empty path, or one that is not where the recording is going, returns
/// NotApplicable: this probe writes, so it must never run speculatively.
PermissionState queryVolumeWritePermission (const std::string& destinationPath) noexcept;

} // namespace mma
