#
# ImGuizmo - immediate mode 3D gizmo for scene editing (translate / rotate / scale)
#

set(_imguizmo_dir ${CMAKE_CURRENT_SOURCE_DIR}/ImGuizmo)
set(_imguizmo_patch ${CMAKE_CURRENT_SOURCE_DIR}/patches/ImGuizmo-editor-viewport.patch)

# Upstream submodule cannot carry caustica DockSpace/viewport fixes; apply locally.
if(EXISTS ${_imguizmo_patch})
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${_imguizmo_patch}
            WORKING_DIRECTORY ${_imguizmo_dir}
            RESULT_VARIABLE _imguizmo_already_patched
            OUTPUT_QUIET
            ERROR_QUIET)
        if(NOT _imguizmo_already_patched EQUAL 0)
            execute_process(
                COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${_imguizmo_patch}
                WORKING_DIRECTORY ${_imguizmo_dir}
                RESULT_VARIABLE _imguizmo_apply
                ERROR_VARIABLE _imguizmo_apply_err)
            if(NOT _imguizmo_apply EQUAL 0)
                message(FATAL_ERROR
                    "Failed to apply ${_imguizmo_patch}:\n${_imguizmo_apply_err}")
            endif()
            message(STATUS "Applied ImGuizmo editor viewport patch")
        endif()
    else()
        message(WARNING "Git not found; skipped applying ImGuizmo editor viewport patch")
    endif()
endif()

set(imguizmo_srcs
    ${_imguizmo_dir}/src/ImGuizmo.cpp
    ${_imguizmo_dir}/src/ImGuizmo.h)

add_library(imguizmo STATIC ${imguizmo_srcs})
target_include_directories(imguizmo PUBLIC ${_imguizmo_dir}/src)
target_link_libraries(imguizmo PUBLIC imgui)
