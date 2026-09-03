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
    test_CameraSelection.cpp
    test_AppSettings.cpp
    test_SessionRecovery.cpp
    test_CardRemovalNotice.cpp
    test_PreflightThroughputTest.cpp
    test_SessionMetadata.cpp
    test_RingBuffer.cpp
    test_DeviceManager.cpp
    test_ClockMasterResolver.cpp
    test_TakeCompleteness.cpp
    test_RecordingEngine.cpp
    test_SessionWriter.cpp
    test_OutputDeviceSelector.cpp
    test_CapacityMonitor.cpp
    test_BufferLadder.cpp
    test_CpuPressureMonitor.cpp
    test_MirrorPolicy.cpp
    test_SetupAdvisor.cpp
    test_SampleFormat.cpp
    test_WritePipeline.cpp
    test_CombinedTake.cpp
    test_TapToNameDetector.cpp
    test_PermissionGuidance.cpp
    test_CaptureCoordinator.cpp
    test_DeviceInputStream.cpp
)

target_link_libraries(mma_core_tests PRIVATE mma_core)
target_include_directories(mma_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME mma_core_tests COMMAND mma_core_tests)
