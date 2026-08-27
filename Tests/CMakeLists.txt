add_executable(mma_core_tests
    main.cpp
    test_DriftCompensator.cpp
    test_ChannelLayoutAnalyzer.cpp
    test_SampleRateNegotiator.cpp
    test_PolarPatternDetector.cpp
    test_DeadChannelDetector.cpp
    test_Metering.cpp
    test_MonitorBus.cpp
    test_SessionFolderNaming.cpp
    test_PreflightThroughputTest.cpp
    test_SessionMetadata.cpp
    test_RingBuffer.cpp
    test_DeviceManager.cpp
    test_RecordingEngine.cpp
    test_SessionWriter.cpp
    test_OutputDeviceSelector.cpp
    test_CapacityMonitor.cpp
)

target_link_libraries(mma_core_tests PRIVATE mma_core)
target_include_directories(mma_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME mma_core_tests COMMAND mma_core_tests)
