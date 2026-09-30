# ctest -P script: static guard for the pinned JUCE camera patch.
#
# The macOS camera backend (juce_CameraDevice_mac.h, Objective-C++) cannot be
# compiled on non-Apple CI, so this only pins the text of the safety fix: the
# frame heartbeat must never ask AVFoundation to capture on an inactive
# connection, must catch an NSException from the capture call, and must keep
# rescheduling itself. Real behaviour can only be proven on a Mac.
if (NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "Pass -DPATCH_FILE=<path to juce-7.0.12-camera-lifecycle.patch>")
endif()

file(READ "${PATCH_FILE}" patch_text)

set(required_added_lines
    "+        if (videoConnection == nil || ! [videoConnection isActive] || ! [videoConnection isEnabled])"
    "+        @try"
    "+        @catch (NSException* exception)"
    "+    void scheduleImageCaptureRetry()"
    "+                        withOwner (self, [] (Pimpl& owner) { owner.scheduleImageCaptureRetry(); });"
    "+                            ownerLink->owner->scheduleImageCaptureRetry();")

set(failed FALSE)
foreach (line IN LISTS required_added_lines)
    string(FIND "${patch_text}" "${line}" position)
    if (position EQUAL -1)
        message(SEND_ERROR "JUCE camera patch is missing required line: ${line}")
        set(failed TRUE)
    endif()
endforeach()

if (failed)
    message(FATAL_ERROR "MAC-CAM-1 camera heartbeat guard is not in the JUCE patch")
endif()

message(STATUS "JUCE camera patch keeps the guarded, self-rescheduling heartbeat")
