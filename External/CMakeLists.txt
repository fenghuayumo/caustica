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

option(DONUT_WITH_NVRHI "Enable NVRHI and related projects" ON)
option(DONUT_WITH_AFTERMATH "Enable Aftermath crash dump generation with engine runtime code" OFF)
option(DONUT_WITH_STATIC_SHADERS "Build engine runtime code with statically linked shaders" OFF)
option(DONUT_WITH_AUDIO "Include Audio features (XAudio2)" OFF)
option(DONUT_WITH_TINYEXR "Include TinyEXR" ON)
option(DONUT_WITH_UNIT_TESTS "Donut unit-tests" OFF)

if (WIN32)
    option(DONUT_FORCE_DISCRETE_GPU "Declare symbols to make the OS run the app on discrete GPU on laptops" OFF)
endif()

set(third_party_folder "Third-Party Libraries")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if (DONUT_WITH_NVRHI AND NOT TARGET imgui)
    include(imgui.cmake)
    set_target_properties(imgui PROPERTIES FOLDER ${third_party_folder})
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

if (DONUT_WITH_TINYEXR AND NOT TARGET tinyexr)
    add_library(tinyexr INTERFACE)
    target_include_directories(tinyexr INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tinyexr)
endif()

if (DONUT_WITH_NVRHI AND NOT TARGET glfw)
    option(GLFW_BUILD_EXAMPLES "" OFF)
    option(GLFW_BUILD_TESTS "" OFF)
    option(GLFW_BUILD_DOCS "" OFF)
    option(GLFW_INSTALL "" OFF)
    add_subdirectory(glfw)
    set_target_properties(glfw PROPERTIES FOLDER ${third_party_folder})
    set_target_properties(update_mappings PROPERTIES FOLDER ${third_party_folder})
endif()

if (NOT TARGET ShaderMake)
    option(SHADERMAKE_FIND_FXC "" "${DONUT_WITH_DX11}")
    if (DONUT_WITH_DX12 OR DONUT_WITH_VULKAN)
        option(SHADERMAKE_FIND_DXC "" ON)
    else()
        option(SHADERMAKE_FIND_DXC "" OFF)
    endif()
    option(SHADERMAKE_FIND_DXC_VK "" "${DONUT_WITH_VULKAN}")

    add_subdirectory(ShaderMake)
endif()

if (DONUT_WITH_NVRHI)
    set(NVRHI_WITH_DX11 "${DONUT_WITH_DX11}" CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX12 "${DONUT_WITH_DX12}" CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VULKAN "${DONUT_WITH_VULKAN}" CACHE BOOL "" FORCE)
    set(NVRHI_WITH_AFTERMATH "${DONUT_WITH_AFTERMATH}" CACHE BOOL "" FORCE)

    add_subdirectory(nvrhi)

    set(DONUT_SHADER_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/caustica/engine" CACHE PATH "" FORCE)

    set(caustica_engine_shader_dir "${CMAKE_SOURCE_DIR}/caustica/engine/shaders")
    set(caustica_engine_shader_config "${caustica_engine_shader_dir}/EngineShaders.cfg")

    file(GLOB_RECURSE caustica_engine_shader_sources
        "${caustica_engine_shader_dir}/*.hlsl"
        "${caustica_engine_shader_dir}/*.hlsli"
        "${caustica_engine_shader_dir}/*.h"
    )
    list(FILTER caustica_engine_shader_sources EXCLUDE REGEX "/passes/tonemapping_ps\\.hlsl$")

    if(DONUT_SHADERS_OUTPUT_DIR AND NOT DONUT_WITH_STATIC_SHADERS)
        set(caustica_engine_shader_output_base ${DONUT_SHADERS_OUTPUT_DIR})
    else()
        set(CAUSTICA_ENGINE_STATIC_SHADERS_DIR "${CMAKE_BINARY_DIR}/caustica/engine/shaders" CACHE INTERNAL "")
        set(CAUSTICA_ENGINE_STATIC_SHADERS_DIR "${CAUSTICA_ENGINE_STATIC_SHADERS_DIR}" CACHE INTERNAL "")
        set(caustica_engine_shader_output_base ${CAUSTICA_ENGINE_STATIC_SHADERS_DIR})
    endif()

    if(DONUT_WITH_STATIC_SHADERS)
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
        passes/cubemap_gs
        passes/deferred_lighting_cs
        passes/depth_ps
        passes/depth_vs
        passes/dlss_exposure_cs
        passes/environment_map_ps
        passes/exposure_cs
        passes/forward_ps
        passes/forward_vs
        passes/gbuffer_ps
        passes/gbuffer_vs
        passes/histogram_cs
        passes/joints_main_ps
        passes/joints_main_vs
        passes/light_probe_cubemap_gs
        passes/light_probe_diffuse_probe_ps
        passes/light_probe_environment_brdf_ps
        passes/light_probe_mip_ps
        passes/light_probe_specular_probe_ps
        passes/material_id_ps
        passes/mipmapgen_cs
        passes/motion_vectors_ps
        passes/pixel_readback_cs
        passes/sky_ps
        passes/ssao_blur_cs
        passes/ssao_compute_cs
        passes/ssao_deinterleave_cs
        passes/taa_cs
    )

    if(DONUT_WITH_AFTERMATH OR DONUT_EMBED_SHADER_PDBS)
        set(caustica_engine_shader_options --embedPDB)
    endif()

    donut_compile_shaders_all_platforms(
        TARGET causticaEngineShaders
        PROJECT_NAME "CausticaEngine"
        CONFIG ${caustica_engine_shader_config}
        FOLDER "caustica/engine"
        OUTPUT_BASE ${caustica_engine_shader_output_base}
        OUTPUT_FORMAT ${caustica_engine_shader_output_format}
        SOURCES ${caustica_engine_shader_sources}
        BYPRODUCTS_NO_EXT ${caustica_engine_shader_byproducts}
        SHADERMAKE_OPTIONS ${caustica_engine_shader_options}
    )
endif()

if (DONUT_WITH_DLSS)
    message(STATUS "Updating DLSS from " ${DONUT_DLSS_FETCH_URL} ", tag " ${DONUT_DLSS_FETCH_TAG} ", into folder " ${DONUT_DLSS_FETCH_DIR} " - please wait!" )
    FetchContent_Declare(
        DLSS
        GIT_REPOSITORY ${DONUT_DLSS_FETCH_URL}
        GIT_TAG ${DONUT_DLSS_FETCH_TAG}
        SOURCE_DIR ${DONUT_DLSS_FETCH_DIR}
    )
    FetchContent_MakeAvailable(DLSS)

    include(cmake/DLSS.cmake)
endif()

if (DONUT_WITH_STREAMLINE)
    if (NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        message(SEND_ERROR "Streamline integration requires that CMAKE_RUNTIME_OUTPUT_DIRECTORY is non-empty.")
    endif()

    if (DONUT_STREAMLINE_FETCH_TAG)
        message(WARNING "DONUT_STREAMLINE_FETCH_TAG is deprecated. Use DONUT_STREAMLINE_FETCH_SHA256 instead. Tag fetching from the public repo no longer has the binaries.")
    endif()

    if ((DONUT_STREAMLINE_FETCH_URL AND NOT DONUT_STREAMLINE_FETCH_SHA256) OR
        (NOT DONUT_STREAMLINE_FETCH_URL AND DONUT_STREAMLINE_FETCH_SHA256))
        message(SEND_ERROR "Both DONUT_STREAMLINE_FETCH_URL and DONUT_STREAMLINE_FETCH_SHA256 must be defined together")
    endif()

    if (DONUT_STREAMLINE_FETCH_URL AND DONUT_STREAMLINE_FETCH_SHA256)
        message(STATUS "Updating Streamline from " ${DONUT_STREAMLINE_FETCH_URL} ", SHA256 " ${DONUT_STREAMLINE_FETCH_SHA256} ", into folder " ${DONUT_STREAMLINE_FETCH_DIR} " - please wait!" )
        FetchContent_Declare(
            streamline
            URL ${DONUT_STREAMLINE_FETCH_URL}
            URL_HASH SHA256=${DONUT_STREAMLINE_FETCH_SHA256}
            SOURCE_DIR ${DONUT_STREAMLINE_FETCH_DIR}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(streamline)
    else()
        if (DONUT_STREAMLINE_SEARCH_PATHS)
            set(STREAMLINE_SEARCH_PATHS ${DONUT_STREAMLINE_SEARCH_PATHS})
        endif()
        find_package(STREAMLINE REQUIRED)

        if (STREAMLINE_FOUND)
            add_subdirectory(${STREAMLINE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/streamline)
        endif()
    endif()
endif()

if (RTXPT_WITH_NATIVE_DLSS)
    set(rtxpt_native_dlss_runtime_libraries "")
    if (DLSS_SHARED_LIBRARY_PATH)
        list(APPEND rtxpt_native_dlss_runtime_libraries "${DLSS_SHARED_LIBRARY_PATH}")
    endif()
    if (DLSS_RR_SHARED_LIBRARY_PATH)
        list(APPEND rtxpt_native_dlss_runtime_libraries "${DLSS_RR_SHARED_LIBRARY_PATH}")
    endif()

    if (rtxpt_native_dlss_runtime_libraries)
        add_custom_target(RtxptNativeDLSSRuntime ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${rtxpt_native_dlss_runtime_libraries} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            COMMENT "Copying native DLSS runtime libraries"
        )
        set_target_properties(RtxptNativeDLSSRuntime PROPERTIES FOLDER "External/engine")
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

##########################################################################
########################### Mathlib ###########################
##########################################################################
# Note: Fetching mathlib manually to bypass NRD trying to fetch an invalid
# tag. This is fixed in latest NRD, so we can remove the workaround after
# we update NRD again.

if(NOT TARGET MathLib)
    # MathLib
    FetchContent_Declare(
        mathlib
        GIT_REPOSITORY https://github.com/NVIDIA-RTX/MathLib.git
        GIT_TAG v7
        GIT_SHALLOW 1
    )
    message("Downloading Mathlib")

    FetchContent_MakeAvailable(mathlib)
endif()

##########################################################################
########################### Nrd ###########################
##########################################################################

# see https://github.com/NVIDIA-RTX/NRD?tab=readme-ov-file#how-to-build
# set(NRD_DXC_CUSTOM_PATH ${DXC_CUSTOM_PATH})
set(NRD_DXC_PATH "${DXC_DXIL_EXECUTABLE}" CACHE STRING "DXC shader compiler path for NRD")
set(NRD_SHADER_OUTPUT_PATH "${SHADER_OUTPUT_PATH}" CACHE STRING "")
set(NRD_PROJECT_FOLDER "External/Nrd")
set(NRD_NORMAL_ENCODING "2" CACHE STRING "Normal encoding variant (0-4, matches nrd::NormalEncoding)")
set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "Roughness encoding variant (0-2, matches nrd::RoughnessEncoding)")
set(NRD_IS_SUBMODULE ON)
# NRD uses a custom output path for some reason
set(GLOBAL_BIN_OUTPUT_PATH ${CMAKE_BINARY_DIR} CACHE STRING "")
set(NRD_SHADERS_PATH "${CMAKE_BINARY_DIR}/NRDShaders" CACHE STRING "")
option(NRD_EMBEDS_SPIRV_SHADERS "NRD embeds SPIRV shaders" OFF)
option(NRD_EMBEDS_DXIL_SHADERS "NRD embeds DXIL shaders" OFF)
option(NRD_EMBEDS_DXBC_SHADERS "NRD embeds DXBC shaders" OFF)
option(NRD_DISABLE_SHADER_COMPILATION "" ON)
option(NRD_USE_PRECOMPILED_SHADERS "" OFF)

add_subdirectory("Nrd")

# Let CMake generate the nrd_shaders.cfg file 
util_generate_shader_config_file(
    "Nrd.cfg" 
    "Nrd/Shaders/Source"
    "-D NRD_COMPILER_DXC=1 -D NRD_NORMAL_ENCODING=${NRD_NORMAL_ENCODING} -D NRD_ROUGHNESS_ENCODING=${NRD_ROUGHNESS_ENCODING}"
)

# Compile all shaders in nrd_shaders.cfg
set(nrd_shader_compile_args
    TARGET nrd_shaders
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/Nrd.cfg
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/Nrd/Shaders/Include
    FOLDER "NRD"
    SPIRV_DXC ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/nrd/spirv
    CFLAGS "-WX -O3 -enable-16bit-types -all_resources_bound"
    INCLUDES ${CMAKE_BINARY_DIR}/_deps/mathlib-src ${CMAKE_CURRENT_SOURCE_DIR}/Nrd/Shaders/Include ${CMAKE_CURRENT_SOURCE_DIR}/Nrd/Shaders/Resources
)
if (WIN32)
    list(APPEND nrd_shader_compile_args
        DXIL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/nrd/dxil
    )
endif()
donut_compile_shaders(${nrd_shader_compile_args})

##########################################################################
########################### Opacity Micro-Map SDK ########################
##########################################################################

option(OMM_INTEGRATION_LAYER_NVRHI "" ON)
option(OMM_ENABLE_PRECOMPILED_SHADERS_DXIL "" OFF)
option(OMM_ENABLE_PRECOMPILED_SHADERS_SPIRV "" OFF)
option(OMM_ENABLE_BENCHMARK "" OFF)
option(OMM_ENABLE_TESTS "" OFF)
option(OMM_BUILD_VIEWER "" OFF)

set(OMM_PROJECT_FOLDER "Omm")
add_subdirectory("Omm")

# Let CMake generate the omm_shaders.cfg file 
util_generate_shader_config_file(
    "Omm.cfg" 
    "Omm/libraries/omm-lib/shaders"
    "-D COMPILER_DXC=1"
)

# Compile all shaders in omm.cfg
set(omm_shader_compile_args
    TARGET omm_shaders
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/Omm.cfg
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/Omm/libraries/omm-lib/include
    FOLDER "Opacity Micro-Map SDK"
    SPIRV_DXC ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/omm/spirv
)
if (WIN32)
    list(APPEND omm_shader_compile_args
        DXIL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/omm/dxil
    )
endif()
donut_compile_shaders(${omm_shader_compile_args})

##########################################################################
################################ RTXDI SDK ###############################
##########################################################################

set(RTXDI_SKIP_SHADER_VALIDATION ON CACHE STRING "Skip RTXDI shader validation")

add_subdirectory("Rtxdi")

##########################################################################
################################ cxxopts ###############################
##########################################################################

if (NOT TARGET cxxopts)
    add_subdirectory("cxxopts")
endif()
