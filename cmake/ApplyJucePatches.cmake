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

    # An existing build tree still holds JUCE patched with an EARLIER revision
    # of this patch (e.g. after `git pull`), so neither check matches. When the
    # tree is FetchContent's own git clone of the pinned commit, restore just
    # the files this patch owns and apply the current revision. Anything else
    # (a release bundle's ThirdParty/JUCE, a different commit, edits elsewhere)
    # still stops below rather than guessing.
    set(pinned_juce_commit "4f43011b96eb0636104cb3e433894cda98243626")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --show-toplevel HEAD
        WORKING_DIRECTORY "${juce_source_dir}"
        RESULT_VARIABLE rev_parse_result
        OUTPUT_VARIABLE rev_parse_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --numstat "${patch_file}"
        WORKING_DIRECTORY "${juce_source_dir}"
        RESULT_VARIABLE numstat_result
        OUTPUT_VARIABLE numstat_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" diff --name-only HEAD
        WORKING_DIRECTORY "${juce_source_dir}"
        RESULT_VARIABLE dirty_result
        OUTPUT_VARIABLE dirty_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    set(can_restore FALSE)
    if (rev_parse_result EQUAL 0 AND numstat_result EQUAL 0 AND dirty_result EQUAL 0)
        string(REPLACE "\n" ";" rev_parse_lines "${rev_parse_output}")
        list(LENGTH rev_parse_lines rev_parse_count)
        if (rev_parse_count EQUAL 2)
            list(GET rev_parse_lines 0 juce_git_toplevel)
            list(GET rev_parse_lines 1 juce_git_head)
            get_filename_component(juce_git_toplevel "${juce_git_toplevel}" REALPATH)
            get_filename_component(juce_source_real "${juce_source_dir}" REALPATH)

            set(patched_files "")
            string(REPLACE "\n" ";" numstat_lines "${numstat_output}")
            foreach (numstat_line IN LISTS numstat_lines)
                if (numstat_line MATCHES "^[0-9-]+\t[0-9-]+\t(.+)$")
                    list(APPEND patched_files "${CMAKE_MATCH_1}")
                endif()
            endforeach()

            set(only_patched_files_dirty TRUE)
            string(REPLACE "\n" ";" dirty_files "${dirty_output}")
            foreach (dirty_file IN LISTS dirty_files)
                list(FIND patched_files "${dirty_file}" dirty_file_index)
                if (dirty_file_index EQUAL -1)
                    set(only_patched_files_dirty FALSE)
                endif()
            endforeach()

            if (juce_git_toplevel STREQUAL juce_source_real
                AND juce_git_head STREQUAL pinned_juce_commit
                AND patched_files
                AND only_patched_files_dirty)
                set(can_restore TRUE)
            endif()
        endif()
    endif()

    if (can_restore)
        message(STATUS "Replacing an earlier revision of SobStage's JUCE camera patch")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" checkout HEAD -- ${patched_files}
            WORKING_DIRECTORY "${juce_source_dir}"
            RESULT_VARIABLE restore_result
            ERROR_VARIABLE restore_error)
        if (NOT restore_result EQUAL 0)
            message(FATAL_ERROR "Could not restore JUCE's camera sources:\n${restore_error}")
        endif()

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

    message(FATAL_ERROR
        "The pinned JUCE camera patch matches neither the original nor patched source. "
        "Refusing to build an unknown camera backend.\n"
        "Apply check: ${patch_check_error}\n"
        "Reverse check: ${reverse_check_error}")
endfunction()
