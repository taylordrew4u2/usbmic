cmake_minimum_required(VERSION 3.22)
project(MultiMicAggregator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MMA_BUILD_APP "Build the JUCE GUI application (requires network fetch of JUCE)" OFF)
option(MMA_BUILD_TESTS "Build the headless Core/ unit test target" ON)

# ---------------------------------------------------------------------------
# Core: platform-independent, no-JUCE-dependency logic library.
# Always built -- this is what compiles and is testable on any machine,
# including a headless Linux CI sandbox with no audio hardware and no JUCE.
# ---------------------------------------------------------------------------
add_library(mma_core STATIC
    Source/Core/DriftCompensator.cpp
    Source/Core/ChannelLayoutAnalyzer.cpp
    Source/Core/SampleRateNegotiator.cpp
    Source/Core/PolarPatternDetector.cpp
    Source/Core/DeadChannelDetector.cpp
    Source/Core/BusPowerDetector.cpp
    Source/Core/ControllerContentionDetector.cpp
    Source/Core/Metering.cpp
    Source/Core/MonitorBus.cpp
    Source/Core/MixBusLimiter.cpp
    Source/Core/RingBuffer.cpp
    Source/Core/SessionWriter.cpp
    Source/Core/SessionMetadata.cpp
    Source/Core/SessionFolderNaming.cpp
    Source/Core/PreflightThroughputTest.cpp
    Source/Core/PortIdentity.cpp
    Source/Core/DeviceManager.cpp
    Source/Core/RecordingEngine.cpp
    Source/Core/OutputDeviceSelector.cpp
    Source/Core/CapacityMonitor.cpp
)
target_include_directories(mma_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty
)

if (MMA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

# ---------------------------------------------------------------------------
# The full JUCE GUI application. Off by default because it requires fetching
# JUCE over the network (FetchContent) which is not available in every build
# environment (e.g. this sandbox). Enable with -DMMA_BUILD_APP=ON on a
# machine with network access and a real JUCE-supported target OS.
# ---------------------------------------------------------------------------
if (MMA_BUILD_APP)
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        7.0.12
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(JUCE)

    juce_add_gui_app(MultiMicAggregator
        PRODUCT_NAME "Multi-Mic Aggregator"
        COMPANY_NAME "MultiMicAggregator"
        MICROPHONE_PERMISSION_ENABLED TRUE
    )

    target_sources(MultiMicAggregator PRIVATE
        Source/Platform/CoreAudioBackend.cpp
        Source/Platform/WasapiAsioBackend.cpp
        Source/Platform/NullBackend.cpp
        Source/Platform/AsioOutputBackend.cpp
        Source/Platform/LicensedVirtualCableBackend.cpp
        Source/Platform/AttestationDriverBackend.cpp
        Source/UI/SkullMeterComponent.cpp
        Source/UI/MixBarComponent.cpp
        Source/UI/MainScreen.cpp
        Source/UI/AdvancedPanel.cpp
        Source/UI/MainComponent.cpp
        Source/UI/Main.cpp
        Source/App/Application.cpp
    )

    target_compile_definitions(MultiMicAggregator PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MultiMicAggregator,JUCE_PRODUCT_NAME>"
    )

    target_link_libraries(MultiMicAggregator PRIVATE
        mma_core
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_extra
    )

    if (WIN32)
        # AvSetMmThreadCharacteristics: Pro Audio scheduling for the WASAPI
        # worker. Without it the OS will not wake a small exclusive-mode buffer
        # reliably enough to hold the §5.4 latency budget.
        target_link_libraries(MultiMicAggregator PRIVATE avrt)
    endif()
endif()
