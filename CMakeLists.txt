#
# Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto. Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

cmake_minimum_required(VERSION 3.18)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0175 OLD) # streamline error

project(caustica)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

option(CAUSTICA_DISTRIBUTION_BUILD "Build release artifacts intended for redistribution, without debug shader/program symbols." OFF)
if (CAUSTICA_DISTRIBUTION_BUILD)
    add_compile_definitions(CAUSTICA_DISTRIBUTION_BUILD=1)
else()
    add_compile_definitions(CAUSTICA_DISTRIBUTION_BUILD=0)
endif()

if (MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

	# Workaround for a Microsoft "fix" to std::mutex that happened in VS2022 v17.10.
	# Using std::mutex with a DLL version of MSVCRT that is older than that VS version will lead to a crash.
	# See https://developercommunity.visualstudio.com/t/Access-violation-with-std::mutex::lock-a/10664660
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")

	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")

    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /D_ITERATOR_DEBUG_LEVEL=1")

    if (NOT CAUSTICA_DISTRIBUTION_BUILD)
        # Only affect Release configuration
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi" CACHE STRING "" FORCE)
        set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Zi" CACHE STRING "" FORCE)
        set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG" CACHE STRING "" FORCE)
        set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG" CACHE STRING "" FORCE)
    endif()
endif()

# we can't run with DX11
option(CAUSTICA_WITH_DX11 "Enable the DX11 version of the framework runtime code" OFF)

if (WIN32)
    set(CAUSTICA_DEFAULT_WITH_DX12 ON)
else()
    set(CAUSTICA_DEFAULT_WITH_DX12 OFF)
endif()
option(CAUSTICA_WITH_DX12 "Enable the DX12 version of the framework runtime code" ${CAUSTICA_DEFAULT_WITH_DX12})

# Vulkan is required for Linux/WSL builds because the D3D backends are Windows-only.
if (WIN32)
    set(CAUSTICA_DEFAULT_WITH_VULKAN OFF)
else()
    set(CAUSTICA_DEFAULT_WITH_VULKAN ON)
endif()
option(CAUSTICA_WITH_VULKAN "Enable the Vulkan version of the framework runtime code" ${CAUSTICA_DEFAULT_WITH_VULKAN})

option(CAUSTICA_WITH_NVRHI "Enable NVRHI and related projects" ON)
option(CAUSTICA_WITH_AFTERMATH "Enable Aftermath crash dump generation with engine runtime code" OFF)

# we want shader PDBs always embedded
option(CAUSTICA_EMBED_SHADER_PDBS "" ON)
if (CAUSTICA_DISTRIBUTION_BUILD)
    set(CAUSTICA_EMBED_SHADER_PDBS OFF CACHE BOOL "" FORCE)
endif()

set (CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)

set(CAUSTICA_LOCAL_CONFIG_ID "NONAME" CACHE STRING "Local user initials for user-specific settings")
add_definitions( -DCAUSTICA_LOCAL_CONFIG_ID_STRING=std::string\(\"${CAUSTICA_LOCAL_CONFIG_ID}\"\) )

#----------------------------------------------------------------------------------------------------------------------
# Helper to download and unzip a package from a URL
# Uses a zero-length file to identify the version of the package
function(CheckAndDownloadPackage NAME VERSION LOCAL_PATH URL ARCHIVE_TYPE)
    # Do we already have the correct version?
    if(NOT EXISTS ${LOCAL_PATH}/${VERSION}.ver)
        # Was there a previous version that we need to delete?
        if(EXISTS ${LOCAL_PATH})
            message(STATUS "Deleting old " ${NAME})
            file(REMOVE_RECURSE ${LOCAL_PATH})
        endif()
        message(STATUS "Obtaining " ${NAME} " " ${VERSION} " from " ${URL})
        file(DOWNLOAD ${URL} ${LOCAL_PATH}.${ARCHIVE_TYPE})
        message(STATUS "Extracting " ${NAME} " to " ${LOCAL_PATH})
        file(ARCHIVE_EXTRACT INPUT ${LOCAL_PATH}.${ARCHIVE_TYPE} DESTINATION ${LOCAL_PATH})
        file(REMOVE ${LOCAL_PATH}.${ARCHIVE_TYPE})
        # Create an empty file so we know which version we have
        file(WRITE ${LOCAL_PATH}/${VERSION}.ver)
    endif()
endfunction()
# use example:
# CheckAndDownloadPackage("DXC" "v1.7.2308" ${CMAKE_CURRENT_SOURCE_DIR}/external/dxc https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.7.2308/dxc_2023_08_14.zip "zip")
#----------------------------------------------------------------------------------------------------------------------


#----------------------------------------------------------------------------------------------------------------------
# Streamline config
#set(FETCHCONTENT_QUIET OFF) <- use for a lot more detailed info from FetchContent
set(CAUSTICA_DEFAULT_WITH_STREAMLINE ON)
option(CAUSTICA_WITH_STREAMLINE "Enable streamline, separate package required" ${CAUSTICA_DEFAULT_WITH_STREAMLINE})
if (CAUSTICA_WITH_STREAMLINE AND NOT WIN32)
    message(WARNING "Streamline integration is currently Windows-only in this codebase; disabling CAUSTICA_WITH_STREAMLINE for this platform.")
    set(CAUSTICA_WITH_STREAMLINE OFF CACHE BOOL "Enable streamline, separate package required" FORCE)
endif()
set(STREAMLINE_DOWNLOAD_PATH "${CMAKE_SOURCE_DIR}/External/Streamline")

set(CAUSTICA_STREAMLINE_FETCH_URL "https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.9.0/streamline-sdk-v2.9.0.zip" CACHE STRING "URL to Streamline package to fetch from https://github.com/NVIDIA-RTX/Streamline/releases")
set(CAUSTICA_STREAMLINE_FETCH_SHA256 "b7e4f31706cfacafba95d2d4abc2c9dcf2dc5fc58b2a6917c71f83051d271aa1" CACHE STRING "SHA256 hash of Streamline package from https://github.com/NVIDIA-RTX/Streamline/releases")
set(CAUSTICA_STREAMLINE_FETCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/Streamline" CACHE STRING "Directory to fetch Streamline to, empty uses build directory default")

option(STREAMLINE_FEATURE_DLSS_SR   "Include DLSS-SR dll"           ON  )
option(STREAMLINE_FEATURE_DLSS_RR   "Include DLSS-RR dll"           ON  )
option(STREAMLINE_FEATURE_NRD       "Include NRD dll"               OFF )
option(STREAMLINE_FEATURE_IMGUI     "Include Imgui dll"             ON  )
option(STREAMLINE_FEATURE_NVPERF    "Include NSight Perf SDK dll"   OFF )
option(STREAMLINE_FEATURE_REFLEX    "Include Reflex dll"            ON  )
option(STREAMLINE_FEATURE_NIS       "Include NIS dll"               OFF )
option(STREAMLINE_FEATURE_DLSS_FG   "Include DLSS-FG dll"           ON  )
option(STREAMLINE_FEATURE_DEEPDVC   "Include DEEPDVC dll"           OFF )
option(STREAMLINE_FEATURE_DIRECTSR  "Include DirectSR dll"          OFF )
#----------------------------------------------------------------------------------------------------------------------

#----------------------------------------------------------------------------------------------------------------------
# Native NGX DLSS config
#
# Use native NGX DLSS integration by default when Vulkan is available.
set(CAUSTICA_DEFAULT_WITH_NATIVE_DLSS ${CAUSTICA_WITH_VULKAN})
option(CAUSTICA_WITH_NATIVE_DLSS "Enable native NVIDIA NGX DLSS integration for Vulkan builds without Streamline" ${CAUSTICA_DEFAULT_WITH_NATIVE_DLSS})
if (CAUSTICA_WITH_NATIVE_DLSS AND CAUSTICA_WITH_STREAMLINE)
    message(WARNING "CAUSTICA_WITH_NATIVE_DLSS is intended for non-Streamline builds; disabling it because CAUSTICA_WITH_STREAMLINE is enabled.")
    set(CAUSTICA_WITH_NATIVE_DLSS OFF CACHE BOOL "Enable native NVIDIA NGX DLSS integration for Vulkan builds without Streamline" FORCE)
endif()
if (CAUSTICA_WITH_NATIVE_DLSS AND NOT CAUSTICA_WITH_VULKAN)
    message(WARNING "CAUSTICA_WITH_NATIVE_DLSS requires CAUSTICA_WITH_VULKAN; disabling native DLSS.")
    set(CAUSTICA_WITH_NATIVE_DLSS OFF CACHE BOOL "Enable native NVIDIA NGX DLSS integration for Vulkan builds without Streamline" FORCE)
endif()
set(CAUSTICA_WITH_DLSS ${CAUSTICA_WITH_NATIVE_DLSS} CACHE BOOL "Enable the NGX DLSS integration used by caustica native DLSS" FORCE)
set(CAUSTICA_DLSS_FETCH_URL "https://github.com/NVIDIA/DLSS.git" CACHE STRING "URL of the DLSS Git repository to fetch from")
set(CAUSTICA_DLSS_FETCH_TAG "a8ed84e2d1cc1efa7bef63fb9394be6cb06c0d74" CACHE STRING "Git commit hash or tag for DLSS")
set(CAUSTICA_DLSS_FETCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/DLSS" CACHE STRING "Directory to fetch DLSS to, empty uses build directory default")
#----------------------------------------------------------------------------------------------------------------------

#----------------------------------------------------------------------------------------------------------------------
# Get and unpack DXC for DirectX shader compilation. Linux/WSL uses the Vulkan SDK DXC.
if (WIN32)
    CheckAndDownloadPackage("DXC" "v1.9.2602.17" ${CMAKE_CURRENT_SOURCE_DIR}/external/dxc https://www.nuget.org/api/v2/package/Microsoft.Direct3D.DXC/1.9.2602.17 "zip")
    set(DXC_CUSTOM_PATH "${CMAKE_SOURCE_DIR}/External/dxc/build/native/bin/x64/" CACHE STRING "Path to embedded dxc")
else()
    if (DEFINED DXC_SPIRV_PATH AND DXC_SPIRV_PATH)
        set(CAUSTICA_DXC_SPIRV_EXECUTABLE "${DXC_SPIRV_PATH}")
    elseif (DEFINED SHADERMAKE_DXC_VK_PATH AND SHADERMAKE_DXC_VK_PATH)
        set(CAUSTICA_DXC_SPIRV_EXECUTABLE "${SHADERMAKE_DXC_VK_PATH}")
    else()
        find_program(CAUSTICA_DXC_SPIRV_EXECUTABLE
            NAMES dxc
            HINTS
                "$ENV{VULKAN_SDK}/bin"
                "$ENV{VULKAN_SDK}/x86_64/bin"
        )
    endif()
    if (NOT CAUSTICA_DXC_SPIRV_EXECUTABLE)
        message(FATAL_ERROR "DXC for SPIR-V was not found. Install the Linux Vulkan SDK in WSL, or set DXC_SPIRV_PATH/SHADERMAKE_DXC_VK_PATH to the dxc executable.")
    endif()
    set(DXC_CUSTOM_PATH "" CACHE STRING "Path to embedded dxc")
endif()
#----------------------------------------------------------------------------------------------------------------------

#----------------------------------------------------------------------------------------------------------------------
# Python scripting via nanobind (embeds CPython into caustica and exposes scene/material/light/ui bindings)
option(CAUSTICA_WITH_PYTHON "Enable embedded Python scripting via nanobind (allows customizing materials, lights, etc. from Python scripts)" ON)
set(CAUSTICA_PYTHON_MIN_VERSION "3.8" CACHE STRING "Minimum required Python version for embedded scripting")
set(CAUSTICA_NANOBIND_GIT_TAG "v2.4.0" CACHE STRING "Git tag/branch of nanobind to fetch")

if (CAUSTICA_WITH_PYTHON)
    # Need both the embedded interpreter (libpython) and the development headers
    find_package(Python ${CAUSTICA_PYTHON_MIN_VERSION} COMPONENTS Interpreter Development REQUIRED)
    message(STATUS "Found Python ${Python_VERSION} at ${Python_EXECUTABLE}")
    message(STATUS "  include dirs : ${Python_INCLUDE_DIRS}")
    message(STATUS "  libraries    : ${Python_LIBRARIES}")

    include(FetchContent)
    if (NOT TARGET nanobind-static)
        FetchContent_Declare(
            nanobind
            GIT_REPOSITORY https://github.com/wjakob/nanobind.git
            GIT_TAG        ${CAUSTICA_NANOBIND_GIT_TAG}
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(nanobind)
    endif()
endif()
#----------------------------------------------------------------------------------------------------------------------

#----------------------------------------------------------------------------------------------------------------------
# Intel Open Image Denoise for offline reference-mode denoising
option(CAUSTICA_WITH_OIDN "Enable Intel Open Image Denoise for Reference path tracing mode" ON)
set(CAUSTICA_OIDN_VERSION "2.4.1" CACHE STRING "Open Image Denoise release version")

if (CAUSTICA_WITH_OIDN)
    if (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CAUSTICA_OIDN_FETCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/OIDN")
        set(CAUSTICA_OIDN_PACKAGE_DIR "${CAUSTICA_OIDN_FETCH_DIR}/oidn-${CAUSTICA_OIDN_VERSION}.x64.windows")
        set(CAUSTICA_OIDN_BIN_DIR "${CAUSTICA_OIDN_PACKAGE_DIR}/bin" CACHE INTERNAL "Open Image Denoise runtime DLL directory")
        set(CAUSTICA_OIDN_LIBRARY_DIR "${CAUSTICA_OIDN_PACKAGE_DIR}/bin" CACHE INTERNAL "Open Image Denoise runtime library directory")

        CheckAndDownloadPackage(
            "Open Image Denoise"
            "v${CAUSTICA_OIDN_VERSION}.x64.windows"
            ${CAUSTICA_OIDN_FETCH_DIR}
            "https://github.com/RenderKit/oidn/releases/download/v${CAUSTICA_OIDN_VERSION}/oidn-${CAUSTICA_OIDN_VERSION}.x64.windows.zip"
            "zip")
    elseif (UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(CAUSTICA_OIDN_FETCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/OIDN")
        set(CAUSTICA_OIDN_PACKAGE_DIR "${CAUSTICA_OIDN_FETCH_DIR}/oidn-${CAUSTICA_OIDN_VERSION}.x86_64.linux")
        set(CAUSTICA_OIDN_LIBRARY_DIR "${CAUSTICA_OIDN_PACKAGE_DIR}/lib" CACHE INTERNAL "Open Image Denoise runtime library directory")

        CheckAndDownloadPackage(
            "Open Image Denoise"
            "v${CAUSTICA_OIDN_VERSION}.x86_64.linux"
            ${CAUSTICA_OIDN_FETCH_DIR}
            "https://github.com/RenderKit/oidn/releases/download/v${CAUSTICA_OIDN_VERSION}/oidn-${CAUSTICA_OIDN_VERSION}.x86_64.linux.tar.gz"
            "tar.gz")
    else()
        message(WARNING "CAUSTICA_WITH_OIDN is supported for x64 Windows and x86_64 Linux packages; disabling OIDN for this platform.")
        set(CAUSTICA_WITH_OIDN OFF CACHE BOOL "Enable Intel Open Image Denoise for Reference path tracing mode" FORCE)
    endif()

    if (CAUSTICA_WITH_OIDN)
        find_package(OpenImageDenoise ${CAUSTICA_OIDN_VERSION} CONFIG REQUIRED
            PATHS
                "${CAUSTICA_OIDN_PACKAGE_DIR}"
                "${CAUSTICA_OIDN_PACKAGE_DIR}/lib/cmake/OpenImageDenoise-${CAUSTICA_OIDN_VERSION}"
            NO_DEFAULT_PATH)

        foreach(_oidn_target OpenImageDenoise OpenImageDenoise_core)
            if (TARGET ${_oidn_target})
                set_target_properties(${_oidn_target} PROPERTIES
                    MAP_IMPORTED_CONFIG_DEBUG Release
                    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
                    MAP_IMPORTED_CONFIG_MINSIZEREL Release)
            endif()
        endforeach()
    endif()
endif()
#----------------------------------------------------------------------------------------------------------------------


#----------------------------------------------------------------------------------------------------------------------
# Get and unpack the Agility SDK
set(CAUSTICA_D3D_AGILITY_SDK_VERSION_NAME "1.619.0" CACHE STRING "Optional Agility SDK version to download")
set_property(CACHE CAUSTICA_D3D_AGILITY_SDK_VERSION_NAME PROPERTY STRINGS "1.619.0" "1.717.0-preview" "1.719.0-preview" "")

if (WIN32 AND CAUSTICA_D3D_AGILITY_SDK_VERSION_NAME)
    # Extract the numeric SDK version (e.g. "1.717.0-preview" -> "717")
    string(REGEX MATCH "^[0-9]+\\.([0-9]+)\\." _agility_ver_match "${CAUSTICA_D3D_AGILITY_SDK_VERSION_NAME}")
    set(_agility_numeric_ver "${CMAKE_MATCH_1}")
    if (NOT _agility_numeric_ver)
        message(FATAL_ERROR "Could not extract numeric version from: ${CAUSTICA_D3D_AGILITY_SDK_VERSION_NAME}")
    endif()

    set(CAUSTICA_D3D_AGILITY_SDK_PATH ${CMAKE_CURRENT_SOURCE_DIR}/external/AgilitySDK/${CAUSTICA_D3D_AGILITY_SDK_VERSION_NAME} CACHE STRING "AgilitySDKPath" FORCE)
    set(CAUSTICA_D3D_AGILITY_SDK_VERSION "${_agility_numeric_ver}" CACHE STRING "AgilitySDKVersion" FORCE)
    CheckAndDownloadPackage("Agility SDK" "${CAUSTICA_D3D_AGILITY_SDK_VERSION_NAME}"
        ${CAUSTICA_D3D_AGILITY_SDK_PATH} "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/${CAUSTICA_D3D_AGILITY_SDK_VERSION_NAME}" "zip")
else()
    set(CAUSTICA_D3D_AGILITY_SDK_PATH "" CACHE STRING "AgilitySDKPath" FORCE)
    set(CAUSTICA_D3D_AGILITY_SDK_VERSION "" CACHE STRING "AgilitySDKVersion" FORCE)
endif()

#----------------------------------------------------------------------------------------------------------------------
if (WIN32)
    set(CAUSTICA_DEFAULT_WITH_NVAPI ON)
else()
    set(CAUSTICA_DEFAULT_WITH_NVAPI OFF)
endif()
option(NVRHI_WITH_NVAPI "Include NVAPI support (requires NVAPI SDK)" ${CAUSTICA_DEFAULT_WITH_NVAPI})
set(NVAPI_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/External/nvapi/" CACHE STRING "Path to NVAPI include headers/shaders" )
set(NVAPI_LIBRARY "${CMAKE_SOURCE_DIR}/External/nvapi/amd64/nvapi64.lib" CACHE STRING "Path to NVAPI .lib file")

if (WIN32)
    set(DXC_PATH "${DXC_CUSTOM_PATH}/dxc.exe" CACHE STRING "Path to embedded dxc file for DX12")
    set(DXC_DXIL_EXECUTABLE "${DXC_CUSTOM_PATH}/dxc.exe" CACHE STRING "Path to embedded dxc file for DX12")
    set(DXC_SPIRV_PATH "${DXC_CUSTOM_PATH}/dxc.exe" CACHE STRING "Path to embedded dxc file for SPIRV")
else()
    set(DXC_PATH "" CACHE STRING "Path to embedded dxc file for DX12")
    set(DXC_DXIL_EXECUTABLE "" CACHE STRING "Path to embedded dxc file for DX12")
    set(DXC_SPIRV_PATH "${CAUSTICA_DXC_SPIRV_EXECUTABLE}" CACHE STRING "Path to dxc file for SPIRV")
endif()

set(SHADERMAKE_FIND_DXC OFF CACHE BOOL  "" FORCE)
set(SHADERMAKE_FIND_DXC_VK OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_DXC_PATH "${DXC_PATH}" CACHE STRING "" FORCE)
set(SHADERMAKE_DXC_VK_PATH "${DXC_SPIRV_PATH}" CACHE STRING "" FORCE)

# option(USE_RAY_TRACING_DENOISER "Include NRD as part of the sample." ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CAUSTICA_SHADERS_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/engine")
option(CAUSTICA_WITH_LZ4 "Include LZ4" OFF)

if (CAUSTICA_WITH_NVRHI)
    # NVRHI backend variables: forwarded from the project-wide CAUSTICA settings.
    set(NVRHI_WITH_DX11 "${CAUSTICA_WITH_DX11}" CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX12 "${CAUSTICA_WITH_DX12}" CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VULKAN "${CAUSTICA_WITH_VULKAN}" CACHE BOOL "" FORCE)
    set(NVRHI_WITH_AFTERMATH "${CAUSTICA_WITH_AFTERMATH}" CACHE BOOL "" FORCE)

    # Engine RHI backend (moved from External/nvrhi to caustica/backend/nvrhi)
    add_subdirectory(caustica/caustica/src/backend/rhi ${CMAKE_BINARY_DIR}/caustica/caustica/backend)
endif()

add_subdirectory("External")

# Default to using DXR OMMs if we can
if(NVRHI_WITH_DX12 AND CAUSTICA_D3D_AGILITY_SDK_VERSION AND (CAUSTICA_D3D_AGILITY_SDK_VERSION GREATER_EQUAL "619"))
    set(NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP ON CACHE BOOL "" FORCE)
else()
    set(NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP OFF CACHE BOOL "" FORCE)
endif()

if (NVRHI_WITH_VULKAN OR NVRHI_WITH_DX12)
	add_subdirectory(caustica)
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT caustica)
endif()
#----------------------------------------------------------------------------------------------------------------------


#----------------------------------------------------------------------------------------------------------------------
# Alternative way of obtaining assets (we're currently relying on git submodule but this is an option as well)
#include(FetchContent)
#set(CAUSTICA_ASSETS_FETCH_URL "https://gitlab-master.nvidia.com/rtx/path-tracing-sdk-assets")
#set(CAUSTICA_ASSETS_FETCH_TAG "main")
#set(CAUSTICA_ASSETS_FETCH_DIR "${CMAKE_SOURCE_DIR}/Assets")
#message(STATUS "Updating assets from " ${CAUSTICA_ASSETS_FETCH_URL} ", tag " ${CAUSTICA_ASSETS_FETCH_TAG} ", into folder " ${CAUSTICA_ASSETS_FETCH_DIR} )
#FetchContent_Declare(assets_download  GIT_REPOSITORY ${CAUSTICA_ASSETS_FETCH_URL}  GIT_TAG ${CAUSTICA_ASSETS_FETCH_TAG}  SOURCE_DIR ${CAUSTICA_ASSETS_FETCH_DIR}  GIT_PROGRESS TRUE)
#FetchContent_MakeAvailable(assets_download)
#----------------------------------------------------------------------------------------------------------------------
