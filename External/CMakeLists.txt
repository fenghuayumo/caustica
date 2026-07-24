#
# Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
# 
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto. Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

##########################################################################
########################### Engine dependencies ########################
##########################################################################

include(FetchContent)
include(cmake/compileshaders.cmake)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(CAUSTICA_WITH_STATIC_SHADERS "Build engine runtime code with statically linked shaders" OFF)
option(CAUSTICA_WITH_AUDIO "Include Audio features (XAudio2)" OFF)
option(CAUSTICA_WITH_TINYEXR "Include TinyEXR" ON)
option(CAUSTICA_WITH_UNIT_TESTS "Caustica unit-tests" OFF)

if (WIN32)
    option(CAUSTICA_FORCE_DISCRETE_GPU "Declare symbols to make the OS run the app on discrete GPU on laptops" OFF)
endif()

set(third_party_folder "Third-Party Libraries")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if (CAUSTICA_WITH_RHI AND NOT TARGET imgui)
    include(imgui.cmake)
    set_target_properties(imgui PROPERTIES FOLDER ${third_party_folder})
endif()

if (CAUSTICA_WITH_RHI AND NOT TARGET imguizmo)
    include(imguizmo.cmake)
    set_target_properties(imguizmo PROPERTIES FOLDER ${third_party_folder})
endif()

if (NOT TARGET jsoncpp_static)
    add_library(jsoncpp_static)
    set(jsoncpp_path "${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp-amalgam")
    target_include_directories(jsoncpp_static PUBLIC "${jsoncpp_path}")
    target_sources(jsoncpp_static PRIVATE "${jsoncpp_path}/jsoncpp.cpp")
    set_target_properties(jsoncpp_static PROPERTIES FOLDER ${third_party_folder})
endif()

if (NOT TARGET stb)
    add_library(stb INTERFACE)
    target_include_directories(stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)
endif()

if (NOT TARGET cgltf)
    add_library(cgltf INTERFACE)
    target_include_directories(cgltf INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cgltf)
endif()

if (NOT TARGET EnTT::EnTT)
    option(ENTT_INCLUDE_HEADERS "Add all EnTT headers to the EnTT target." OFF)
    option(ENTT_INCLUDE_NATVIS "Add EnTT natvis files to the EnTT target." OFF)
    add_subdirectory(entt)
    set_target_properties(EnTT PROPERTIES FOLDER ${third_party_folder})
endif()

if (CAUSTICA_WITH_TINYEXR AND NOT TARGET tinyexr)
    add_library(tinyexr INTERFACE)
    target_include_directories(tinyexr INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tinyexr)
endif()

if (CAUSTICA_WITH_RHI AND NOT TARGET glfw)
    option(GLFW_BUILD_EXAMPLES "" OFF)
    option(GLFW_BUILD_TESTS "" OFF)
    option(GLFW_BUILD_DOCS "" OFF)
    option(GLFW_INSTALL "" OFF)
    add_subdirectory(glfw)
    set_target_properties(glfw PROPERTIES FOLDER ${third_party_folder})
    set_target_properties(update_mappings PROPERTIES FOLDER ${third_party_folder})
endif()

if (NOT TARGET ShaderMake)
    option(SHADERMAKE_FIND_FXC "" "${CAUSTICA_WITH_DX11}")
    if (CAUSTICA_WITH_DX12 OR CAUSTICA_WITH_VULKAN)
        option(SHADERMAKE_FIND_DXC "" ON)
    else()
        option(SHADERMAKE_FIND_DXC "" OFF)
    endif()
    option(SHADERMAKE_FIND_DXC_VK "" "${CAUSTICA_WITH_VULKAN}")

    add_subdirectory(ShaderMake)
endif()

if (CAUSTICA_WITH_RHI)
    # Caustica RHI is now built from
    # caustica/backend, which is added before External in the root CMakeLists.
    set(CAUSTICA_SHADER_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/caustica/caustica" CACHE PATH "" FORCE)

    set(caustica_engine_shader_dir "${CMAKE_SOURCE_DIR}/caustica/caustica/shaders")
    set(caustica_engine_shader_config "${caustica_engine_shader_dir}/EngineShaders.cfg")

    file(GLOB_RECURSE caustica_engine_shader_sources
        "${caustica_engine_shader_dir}/*.hlsl"
        "${caustica_engine_shader_dir}/*.hlsli"
        "${caustica_engine_shader_dir}/*.h"
    )
    list(FILTER caustica_engine_shader_sources EXCLUDE REGEX "/passes/tonemapping_ps\\.hlsl$")

    if(CAUSTICA_SHADERS_OUTPUT_DIR AND NOT CAUSTICA_WITH_STATIC_SHADERS)
        set(caustica_engine_shader_output_base ${CAUSTICA_SHADERS_OUTPUT_DIR})
    else()
        set(CAUSTICA_ENGINE_STATIC_SHADERS_DIR "${CMAKE_BINARY_DIR}/caustica/caustica/shaders" CACHE INTERNAL "")
        set(CAUSTICA_ENGINE_STATIC_SHADERS_DIR "${CAUSTICA_ENGINE_STATIC_SHADERS_DIR}" CACHE INTERNAL "")
        set(caustica_engine_shader_output_base ${CAUSTICA_ENGINE_STATIC_SHADERS_DIR})
    endif()

    if(CAUSTICA_WITH_STATIC_SHADERS)
        set(caustica_engine_shader_output_format "HEADER")
    else()
        set(caustica_engine_shader_output_format "BINARY")
    endif()

    set(caustica_engine_shader_byproducts
        blit_ps
        fullscreen_vs
        ies_profile_cs
        imgui_pixel
        imgui_vertex
        rect_vs
        sharpen_ps
        skinning_cs
        passes/bloom_ps
        passes/dlss_exposure_cs
        passes/exposure_cs
        passes/histogram_cs
        passes/mipmapgen_cs
        passes/motion_vectors_ps
        passes/pixel_readback_cs
        passes/taa_cs
    )

    if(CAUSTICA_WITH_AFTERMATH OR CAUSTICA_EMBED_SHADER_PDBS)
        set(caustica_engine_shader_options --embedPDB)
    endif()

    caustica_compile_shaders_all_platforms(
        TARGET causticaEngineShaders
        PROJECT_NAME "CausticaEngine"
        CONFIG ${caustica_engine_shader_config}
        FOLDER "caustica/caustica"
        OUTPUT_BASE ${caustica_engine_shader_output_base}
        OUTPUT_FORMAT ${caustica_engine_shader_output_format}
        SOURCES ${caustica_engine_shader_sources}
        BYPRODUCTS_NO_EXT ${caustica_engine_shader_byproducts}
        SHADERMAKE_OPTIONS ${caustica_engine_shader_options}
    )
endif()

if (CAUSTICA_WITH_DLSS)
    message(STATUS "Updating DLSS from " ${CAUSTICA_DLSS_FETCH_URL} ", tag " ${CAUSTICA_DLSS_FETCH_TAG} ", into folder " ${CAUSTICA_DLSS_FETCH_DIR} " - please wait!" )
    FetchContent_Declare(
        DLSS
        GIT_REPOSITORY ${CAUSTICA_DLSS_FETCH_URL}
        GIT_TAG ${CAUSTICA_DLSS_FETCH_TAG}
        SOURCE_DIR ${CAUSTICA_DLSS_FETCH_DIR}
    )
    FetchContent_MakeAvailable(DLSS)

    include(cmake/DLSS.cmake)
endif()

if (CAUSTICA_WITH_STREAMLINE)
    if (NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        message(SEND_ERROR "Streamline integration requires that CMAKE_RUNTIME_OUTPUT_DIRECTORY is non-empty.")
    endif()

    if (CAUSTICA_STREAMLINE_FETCH_TAG)
        message(WARNING "CAUSTICA_STREAMLINE_FETCH_TAG is deprecated. Use CAUSTICA_STREAMLINE_FETCH_SHA256 instead. Tag fetching from the public repo no longer has the binaries.")
    endif()

    if ((CAUSTICA_STREAMLINE_FETCH_URL AND NOT CAUSTICA_STREAMLINE_FETCH_SHA256) OR
        (NOT CAUSTICA_STREAMLINE_FETCH_URL AND CAUSTICA_STREAMLINE_FETCH_SHA256))
        message(SEND_ERROR "Both CAUSTICA_STREAMLINE_FETCH_URL and CAUSTICA_STREAMLINE_FETCH_SHA256 must be defined together")
    endif()

    if (CAUSTICA_STREAMLINE_FETCH_DIR
        AND EXISTS "${CAUSTICA_STREAMLINE_FETCH_DIR}/CMakeLists.txt"
        AND EXISTS "${CAUSTICA_STREAMLINE_FETCH_DIR}/include/sl.h")
        message(STATUS "Using local Streamline SDK from ${CAUSTICA_STREAMLINE_FETCH_DIR}")
        add_subdirectory("${CAUSTICA_STREAMLINE_FETCH_DIR}" "${CMAKE_BINARY_DIR}/streamline")
    elseif (CAUSTICA_STREAMLINE_FETCH_URL AND CAUSTICA_STREAMLINE_FETCH_SHA256)
        message(STATUS "Updating Streamline from " ${CAUSTICA_STREAMLINE_FETCH_URL} ", SHA256 " ${CAUSTICA_STREAMLINE_FETCH_SHA256} ", into folder " ${CAUSTICA_STREAMLINE_FETCH_DIR} " - please wait!" )
        FetchContent_Declare(
            streamline
            URL ${CAUSTICA_STREAMLINE_FETCH_URL}
            URL_HASH SHA256=${CAUSTICA_STREAMLINE_FETCH_SHA256}
            SOURCE_DIR ${CAUSTICA_STREAMLINE_FETCH_DIR}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(streamline)
    else()
        if (CAUSTICA_STREAMLINE_SEARCH_PATHS)
            set(STREAMLINE_SEARCH_PATHS ${CAUSTICA_STREAMLINE_SEARCH_PATHS})
        endif()
        find_package(STREAMLINE REQUIRED)

        if (STREAMLINE_FOUND)
            add_subdirectory(${STREAMLINE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/streamline)
        endif()
    endif()
endif()

if (CAUSTICA_WITH_NATIVE_DLSS)
    set(caustica_native_dlss_runtime_libraries "")
    if (DLSS_SHARED_LIBRARY_PATH)
        list(APPEND caustica_native_dlss_runtime_libraries "${DLSS_SHARED_LIBRARY_PATH}")
    endif()
    if (DLSS_RR_SHARED_LIBRARY_PATH)
        list(APPEND caustica_native_dlss_runtime_libraries "${DLSS_RR_SHARED_LIBRARY_PATH}")
    endif()

    if (caustica_native_dlss_runtime_libraries)
        add_custom_target(CausticaNativeDLSSRuntime ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${caustica_native_dlss_runtime_libraries} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            COMMENT "Copying native DLSS runtime libraries"
        )
        set_target_properties(CausticaNativeDLSSRuntime PROPERTIES FOLDER "External/engine")
    endif()
endif()

##########################################################################
################################ UTIL ####################################
##########################################################################
function(util_get_shader_profile_from_name FILE_NAME DXC_PROFILE)
    get_filename_component(EXTENSION ${FILE_NAME} EXT)
    if ("${EXTENSION}" STREQUAL ".cs.hlsl")
        set(DXC_PROFILE "cs" PARENT_SCOPE)
    endif()
    if ("${EXTENSION}" STREQUAL ".vs.hlsl")
        set(DXC_PROFILE "vs" PARENT_SCOPE)
    endif()
    if ("${EXTENSION}" STREQUAL ".gs.hlsl")
        set(DXC_PROFILE "gs" PARENT_SCOPE)
    endif()
    if ("${EXTENSION}" STREQUAL ".ps.hlsl")
        set(DXC_PROFILE "ps" PARENT_SCOPE)
    endif()
endfunction()

function(util_generate_shader_config_file OUT_FILE_NAME DIR DEFINES)
    file(GLOB_RECURSE HLSL_FILES "${CMAKE_CURRENT_SOURCE_DIR}/${DIR}/*.hlsl")

    set(out_content "")
    foreach(FILE_NAME ${HLSL_FILES})
        get_filename_component(NAME_ONLY ${FILE_NAME} NAME)
        set(DXC_PROFILE "")
        util_get_shader_profile_from_name(${FILE_NAME} DXC_PROFILE)
        set(out_content "${out_content}${DIR}/${NAME_ONLY} -T ${DXC_PROFILE} -E main ${DEFINES}\n")
    endforeach()

    file(WRITE ${OUT_FILE_NAME} ${out_content})
endfunction()

# NRD lives in caustica/caustica/nrd (first-party). MathLib + nrd_shaders are
# wired from caustica/CMakeLists.txt / caustica/caustica/CMakeLists.txt.

##########################################################################
################################ cxxopts ###############################
##########################################################################

if (NOT TARGET cxxopts)
    add_subdirectory("cxxopts")
endif()
