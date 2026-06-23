#
# Copyright (c) 2014-2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto. Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

# Engine RHI backend: internal rendering hardware interface library.
# Derived from NVRHI, now absorbed into the engine source tree.
# Headers are at caustica/caustica/include/backend/rhi/
# Sources are here at caustica/caustica/src/backend/rhi/

set(RHI_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/caustica/caustica/include/backend/rhi")

include(CMakeDependentOption)

if (NVRHI_WITH_DX12)
    option(NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP "Use D3D12 native Opacity Micromaps from DXR 1.2" OFF)
endif()

if (MSVC)
    string(REGEX REPLACE "/W[0-4]\\s*" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(APPEND CMAKE_CXX_FLAGS " /W4")
    if (NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        string(APPEND CMAKE_CXX_FLAGS " /MP")
    endif()
endif()

# Global third-party submodules (moved from nvrhi/thirdparty/ to repo root)
if (NVRHI_WITH_VULKAN AND NOT TARGET Vulkan-Headers AND NOT TARGET Vulkan::Headers)
    add_subdirectory(${CMAKE_SOURCE_DIR}/thirdparty/Vulkan-Headers
                     ${CMAKE_BINARY_DIR}/thirdparty/Vulkan-Headers)
endif()
if (NVRHI_WITH_DX12 AND NOT TARGET DirectX-Headers AND NOT TARGET Microsoft::DirectX-Headers)
    add_subdirectory(${CMAKE_SOURCE_DIR}/thirdparty/DirectX-Headers
                     ${CMAKE_BINARY_DIR}/thirdparty/DirectX-Headers)
endif()

if (NVRHI_WITH_RTXMU)
    if (TARGET Vulkan-Headers)
        get_target_property(RTXMU_VULKAN_INCLUDE_DIR Vulkan-Headers INTERFACE_INCLUDE_DIRECTORIES)
    elseif (TARGET Vulkan::Headers)
        get_target_property(RTXMU_VULKAN_INCLUDE_DIR Vulkan::Headers INTERFACE_INCLUDE_DIRECTORIES)
    endif()

    option(RTXMU_WITH_VULKAN "" ${NVRHI_WITH_VULKAN})
    option(RTXMU_WITH_D3D12 "" ${NVRHI_WITH_DX12})

    if (NOT TARGET rtxmu)
        add_subdirectory(${CMAKE_SOURCE_DIR}/rtxmu
                         ${CMAKE_BINARY_DIR}/rtxmu)
    endif()
endif()

if (NVRHI_WITH_NVAPI AND NOT TARGET nvapi)
    add_library(nvapi STATIC IMPORTED GLOBAL)
    target_include_directories(nvapi INTERFACE "${NVAPI_INCLUDE_DIR}")
    set_property(TARGET nvapi PROPERTY IMPORTED_LOCATION "${NVAPI_LIBRARY}")
endif()

# NOTE: Aftermath SDK fetch was previously in nvrhi/cmake/FetchAftermath.cmake.
# Aftermath support can be re-enabled by providing the NSight Aftermath SDK.
# if (NVRHI_WITH_AFTERMATH AND NOT TARGET aftermath)
#     include(...)  # Path to FetchAftermath.cmake
# endif()

# ------------------------------------------------------------------
# Common headers and sources
# ------------------------------------------------------------------
set(include_common
    ${RHI_INCLUDE_DIR}/nvrhi.h
    ${RHI_INCLUDE_DIR}/nvrhiHLSL.h
    ${RHI_INCLUDE_DIR}/utils.h
    ${RHI_INCLUDE_DIR}/common/containers.h
    ${RHI_INCLUDE_DIR}/common/misc.h
    ${RHI_INCLUDE_DIR}/common/resource.h
    ${RHI_INCLUDE_DIR}/common/aftermath.h)

set(src_common
    common/format-info.cpp
    common/misc.cpp
    common/state-tracking.cpp
    common/state-tracking.h
    common/utils.cpp
    common/aftermath.cpp)

# ------------------------------------------------------------------
# Validation layer
# ------------------------------------------------------------------
set(include_validation
    ${RHI_INCLUDE_DIR}/validation.h)
set(src_validation
    validation/validation-commandlist.cpp
    validation/validation-device.cpp
    validation/validation-backend.h)

# ------------------------------------------------------------------
# D3D11 backend
# ------------------------------------------------------------------
set(include_d3d11
    ${RHI_INCLUDE_DIR}/d3d11.h)
set(src_d3d11
    common/dxgi-format.h
    common/dxgi-format.cpp
    d3d11/d3d11-buffer.cpp
    d3d11/d3d11-commandlist.cpp
    d3d11/d3d11-compute.cpp
    d3d11/d3d11-constants.cpp
    d3d11/d3d11-backend.h
    d3d11/d3d11-device.cpp
    d3d11/d3d11-graphics.cpp
    d3d11/d3d11-queries.cpp
    d3d11/d3d11-resource-bindings.cpp
    d3d11/d3d11-shader.cpp
    d3d11/d3d11-texture.cpp)

# ------------------------------------------------------------------
# D3D12 backend
# ------------------------------------------------------------------
set(include_d3d12
    ${RHI_INCLUDE_DIR}/d3d12.h)
set(src_d3d12
    common/dxgi-format.h
    common/dxgi-format.cpp
    common/versioning.h
    d3d12/d3d12-buffer.cpp
    d3d12/d3d12-commandlist.cpp
    d3d12/d3d12-compute.cpp
    d3d12/d3d12-constants.cpp
    d3d12/d3d12-backend.h
    d3d12/d3d12-descriptor-heap.cpp
    d3d12/d3d12-device.cpp
    d3d12/d3d12-graphics.cpp
    d3d12/d3d12-meshlets.cpp
    d3d12/d3d12-queries.cpp
    d3d12/d3d12-raytracing.cpp
    d3d12/d3d12-resource-bindings.cpp
    d3d12/d3d12-shader.cpp
    d3d12/d3d12-state-tracking.cpp
    d3d12/d3d12-texture.cpp
    d3d12/d3d12-upload.cpp)

# ------------------------------------------------------------------
# Vulkan backend
# ------------------------------------------------------------------
set(include_vk
    ${RHI_INCLUDE_DIR}/vulkan.h)
set(src_vk
    common/versioning.h
    vulkan/vulkan-allocator.cpp
    vulkan/vulkan-buffer.cpp
    vulkan/vulkan-commandlist.cpp
    vulkan/vulkan-compute.cpp
    vulkan/vulkan-constants.cpp
    vulkan/vulkan-device.cpp
    vulkan/vulkan-graphics.cpp
    vulkan/vulkan-meshlets.cpp
    vulkan/vulkan-queries.cpp
    vulkan/vulkan-queue.cpp
    vulkan/vulkan-raytracing.cpp
    vulkan/vulkan-resource-bindings.cpp
    vulkan/vulkan-shader.cpp
    vulkan/vulkan-staging-texture.cpp
    vulkan/vulkan-state-tracking.cpp
    vulkan/vulkan-texture.cpp
    vulkan/vulkan-upload.cpp
    vulkan/vulkan-backend.h)

# ==================================================================
# nvrhi target: core interface + common implementation
# ==================================================================
if (NVRHI_BUILD_SHARED)
    add_library(nvrhi SHARED
        ${include_common}
        ${src_common})
else()
    add_library(nvrhi STATIC
        ${include_common}
        ${src_common})
endif()

if (NVRHI_BUILD_SHARED)
    target_compile_definitions(nvrhi PRIVATE NVRHI_SHARED_LIBRARY_BUILD=1)
    if(NVRHI_WITH_VULKAN)
        target_compile_definitions(nvrhi PRIVATE VULKAN_HPP_STORAGE_SHARED VULKAN_HPP_STORAGE_SHARED_EXPORT)
    endif()
    set_property(TARGET nvrhi PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if (NVRHI_WITH_VALIDATION)
    target_sources(nvrhi PRIVATE
        ${include_validation}
        ${src_validation})
endif()

target_include_directories(nvrhi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/caustica/caustica/include/backend>)

set_target_properties(nvrhi PROPERTIES FOLDER "caustica/backend")

target_compile_definitions(nvrhi PRIVATE NVRHI_WITH_AFTERMATH=$<BOOL:${NVRHI_WITH_AFTERMATH}>)

# ==================================================================
# D3D11 backend
# ==================================================================
if (NVRHI_WITH_DX11)
    if (NVRHI_BUILD_SHARED)
        set(nvrhi_d3d11_target nvrhi)
        target_sources(${nvrhi_d3d11_target} PRIVATE
            ${include_d3d11}
            ${src_d3d11})
    else()
        set(nvrhi_d3d11_target nvrhi_d3d11)
        add_library(${nvrhi_d3d11_target} STATIC
            ${include_d3d11}
            ${src_d3d11})
        set_target_properties(${nvrhi_d3d11_target} PROPERTIES FOLDER "caustica/backend")
        target_include_directories(${nvrhi_d3d11_target} PRIVATE ${CMAKE_SOURCE_DIR}/caustica/caustica/include/backend)
    endif()

    target_link_libraries(${nvrhi_d3d11_target} PUBLIC d3d11 dxguid)

    if (NVRHI_WITH_NVAPI)
        target_link_libraries(${nvrhi_d3d11_target} PUBLIC nvapi)
    endif()
    target_compile_definitions(${nvrhi_d3d11_target} PRIVATE NVRHI_D3D11_WITH_NVAPI=$<BOOL:${NVRHI_WITH_NVAPI}>)

    if (NVRHI_WITH_AFTERMATH)
        target_link_libraries(${nvrhi_d3d11_target} PUBLIC aftermath)
    endif()
    target_compile_definitions(${nvrhi_d3d11_target} PRIVATE NVRHI_WITH_AFTERMATH=$<BOOL:${NVRHI_WITH_AFTERMATH}>)
endif()

# ==================================================================
# D3D12 backend
# ==================================================================
if (NVRHI_WITH_DX12)
    if (NVRHI_BUILD_SHARED)
        set(nvrhi_d3d12_target nvrhi)
        target_sources(${nvrhi_d3d12_target} PRIVATE
            ${include_d3d12}
            ${src_d3d12})
    else()
        set(nvrhi_d3d12_target nvrhi_d3d12)
        add_library(${nvrhi_d3d12_target} STATIC
            ${include_d3d12}
            ${src_d3d12})
        set_target_properties(${nvrhi_d3d12_target} PROPERTIES FOLDER "caustica/backend")
        target_include_directories(${nvrhi_d3d12_target} PRIVATE ${CMAKE_SOURCE_DIR}/caustica/caustica/include/backend)
    endif()

    if (NVRHI_WITH_RTXMU)
        target_compile_definitions(${nvrhi_d3d12_target} PUBLIC NVRHI_WITH_RTXMU=1)
        target_link_libraries(${nvrhi_d3d12_target} PUBLIC rtxmu)
    endif()

    target_compile_definitions(${nvrhi_d3d12_target} PRIVATE NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=$<BOOL:${NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP}>)

    target_link_libraries(${nvrhi_d3d12_target} PUBLIC Microsoft::DirectX-Headers Microsoft::DirectX-Guids d3d12)

    if (NVRHI_WITH_NVAPI)
        target_link_libraries(${nvrhi_d3d12_target} PUBLIC nvapi)
    endif()
    target_compile_definitions(${nvrhi_d3d12_target} PRIVATE NVRHI_D3D12_WITH_NVAPI=$<BOOL:${NVRHI_WITH_NVAPI}>)

    if (NVRHI_WITH_AFTERMATH)
        target_link_libraries(${nvrhi_d3d12_target} PUBLIC aftermath)
    endif()
    target_compile_definitions(${nvrhi_d3d12_target} PRIVATE NVRHI_WITH_AFTERMATH=$<BOOL:${NVRHI_WITH_AFTERMATH}>)
endif()

# ==================================================================
# Vulkan backend
# ==================================================================
if (NVRHI_WITH_VULKAN)
    if (NVRHI_BUILD_SHARED)
        set(nvrhi_vulkan_target nvrhi)
        target_sources(${nvrhi_vulkan_target} PRIVATE
            ${include_vk}
            ${src_vk})
    else()
        set(nvrhi_vulkan_target nvrhi_vk)
        add_library(${nvrhi_vulkan_target} STATIC
            ${include_vk}
            ${src_vk})
        set_target_properties(${nvrhi_vulkan_target} PROPERTIES FOLDER "caustica/backend")
        target_include_directories(${nvrhi_vulkan_target} PRIVATE ${CMAKE_SOURCE_DIR}/caustica/caustica/include/backend)
    endif()

    if(WIN32)
        target_compile_definitions(${nvrhi_vulkan_target} PUBLIC VK_USE_PLATFORM_WIN32_KHR)
        target_compile_definitions(${nvrhi_vulkan_target} PRIVATE NOMINMAX)
    endif()

    if (NVRHI_WITH_RTXMU)
        target_compile_definitions(${nvrhi_vulkan_target} PUBLIC NVRHI_WITH_RTXMU=1)
        target_link_libraries(${nvrhi_vulkan_target} PUBLIC rtxmu)
    endif()

    if (TARGET Vulkan-Headers)
        target_link_libraries(${nvrhi_vulkan_target} PRIVATE Vulkan-Headers)
    elseif (TARGET Vulkan::Headers)
        target_link_libraries(${nvrhi_vulkan_target} PRIVATE Vulkan::Headers)
    endif()

    if (NVRHI_WITH_AFTERMATH)
        target_link_libraries(${nvrhi_vulkan_target} PUBLIC aftermath)
    endif()
    target_compile_definitions(${nvrhi_vulkan_target} PRIVATE NVRHI_WITH_AFTERMATH=$<BOOL:${NVRHI_WITH_AFTERMATH}>)
endif()

# ==================================================================
# causticaRhi wrapper targets (expected by OMM)
# ==================================================================
if (NOT TARGET causticaRhi)
    add_library(causticaRhi INTERFACE)
    target_link_libraries(causticaRhi INTERFACE nvrhi)
    set_target_properties(causticaRhi PROPERTIES FOLDER "caustica/backend")
endif()
if (NVRHI_WITH_DX12 AND NOT TARGET causticaRhi_d3d12)
    add_library(causticaRhi_d3d12 INTERFACE)
    target_link_libraries(causticaRhi_d3d12 INTERFACE nvrhi_d3d12)
    set_target_properties(causticaRhi_d3d12 PROPERTIES FOLDER "caustica/backend")
endif()
if (NVRHI_WITH_VULKAN AND NOT TARGET causticaRhi_vk)
    add_library(causticaRhi_vk INTERFACE)
    target_link_libraries(causticaRhi_vk INTERFACE nvrhi_vk)
    set_target_properties(causticaRhi_vk PROPERTIES FOLDER "caustica/backend")
endif()
