# ctest -P script: generate SobStage's macOS Info.plist with the same juceaide
# JUCE uses at configure time on a Mac, and check the keys the release depends
# on. On Linux juce_add_gui_app never writes an Info.plist, so without this the
# plist content is only seen by Tools/verify_macos_release.sh on a macOS runner.
#
#   -DJUCEAIDE=<path to juceaide>  -DINFO_FILE=<SobStage JuceLibraryCode/Info.txt>
#   -DOUTPUT=<plist to write>      -DEXPECTED_MINIMUM_MACOS=<e.g. 13.0>
foreach (var JUCEAIDE INFO_FILE OUTPUT EXPECTED_MINIMUM_MACOS)
    if (NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "Pass -D${var}=...")
    endif()
endforeach()

execute_process(
    COMMAND "${JUCEAIDE}" plist App "${INFO_FILE}" "${OUTPUT}"
    RESULT_VARIABLE juceaide_result)
if (NOT juceaide_result EQUAL 0)
    message(FATAL_ERROR "juceaide plist failed (${juceaide_result})")
endif()

file(READ "${OUTPUT}" plist_text)
# juceaide pretty-prints; collapse whitespace between tags so the checks below
# do not depend on its indentation.
string(REGEX REPLACE ">[ \t\r\n]+<" "><" plist_text "${plist_text}")

set(failed FALSE)
function(require_key_value key value)
    string(FIND "${plist_text}" "<key>${key}</key><string>${value}</string>" position)
    if (position EQUAL -1)
        message(SEND_ERROR "Info.plist lacks ${key} = '${value}'")
        set(failed TRUE PARENT_SCOPE)
    endif()
endfunction()

function(require_non_empty_string key)
    if (NOT plist_text MATCHES "<key>${key}</key><string>[^<]+</string>")
        message(SEND_ERROR "Info.plist lacks a non-empty ${key}")
        set(failed TRUE PARENT_SCOPE)
    endif()
endfunction()

function(require_single key)
    string(REGEX MATCHALL "<key>${key}</key>" hits "${plist_text}")
    list(LENGTH hits count)
    if (NOT count EQUAL 1)
        message(SEND_ERROR "Info.plist declares ${key} ${count} times, expected once")
        set(failed TRUE PARENT_SCOPE)
    endif()
endfunction()

require_key_value(LSMinimumSystemVersion "${EXPECTED_MINIMUM_MACOS}")
require_single(LSMinimumSystemVersion)
# PLIST_TO_MERGE must merge with, not replace, the keys JUCE generates.
require_non_empty_string(NSMicrophoneUsageDescription)
require_non_empty_string(NSCameraUsageDescription)
# macOS turns gesture Reactions (balloons, confetti, fireworks) on for every
# app and draws them into the camera frames, so a performer's thumbs-up would
# be burned into the recorded take. Since macOS 14.4 this key sets the app's
# default to off (a user's own Video menu choice still wins).
string(FIND "${plist_text}" "<key>NSCameraReactionEffectGesturesEnabledDefault</key><false/>" position)
if (position EQUAL -1)
    message(SEND_ERROR "Info.plist must set NSCameraReactionEffectGesturesEnabledDefault to false")
    set(failed TRUE)
endif()
require_single(NSCameraReactionEffectGesturesEnabledDefault)
require_key_value(CFBundleIdentifier "com.taylordrew.sobstage")
require_key_value(CFBundlePackageType "APPL")
require_non_empty_string(CFBundleExecutable)

if (failed)
    message(FATAL_ERROR "Info.plist check failed: ${OUTPUT}")
endif()
message(STATUS "Info.plist OK: LSMinimumSystemVersion ${EXPECTED_MINIMUM_MACOS}, privacy strings present, gesture Reactions off")
