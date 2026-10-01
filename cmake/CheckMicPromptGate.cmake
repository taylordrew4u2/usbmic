# ctest -P script: the macOS "microphone prompt still unanswered" Record gate
# lives in a JUCE_MAC block of Application.cpp, which the Linux build never
# compiles. Pin its shape here so a refactor cannot drop it silently.
#
#   -DAPPLICATION_CPP=<Source/App/Application.cpp>
if (NOT DEFINED APPLICATION_CPP OR "${APPLICATION_CPP}" STREQUAL "")
    message(FATAL_ERROR "Pass -DAPPLICATION_CPP=...")
endif()

file(READ "${APPLICATION_CPP}" text)

# The body of getRecordDisabledReason(), up to the next member definition.
string(FIND "${text}" "juce::String Application::getRecordDisabledReason() const" start)
if (start EQUAL -1)
    message(FATAL_ERROR "getRecordDisabledReason() not found")
endif()
string(SUBSTRING "${text}" ${start} -1 gate)
string(FIND "${gate}" "\n}\n" end)
string(SUBSTRING "${gate}" 0 ${end} gate)

set(failed FALSE)

# After the streams are known open (the prompt is only on screen then), and
# before anything that lets the take start.
string(FIND "${gate}" "! capture->isMonitoring()" monitoring)
string(FIND "${gate}" "    if (const auto promptReason = PermissionGuidance::pendingPromptReason (\n            microphonePermission, true, capture->getPeakArrived() > kRealSoundThreshold)" prompt)
string(FIND "${gate}" "recoveryBlockingReason()" recovery)
if (monitoring EQUAL -1 OR prompt EQUAL -1 OR recovery EQUAL -1)
    message(SEND_ERROR "getRecordDisabledReason() lacks the JUCE_MAC pendingPromptReason gate")
    set(failed TRUE)
elseif (NOT (monitoring LESS prompt AND prompt LESS recovery))
    message(SEND_ERROR "pendingPromptReason gate must sit after isMonitoring() and before recovery")
    set(failed TRUE)
endif()

# A grant that lands mid-take must not be journalled as an all-clear.
string(FIND "${text}" "PermissionGuidance::grantArrivedMessage (takeRunning)" grantMessage)
if (grantMessage EQUAL -1)
    message(SEND_ERROR "refreshMicrophonePermission() does not use grantArrivedMessage()")
    set(failed TRUE)
endif()

# Record re-samples the permission instead of trusting a sample up to 2 s old.
string(FIND "${text}" "void Application::toggleRecording()" toggle)
string(SUBSTRING "${text}" ${toggle} 1500 toggleHead)
string(FIND "${toggleHead}" "refreshMicrophonePermission()" refreshAt)
string(FIND "${toggleHead}" "getRecordDisabledReason()" gateAt)
if (refreshAt EQUAL -1 OR gateAt EQUAL -1 OR NOT refreshAt LESS gateAt)
    message(SEND_ERROR "toggleRecording() must refresh the microphone permission before its gate")
    set(failed TRUE)
endif()

if (failed)
    message(FATAL_ERROR "Microphone prompt gate check failed")
endif()
message(STATUS "Microphone prompt gate present")
