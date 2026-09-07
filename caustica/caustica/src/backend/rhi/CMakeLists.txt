#
set(RHI_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/caustica/caustica/include/backend/rhi")

include(CMakeDependentOption)

option(CAUSTICA_RHI_WITH_ACCEL_STRUCT_MANAGER
    "Use Caustica's internal acceleration-structure memory manager"
    OFF)

if (CAUSTICA_RHI_WITH_DX12)
    option(CAUSTICA_RHI_D3D12_WITH_DXR12_OPACITY_MICROMAP "Use D3D12 native Opacity Micromaps from DXR 1.2" OFF)
endif()

if (MSVC)
    string(REGEX REPLACE "/W[0-4]\\s*" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(APPEND CMAKE_CXX_FLAGS " /W4")
    if (NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        string(APPEND CMAKE_CXX_FLAGS " /MP")
    endif()
endif()

# Global third-party submodules (resolved from repo-root thirdparty/)
if (CAUSTICA_RHI_WITH_VULKAN AND NOT TARGET Vulkan-Headers AND NOT TARGET Vulkan::Headers)
    add_subdirectory(${CMAKE_SOURCE_DIR}/thirdparty/Vulkan-Headers
                     ${CMAKE_BINARY_DIR}/thirdparty/Vulkan-Headers)
endif()
if (CAUSTICA_RHI_WITH_DX12 AND NOT TARGET DirectX-Headers AND NOT TARGET Microsoft::DirectX-Headers)
    add_subdirectory(${CMAKE_SOURCE_DIR}/thirdparty/DirectX-Headers
                     ${CMAKE_BINARY_DIR}/thirdparty/DirectX-Headers)
endif()

if (CAUSTICA_RHI_WITH_NVAPI AND NOT TARGET nvapi)
    add_library(nvapi STATIC IMPORTED GLOBAL)
    target_include_directories(nvapi INTERFACE "${NVAPI_INCLUDE_DIR}")
    set_property(TARGET nvapi PROPERTY IMPORTED_LOCATION "${NVAPI_LIBRARY}")
endif()

# NOTE: Aftermath SDK fetch was previously fetched via Aftermath CMake helper.
# Aftermath support can be re-enabled by providing the NSight Aftermath SDK.
# if (CAUSTICA_RHI_WITH_AFTERMATH AND NOT TARGET aftermath)
#     include(...)  # Path to FetchAftermath.cmake
# endif()

# ------------------------------------------------------------------
# Common headers and sources
# ------------------------------------------------------------------
set(include_common
    ${RHI_INCLUDE_DIR}/rhi.h
    ${RHI_INCLUDE_DIR}/rhi_types.h
    ${RHI_INCLUDE_DIR}/device_factory.h
    ${RHI_INCLUDE_DIR}/rhi_hlsl.h
    ${RHI_INCLUDE_DIR}/caustica.h
    ${RHI_INCLUDE_DIR}/caustica/format.h
    ${RHI_INCLUDE_DIR}/utils.h
    ${RHI_INCLUDE_DIR}/command_list_pool.h
    ${RHI_INCLUDE_DIR}/common/containers.h
    ${RHI_INCLUDE_DIR}/common/misc.h
    ${RHI_INCLUDE_DIR}/common/resource.h
    ${RHI_INCLUDE_DIR}/common/deferred-deletion.h
    ${RHI_INCLUDE_DIR}/common/aftermath.h)

set(src_common
    common/FormatInfo.cpp
    common/DeviceFactory.cpp
    common/CausticaFormat.cpp
    common/Misc.cpp
    common/StateTracking.cpp
    common/StateTracking.h
    common/Utils.cpp
    common/CommandListPool.cpp
    common/Aftermath.cpp)

set(accel_struct_manager_dir ${CMAKE_CURRENT_SOURCE_DIR}/internal/accel_struct)
set(accel_struct_manager_common
    ${accel_struct_manager_dir}/AccelStructManager.h
    ${accel_struct_manager_dir}/Logger.h
    ${accel_struct_manager_dir}/Logger.cpp
    ${accel_struct_manager_dir}/Suballocator.h
    ${accel_struct_manager_dir}/LICENSE.txt)
set(accel_struct_manager_d3d12
    ${accel_struct_manager_dir}/D3D12AccelStructManager.h
    ${accel_struct_manager_dir}/D3D12AccelStructManager.cpp
    ${accel_struct_manager_dir}/D3D12Suballocator.h
    ${accel_struct_manager_dir}/D3D12Suballocator.cpp)
set(accel_struct_manager_vulkan
    ${accel_struct_manager_dir}/VkAccelStructManager.h
    ${accel_struct_manager_dir}/VkAccelStructManager.cpp
    ${accel_struct_manager_dir}/VulkanSuballocator.h
    ${accel_struct_manager_dir}/VulkanSuballocator.cpp)

# ------------------------------------------------------------------
# Validation layer
# ------------------------------------------------------------------
set(include_validation
    ${RHI_INCLUDE_DIR}/validation.h)
set(src_validation
    validation/ValidationCommandList.cpp
    validation/ValidationDevice.cpp
    validation/ValidationBackend.h)

# ------------------------------------------------------------------
# D3D12 backend
# ------------------------------------------------------------------
set(include_d3d12
    ${RHI_INCLUDE_DIR}/d3d12.h)
set(src_d3d12
    common/DxgiFormat.h
    common/DxgiFormat.cpp
    common/Versioning.h
    d3d12/D3D12Buffer.cpp
    d3d12/D3D12CommandList.cpp
    d3d12/D3D12Compute.cpp
    d3d12/D3D12Constants.cpp
    d3d12/D3D12Backend.h
    d3d12/D3D12DescriptorHeap.cpp
    d3d12/D3D12Device.cpp
    d3d12/D3D12Graphics.cpp
    d3d12/D3D12Meshlets.cpp
    d3d12/D3D12Queries.cpp
    d3d12/D3D12RayTracing.cpp
    d3d12/D3D12ResourceBindings.cpp
    d3d12/D3D12Shader.cpp
    d3d12/D3D12StateTracking.cpp
    d3d12/D3D12Texture.cpp
    d3d12/D3D12Upload.cpp)

# ------------------------------------------------------------------
# Vulkan backend
# ------------------------------------------------------------------
set(include_vk
    ${RHI_INCLUDE_DIR}/vulkan.h)
set(src_vk
    common/Versioning.h
    vulkan/VulkanAllocator.cpp
    vulkan/VulkanBuffer.cpp
    vulkan/VulkanCommandList.cpp
    vulkan/VulkanCompute.cpp
    vulkan/VulkanConstants.cpp
    vulkan/VulkanDevice.cpp
    vulkan/VulkanGraphics.cpp
    vulkan/VulkanMeshlets.cpp
    vulkan/VulkanQueries.cpp
    vulkan/VulkanQueue.cpp
    vulkan/VulkanRayTracing.cpp
    vulkan/VulkanResourceBindings.cpp
    vulkan/VulkanShader.cpp
    vulkan/VulkanStagingTexture.cpp
    vulkan/VulkanStateTracking.cpp
    vulkan/VulkanTexture.cpp
    vulkan/VulkanUpload.cpp
    vulkan/VulkanBackend.h)

# ==================================================================
# caustica_rhi target: core interface + common implementation
# ==================================================================
if (CAUSTICA_RHI_BUILD_SHARED)
    add_library(caustica_rhi SHARED
        ${include_common}
        ${src_common})
else()
    add_library(caustica_rhi STATIC
        ${include_common}
        ${src_common})
endif()

if (CAUSTICA_RHI_BUILD_SHARED)
    target_compile_definitions(caustica_rhi PRIVATE CAUSTICA_RHI_SHARED_LIBRARY_BUILD=1)
    if(CAUSTICA_RHI_WITH_VULKAN)
        target_compile_definitions(caustica_rhi PRIVATE VULKAN_HPP_STORAGE_SHARED VULKAN_HPP_STORAGE_SHARED_EXPORT)
    endif()
    set_property(TARGET caustica_rhi PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if (CAUSTICA_RHI_WITH_VALIDATION)
    target_sources(caustica_rhi PRIVATE
        ${include_validation}
        ${src_validation})
endif()

target_include_directories(caustica_rhi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/caustica/caustica/include/backend>)

set_target_properties(caustica_rhi PROPERTIES FOLDER "caustica/backend")


target_compile_definitions(caustica_rhi PRIVATE CAUSTICA_RHI_WITH_AFTERMATH=$<BOOL:${CAUSTICA_RHI_WITH_AFTERMATH}>)

# ==================================================================
# D3D12 backend
# ==================================================================
if (CAUSTICA_RHI_WITH_DX12)
    if (CAUSTICA_RHI_BUILD_SHARED)
        set(caustica_rhi_d3d12_target caustica_rhi)
        target_sources(${caustica_rhi_d3d12_target} PRIVATE
            ${include_d3d12}
            ${src_d3d12})
    else()
        set(caustica_rhi_d3d12_target caustica_rhi_d3d12)
        add_library(${caustica_rhi_d3d12_target} STATIC
            ${include_d3d12}
            ${src_d3d12})
        set_target_properties(${caustica_rhi_d3d12_target} PROPERTIES FOLDER "caustica/backend")
        target_include_directories(${caustica_rhi_d3d12_target} PRIVATE ${CMAKE_SOURCE_DIR}/caustica/caustica/include/backend)
    endif()

    if (CAUSTICA_RHI_WITH_ACCEL_STRUCT_MANAGER)
        target_sources(${caustica_rhi_d3d12_target} PRIVATE
            ${accel_struct_manager_common}
            ${accel_struct_manager_d3d12})
        target_include_directories(${caustica_rhi_d3d12_target} PRIVATE ${accel_struct_manager_dir})
        target_compile_definitions(${caustica_rhi_d3d12_target} PRIVATE CAUSTICA_RHI_WITH_ACCEL_STRUCT_MANAGER=1)
    endif()

    target_compile_definitions(${caustica_rhi_d3d12_target} PRIVATE CAUSTICA_RHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=$<BOOL:${CAUSTICA_RHI_D3D12_WITH_DXR12_OPACITY_MICROMAP}>)

    target_link_libraries(${caustica_rhi_d3d12_target} PUBLIC Microsoft::DirectX-Headers Microsoft::DirectX-Guids d3d12)

    if (CAUSTICA_RHI_WITH_NVAPI)
        target_link_libraries(${caustica_rhi_d3d12_target} PUBLIC nvapi)
    endif()
    target_compile_definitions(${caustica_rhi_d3d12_target} PRIVATE CAUSTICA_RHI_D3D12_WITH_NVAPI=$<BOOL:${CAUSTICA_RHI_WITH_NVAPI}>)

    if (CAUSTICA_RHI_WITH_AFTERMATH)
        target_link_libraries(${caustica_rhi_d3d12_target} PUBLIC aftermath)
    endif()
    target_compile_definitions(${caustica_rhi_d3d12_target} PRIVATE CAUSTICA_RHI_WITH_AFTERMATH=$<BOOL:${CAUSTICA_RHI_WITH_AFTERMATH}>)
endif()

# ==================================================================
# Vulkan backend
# ==================================================================
if (CAUSTICA_RHI_WITH_VULKAN)
    if (CAUSTICA_RHI_BUILD_SHARED)
        set(caustica_rhi_vk_target caustica_rhi)
        target_sources(${caustica_rhi_vk_target} PRIVATE
            ${include_vk}
            ${src_vk})
    else()
        set(caustica_rhi_vk_target caustica_rhi_vk)
        add_library(${caustica_rhi_vk_target} STATIC
            ${include_vk}
            ${src_vk})
        set_target_properties(${caustica_rhi_vk_target} PROPERTIES FOLDER "caustica/backend")
        target_include_directories(${caustica_rhi_vk_target} PRIVATE ${CMAKE_SOURCE_DIR}/caustica/caustica/include/backend)
        target_link_libraries(${caustica_rhi_vk_target} PUBLIC caustica_rhi)
    endif()

    if(WIN32)
        target_compile_definitions(${caustica_rhi_vk_target} PUBLIC VK_USE_PLATFORM_WIN32_KHR)
        target_compile_definitions(${caustica_rhi_vk_target} PRIVATE NOMINMAX)
    endif()

    if (CAUSTICA_RHI_WITH_ACCEL_STRUCT_MANAGER)
        target_sources(${caustica_rhi_vk_target} PRIVATE ${accel_struct_manager_vulkan})
        if (NOT CAUSTICA_RHI_BUILD_SHARED OR NOT CAUSTICA_RHI_WITH_DX12)
            target_sources(${caustica_rhi_vk_target} PRIVATE ${accel_struct_manager_common})
        endif()
        target_include_directories(${caustica_rhi_vk_target} PRIVATE ${accel_struct_manager_dir})
        target_compile_definitions(${caustica_rhi_vk_target} PRIVATE CAUSTICA_RHI_WITH_ACCEL_STRUCT_MANAGER=1)
    endif()

    if (TARGET Vulkan-Headers)
        target_link_libraries(${caustica_rhi_vk_target} PRIVATE Vulkan-Headers)
    elseif (TARGET Vulkan::Headers)
        target_link_libraries(${caustica_rhi_vk_target} PRIVATE Vulkan::Headers)
    endif()

    if (CAUSTICA_RHI_WITH_AFTERMATH)
        target_link_libraries(${caustica_rhi_vk_target} PUBLIC aftermath)
    endif()
    target_compile_definitions(${caustica_rhi_vk_target} PRIVATE CAUSTICA_RHI_WITH_AFTERMATH=$<BOOL:${CAUSTICA_RHI_WITH_AFTERMATH}>)
endif()
