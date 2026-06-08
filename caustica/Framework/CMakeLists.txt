# Copyright (c) 2014-2026, NVIDIA CORPORATION. All rights reserved.
#
# Localized framework runtime sources used by caustica. This target intentionally
# excludes donut/render/ToneMappingPasses because caustica owns its tonemapper.

file(GLOB caustica_framework_core_src
    include/donut/core/chunk/*.h
    include/donut/core/math/*.h
    include/donut/core/vfs/TarFile.h
    include/donut/core/vfs/VFS.h
    include/donut/core/*.h
    src/core/chunk/*.cpp
    src/core/math/*.cpp
    src/core/vfs/TarFile.cpp
    src/core/vfs/VFS.cpp
    src/core/*.cpp
)

file(GLOB caustica_framework_engine_src
    include/donut/engine/*.h
    src/engine/*.cpp
    src/engine/*.c
    src/engine/*.h
)

file(GLOB caustica_framework_app_src
    LIST_DIRECTORIES false
    include/donut/app/*.h
    src/app/*.cpp
)

file(GLOB caustica_framework_render_src
    LIST_DIRECTORIES false
    include/donut/render/*.h
    src/render/*.cpp
)

list(FILTER caustica_framework_render_src EXCLUDE REGEX ".*/ToneMappingPasses\\.(h|cpp)$")

if (WIN32)
    list(APPEND caustica_framework_core_src
        include/donut/core/vfs/WinResFS.h
        src/core/vfs/WinResFS.cpp
    )
endif()

if (DONUT_WITH_STREAMLINE)
    list(APPEND caustica_framework_app_src
        src/app/streamline/StreamlineIntegration.cpp
        src/app/streamline/StreamlineIntegration.h
    )
endif()

if (DONUT_WITH_DX11)
    list(APPEND caustica_framework_app_src src/app/dx11/DeviceManager_DX11.cpp)
endif()

if (DONUT_WITH_DX12)
    list(APPEND caustica_framework_app_src src/app/dx12/DeviceManager_DX12.cpp)
endif()

if (DONUT_WITH_VULKAN)
    list(APPEND caustica_framework_app_src src/app/vulkan/DeviceManager_VK.cpp)
endif()

if (DONUT_WITH_AFTERMATH)
    list(APPEND caustica_framework_app_src src/app/aftermath/AftermathCrashDump.cpp)
endif()

if (MSVC)
    list(APPEND caustica_framework_core_src donut.natvis)
endif()

add_library(causticaFramework STATIC
    ${caustica_framework_core_src}
    ${caustica_framework_engine_src}
    ${caustica_framework_app_src}
    ${caustica_framework_render_src}
)

target_include_directories(causticaFramework
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "$<$<BOOL:${DONUT_WITH_STREAMLINE}>:${CMAKE_CURRENT_SOURCE_DIR}/src/app/streamline>"
)

target_link_libraries(causticaFramework
    PUBLIC
        nvrhi
        jsoncpp_static
        stb
        cgltf
        ShaderMakeBlob
        glfw
        imgui
)

if (NOT WIN32)
    target_link_libraries(causticaFramework PUBLIC stdc++fs dl pthread)
endif()

if (WIN32)
    target_compile_definitions(causticaFramework PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(causticaFramework PRIVATE DONUT_FORCE_DISCRETE_GPU=$<BOOL:${DONUT_FORCE_DISCRETE_GPU}>)
endif()

target_compile_definitions(causticaFramework
    PUBLIC
        DONUT_WITH_DX11=$<BOOL:${DONUT_WITH_DX11}>
        DONUT_WITH_DX12=$<BOOL:${DONUT_WITH_DX12}>
        DONUT_WITH_VULKAN=$<BOOL:${DONUT_WITH_VULKAN}>
        DONUT_WITH_STATIC_SHADERS=$<BOOL:${DONUT_WITH_STATIC_SHADERS}>
        DONUT_WITH_AFTERMATH=$<BOOL:${DONUT_WITH_AFTERMATH}>
        DONUT_WITH_STREAMLINE=$<BOOL:${DONUT_WITH_STREAMLINE}>
        DONUT_WITH_DLSS=$<BOOL:${DONUT_WITH_DLSS}>
)

if (DONUT_WITH_AUDIO)
    target_link_libraries(causticaFramework PUBLIC Xaudio2)
    target_compile_definitions(causticaFramework PUBLIC DONUT_WITH_AUDIO)
endif()

if (DONUT_WITH_TINYEXR)
    target_link_libraries(causticaFramework PUBLIC tinyexr)
    target_compile_definitions(causticaFramework PUBLIC DONUT_WITH_TINYEXR)
endif()

if (DONUT_WITH_STREAMLINE)
    target_link_libraries(causticaFramework PUBLIC streamline)

    if (DONUT_WITH_VULKAN AND TARGET glfw)
        target_compile_definitions(glfw PRIVATE _GLFW_VULKAN_LIBRARY="sl.interposer.dll")
    endif()

    if (STREAMLINE_FEATURE_DEEPDVC)
        target_compile_definitions(causticaFramework PRIVATE STREAMLINE_FEATURE_DEEPDVC)
    endif()
    if (STREAMLINE_FEATURE_DIRECTSR)
        target_compile_definitions(causticaFramework PRIVATE STREAMLINE_FEATURE_DIRECTSR)
    endif()
    if (STREAMLINE_FEATURE_DLSS_SR)
        target_compile_definitions(causticaFramework PRIVATE STREAMLINE_FEATURE_DLSS_SR)
    endif()
    if (STREAMLINE_FEATURE_DLSS_FG)
        target_compile_definitions(causticaFramework PRIVATE STREAMLINE_FEATURE_DLSS_FG)
    endif()
    if (STREAMLINE_FEATURE_DLSS_RR)
        target_compile_definitions(causticaFramework PRIVATE STREAMLINE_FEATURE_DLSS_RR)
    endif()
    if (STREAMLINE_FEATURE_IMGUI)
        target_compile_definitions(causticaFramework PRIVATE STREAMLINE_FEATURE_IMGUI)
    endif()
    if (STREAMLINE_FEATURE_NIS)
        target_compile_definitions(causticaFramework PRIVATE STREAMLINE_FEATURE_NIS)
    endif()
    if (STREAMLINE_FEATURE_NVPERF)
        target_compile_definitions(causticaFramework PRIVATE STREAMLINE_FEATURE_NVPERF)
    endif()
    if (STREAMLINE_FEATURE_REFLEX)
        target_compile_definitions(causticaFramework PRIVATE STREAMLINE_FEATURE_REFLEX)
    endif()
endif()

if (DONUT_WITH_DX11)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causticaFramework PUBLIC nvrhi_d3d11)
    endif()
    target_link_libraries(causticaFramework PUBLIC d3d11 dxgi dxguid)
endif()

if (DONUT_WITH_DX12)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causticaFramework PUBLIC nvrhi_d3d12)
    endif()
    target_link_libraries(causticaFramework PUBLIC d3d12 dxgi dxguid)
endif()

if (DONUT_WITH_VULKAN)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causticaFramework PUBLIC nvrhi_vk)
    endif()

    if (TARGET Vulkan-Headers)
        target_link_libraries(causticaFramework PUBLIC Vulkan-Headers)
    elseif (TARGET Vulkan::Headers)
        target_link_libraries(causticaFramework PUBLIC Vulkan::Headers)
    endif()
endif()

if (DONUT_WITH_DLSS AND TARGET DLSS)
    target_link_libraries(causticaFramework PUBLIC DLSS)
endif()

if (DONUT_WITH_STATIC_SHADERS)
    target_include_directories(causticaFramework PRIVATE "${CAUSTICA_FRAMEWORK_STATIC_SHADERS_DIR}")
endif()

if (TARGET causticaFrameworkShaders)
    add_dependencies(causticaFramework causticaFrameworkShaders)
endif()

set_target_properties(causticaFramework PROPERTIES FOLDER "caustica/Framework")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    ${caustica_framework_core_src}
    ${caustica_framework_engine_src}
    ${caustica_framework_app_src}
    ${caustica_framework_render_src}
)
