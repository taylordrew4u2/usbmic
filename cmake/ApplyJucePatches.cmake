function(sobstage_apply_juce_camera_patch juce_source_dir)
    set(patch_file
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../ThirdParty/Patches/juce-7.0.12-camera-lifecycle.patch")

    if (NOT EXISTS "${patch_file}")
        message(FATAL_ERROR "The pinned JUCE camera lifecycle patch is missing: ${patch_file}")
    endif()

    find_package(Git REQUIRED)

    # JUCE 7.0.12 stores these headers with CRLF line endings. The maintained
    # patch is normal repository text, so ignore context-only line-ending
    # differences while still requiring every changed source line to match.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check --ignore-whitespace --whitespace=nowarn "${patch_file}"
        WORKING_DIRECTORY "${juce_source_dir}"
        RESULT_VARIABLE patch_can_apply
        ERROR_VARIABLE patch_check_error)

    if (patch_can_apply EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --whitespace=nowarn "${patch_file}"
            WORKING_DIRECTORY "${juce_source_dir}"
            RESULT_VARIABLE patch_result
            ERROR_VARIABLE patch_error)

        if (NOT patch_result EQUAL 0)
            message(FATAL_ERROR "Could not apply the pinned JUCE camera patch:\n${patch_error}")
        endif()

        message(STATUS "Applied SobStage's JUCE 7.0.12 camera lifecycle patch")
        return()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-whitespace --whitespace=nowarn "${patch_file}"
        WORKING_DIRECTORY "${juce_source_dir}"
        RESULT_VARIABLE patch_is_applied
        ERROR_VARIABLE reverse_check_error)

    if (patch_is_applied EQUAL 0)
        message(STATUS "SobStage's JUCE 7.0.12 camera lifecycle patch is already applied")
        return()
    endif()

    message(FATAL_ERROR
        "The pinned JUCE camera patch matches neither the original nor patched source. "
        "Refusing to build an unknown camera backend.\n"
        "Apply check: ${patch_check_error}\n"
        "Reverse check: ${reverse_check_error}")
endfunction()
