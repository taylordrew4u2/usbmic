#!/usr/bin/env bash
# Verify the user-visible contract of a packaged macOS app. Run this after each
# archive round-trip so packaging cannot silently change executable permissions
# or ship a bundle built for the runner instead of the supported Macs.
set -euo pipefail

if [[ $# -lt 2 || $# -gt 4 ]]; then
  echo "Usage: $0 /path/to/SobStage.app EXPECTED_VERSION [EXPECTED_MINIMUM_MACOS] [--capture-entitlements]" >&2
  exit 2
fi

APP_PATH="$1"
EXPECTED_VERSION="$2"
EXPECTED_MINIMUM_MACOS="${3:-13.0}"
SIGNING_MODE="${4:-}"
EXPECTED_BUNDLE_IDENTIFIER="com.taylordrew.sobstage"
EXPECTED_COPYRIGHT="Copyright (c) 2026 Taylor Drew Kozero"
EXPECTED_MIC_PERMISSION="SobStage records from external USB, FireWire, and Thunderbolt microphones and audio interfaces you connect."

fail() {
  echo "::error::$*" >&2
  exit 1
}

[[ -d "$APP_PATH" ]] || fail "macOS app bundle not found: $APP_PATH"

PLIST="$APP_PATH/Contents/Info.plist"
[[ -f "$PLIST" ]] || fail "Info.plist missing from $APP_PATH"

plist_value() {
  /usr/libexec/PlistBuddy -c "Print :$1" "$PLIST" 2>/dev/null
}

BUNDLE_EXECUTABLE="$(plist_value CFBundleExecutable)" \
  || fail "CFBundleExecutable missing from Info.plist"
EXECUTABLE="$APP_PATH/Contents/MacOS/$BUNDLE_EXECUTABLE"
[[ -f "$EXECUTABLE" ]] || fail "bundle executable missing: $EXECUTABLE"
[[ -x "$EXECUTABLE" ]] || fail "bundle executable is not executable: $EXECUTABLE"

SHORT_VERSION="$(plist_value CFBundleShortVersionString)" \
  || fail "CFBundleShortVersionString missing from Info.plist"
BUNDLE_VERSION="$(plist_value CFBundleVersion)" \
  || fail "CFBundleVersion missing from Info.plist"
[[ "$SHORT_VERSION" == "$EXPECTED_VERSION" ]] \
  || fail "CFBundleShortVersionString is $SHORT_VERSION, expected $EXPECTED_VERSION"
[[ "$BUNDLE_VERSION" == "$EXPECTED_VERSION" ]] \
  || fail "CFBundleVersion is $BUNDLE_VERSION, expected $EXPECTED_VERSION"

BUNDLE_IDENTIFIER="$(plist_value CFBundleIdentifier)" \
  || fail "CFBundleIdentifier missing from Info.plist"
[[ "$BUNDLE_IDENTIFIER" == "$EXPECTED_BUNDLE_IDENTIFIER" ]] \
  || fail "CFBundleIdentifier is $BUNDLE_IDENTIFIER, expected $EXPECTED_BUNDLE_IDENTIFIER"

COPYRIGHT="$(plist_value NSHumanReadableCopyright)" \
  || fail "NSHumanReadableCopyright missing from Info.plist"
[[ "$COPYRIGHT" == "$EXPECTED_COPYRIGHT" ]] \
  || fail "NSHumanReadableCopyright is $COPYRIGHT, expected $EXPECTED_COPYRIGHT"

PACKAGE_TYPE="$(plist_value CFBundlePackageType)" \
  || fail "CFBundlePackageType missing from Info.plist"
[[ "$PACKAGE_TYPE" == "APPL" ]] \
  || fail "CFBundlePackageType is $PACKAGE_TYPE, expected APPL"

MIC_PERMISSION="$(plist_value NSMicrophoneUsageDescription)" \
  || fail "NSMicrophoneUsageDescription missing from Info.plist"
[[ "$MIC_PERMISSION" == "$EXPECTED_MIC_PERMISSION" ]] \
  || fail "unexpected microphone permission text: $MIC_PERMISSION"

CAMERA_PERMISSION="$(plist_value NSCameraUsageDescription)" \
  || fail "NSCameraUsageDescription missing from Info.plist"
[[ -n "$CAMERA_PERMISSION" ]] \
  || fail "NSCameraUsageDescription is empty"

ICON_FILE="$(plist_value CFBundleIconFile)" \
  || fail "CFBundleIconFile missing from Info.plist"
[[ -f "$APP_PATH/Contents/Resources/$ICON_FILE" ]] \
  || fail "declared app icon is missing: $ICON_FILE"

ARCHITECTURES="$(lipo -archs "$EXECUTABLE")" \
  || fail "could not inspect executable architectures"
NORMALIZED_ARCHITECTURES="$({
  for architecture in $ARCHITECTURES; do
    printf '%s\n' "$architecture"
  done
} | sort | paste -sd ' ' -)"
[[ "$NORMALIZED_ARCHITECTURES" == "arm64 x86_64" ]] \
  || fail "architectures are '$ARCHITECTURES', expected universal arm64 and x86_64"

MINIMUM_VERSIONS="$(otool -l "$EXECUTABLE" | awk '$1 == "minos" { print $2 }')"
[[ -n "$MINIMUM_VERSIONS" ]] \
  || fail "no LC_BUILD_VERSION minimum macOS value found"

MINIMUM_VERSION_COUNT=0
while IFS= read -r minimum_version; do
  [[ -n "$minimum_version" ]] || continue
  MINIMUM_VERSION_COUNT=$((MINIMUM_VERSION_COUNT + 1))
  [[ "$minimum_version" == "$EXPECTED_MINIMUM_MACOS" ]] \
    || fail "minimum macOS is $minimum_version, expected $EXPECTED_MINIMUM_MACOS"
done <<< "$MINIMUM_VERSIONS"
[[ "$MINIMUM_VERSION_COUNT" -eq 2 ]] \
  || fail "found $MINIMUM_VERSION_COUNT minimum-macOS records, expected one per architecture"

codesign --verify --deep --strict --verbose=2 "$APP_PATH"

if [[ "$SIGNING_MODE" == "--capture-entitlements" ]]; then
  ENTITLEMENTS="$(mktemp)"
  cleanup_entitlements() { rm -f "$ENTITLEMENTS"; }
  trap cleanup_entitlements EXIT

  codesign -d --entitlements :- "$APP_PATH" > "$ENTITLEMENTS"
  [[ "$(/usr/libexec/PlistBuddy -c 'Print :com.apple.security.device.audio-input' "$ENTITLEMENTS" 2>/dev/null)" == "true" ]] \
    || fail "production signature is missing the audio-input entitlement"
  [[ "$(/usr/libexec/PlistBuddy -c 'Print :com.apple.security.device.camera' "$ENTITLEMENTS" 2>/dev/null)" == "true" ]] \
    || fail "production signature is missing the camera entitlement"

  cleanup_entitlements
  trap - EXIT
elif [[ -n "$SIGNING_MODE" ]]; then
  fail "unknown signing-verification mode: $SIGNING_MODE"
fi

echo "Verified $APP_PATH"
echo "  version: $EXPECTED_VERSION"
echo "  identifier: $BUNDLE_IDENTIFIER"
echo "  architectures: $ARCHITECTURES"
echo "  minimum macOS: $EXPECTED_MINIMUM_MACOS"
echo "  executable permission and privacy strings: present"
if [[ "$SIGNING_MODE" == "--capture-entitlements" ]]; then
  echo "  hardened-runtime capture entitlements: present"
fi
