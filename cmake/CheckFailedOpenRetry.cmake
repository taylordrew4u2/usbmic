# ctest -P script: a rig where no microphone would open must be retried from
# the status poll. Fixing the cause -- the interface's rate in Audio MIDI
# Setup, quitting the app that held it -- fires no device-list notification,
# and a device that never opened has no stream to listen on, so without this
# Record stayed disabled until a replug or a relaunch. The decision itself is
# FailedOpenRetry (unit-tested); this pins that the poll actually runs it,
# ahead of any early return, and acts on it.
#
#   -DAPPLICATION_CPP=<Source/App/Application.cpp>
if (NOT DEFINED APPLICATION_CPP OR "${APPLICATION_CPP}" STREQUAL "")
    message(FATAL_ERROR "Pass -DAPPLICATION_CPP=...")
endif()

file(READ "${APPLICATION_CPP}" text)

string(FIND "${text}" "juce::String Application::pollStatusAdvice (double sinceLastCallSeconds)" start)
if (start EQUAL -1)
    message(FATAL_ERROR "pollStatusAdvice() not found")
endif()
string(SUBSTRING "${text}" ${start} -1 body)
string(FIND "${body}" "\n}\n" end)
string(SUBSTRING "${body}" 0 ${end} body)

string(FIND "${body}" "failedOpenRetry.tick (sinceLastCallSeconds, situation)" tick)
if (tick EQUAL -1)
    message(FATAL_ERROR "pollStatusAdvice() does not tick failedOpenRetry")
endif()

string(SUBSTRING "${body}" 0 ${tick} beforeTick)
string(FIND "${beforeTick}" "return " earlyReturn)
if (NOT earlyReturn EQUAL -1)
    message(FATAL_ERROR "failedOpenRetry must be ticked before any return in pollStatusAdvice()")
endif()

string(SUBSTRING "${body}" ${tick} 200 afterTick)
string(FIND "${afterTick}" "onDeviceListChanged();" reopen)
if (reopen EQUAL -1)
    message(FATAL_ERROR "a failedOpenRetry tick must reopen through onDeviceListChanged()")
endif()

foreach (field "situation.idle =" "situation.includedMicCount =" "situation.monitoring ="
               "situation.permissionDenied =")
    string(FIND "${body}" "${field}" at)
    if (at EQUAL -1)
        message(FATAL_ERROR "pollStatusAdvice() no longer fills '${field}'")
    endif()
endforeach()

message(STATUS "A rig with no microphone open is retried from the status poll")
