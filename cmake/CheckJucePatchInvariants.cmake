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
    "+                            ownerLink->owner->scheduleImageCaptureRetry();"
    # MAC-CAM-2: macOS 27 aborted on launch when the preview layer's
    # configuration commit raced the queued startRunning. The layer is
    # attached before the start, the start is caught and retried, and no
    # reconfiguration runs while a start is queued.
    "+                        [retainedSession startRunning];"
    "+                            weakRef->sessionStartFinished (started, raised);"
    "+        if (sessionStartQueued)"
    "+        return view;")

set(failed FALSE)
foreach (line IN LISTS required_added_lines)
    string(FIND "${patch_text}" "${line}" position)
    if (position EQUAL -1)
        message(SEND_ERROR "JUCE camera patch is missing required line: ${line}")
        set(failed TRUE)
    endif()
endforeach()

if (failed)
    message(FATAL_ERROR "MAC-CAM-1/2 camera guards are not in the JUCE patch")
endif()

# The preview layer must be attached before the session start is queued.
string(FIND "${patch_text}" "[view setLayer: [AVCaptureVideoPreviewLayer layerWithSession: getCaptureSession()]];" layer_at REVERSE)
string(FIND "${patch_text}" "+        startSession();\r\n+        return view;" start_at)
if (start_at EQUAL -1)
    string(FIND "${patch_text}" "+        startSession();\n+        return view;" start_at)
endif()
if (layer_at EQUAL -1 OR start_at EQUAL -1 OR start_at LESS layer_at)
    message(FATAL_ERROR "MAC-CAM-2 preview layer is not attached before the capture session starts")
endif()

message(STATUS "JUCE camera patch keeps the guarded, self-rescheduling heartbeat")
