# ---------------------------------------------------------------------------
# causticaBase — Foundation library (core + math, zero rendering dependencies)
# ---------------------------------------------------------------------------

file(GLOB caustica_base_core_headers
    include/core/*.h
    include/core/chunk/*.h
    include/core/vfs/*.h
)

file(GLOB caustica_base_core_sources
    src/core/*.cpp
    src/core/chunk/*.cpp
    src/core/vfs/*.cpp
)
list(FILTER caustica_base_core_sources EXCLUDE REGEX ".*/MediaFileSystem\\.cpp$")

file(GLOB caustica_base_math_headers
    include/math/*.h
)

file(GLOB caustica_base_math_sources
    src/math/*.cpp
)

if (NOT WIN32)
    list(FILTER caustica_base_core_headers EXCLUDE REGEX ".*/WinResFS\\.h$")
    list(FILTER caustica_base_core_sources EXCLUDE REGEX ".*/WinResFS\\.cpp$")
endif()

add_library(causticaBase STATIC
    ${caustica_base_core_headers}
    ${caustica_base_core_sources}
    ${caustica_base_math_headers}
    ${caustica_base_math_sources}
)

target_include_directories(causticaBase
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(causticaBase
    PUBLIC
        jsoncpp_static
        cxxopts
)

if (MSVC)
    target_compile_options(causticaBase PRIVATE /utf-8)
endif()

if (WIN32)
    target_compile_definitions(causticaBase PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(causticaBase PROPERTIES FOLDER "caustica/caustica")

# ---------------------------------------------------------------------------
# causticaEngine — Main engine library
# ---------------------------------------------------------------------------

file(GLOB caustica_engine_engine_src
    include/engine/*.h
    src/engine/*.cpp
    src/engine/*.c
    src/engine/*.h
)

# Core utilities with scene/render dependencies (MediaFileSystem uses FindScenes / SceneRender)
file(GLOB caustica_engine_core_util_src
    include/core/MediaFileSystem.h
    src/core/MediaFileSystem.cpp
)

# Scene layer: ECS, scene graph, scene types, camera (DIVSHOT-style architecture)
file(GLOB caustica_engine_scene_src
    include/scene/*.h
    src/scene/*.cpp
    include/scene/camera/*.h
    src/scene/camera/*.cpp
)

# Audio layer: audio engine and cache
file(GLOB caustica_engine_audio_src
    include/audio/*.h
    src/audio/*.cpp
)

# Animation layer: keyframe animation system
file(GLOB caustica_engine_animation_src
    include/animation/*.h
    src/animation/*.cpp
)

# ImGui layer: integration backends and console
file(GLOB caustica_engine_imgui_src
    include/imgui/*.h
    src/imgui/*.cpp
)

# Asset layer: loading, caching (DIVSHOT-style 4-layer architecture)
file(GLOB caustica_engine_assets_src
    include/assets/loader/*.h
    src/assets/loader/*.cpp
    include/assets/cache/*.h
    src/assets/cache/*.cpp
)

# Render layer: Core infrastructure, GPU utilities, and all Passes
file(GLOB caustica_engine_render_core_src
    include/render/Core/*.h
    src/render/Core/*.cpp
)

file(GLOB caustica_engine_render_gpusort_src
    include/render/GPUSort/*.h
    src/render/GPUSort/*.cpp
)

file(GLOB_RECURSE caustica_engine_render_passes_src
    include/render/Passes/*.h
    include/render/Passes/*.hpp
    src/render/Passes/*.cpp
    src/render/Passes/*.c
)

set(caustica_engine_render_src
    ${caustica_engine_render_core_src}
    ${caustica_engine_render_gpusort_src}
    ${caustica_engine_render_passes_src}
)

if (CAUSTICA_WITH_STREAMLINE)
    list(APPEND caustica_engine_engine_src
        src/engine/streamline/StreamlineIntegration.cpp
        src/engine/streamline/StreamlineIntegration.h
    )
endif()

if (CAUSTICA_WITH_DX11)
    list(APPEND caustica_engine_engine_src
        include/backend/dx11/GpuDevice_DX11.h
        src/backend/dx11/GpuDevice_DX11.cpp)
endif()

if (CAUSTICA_WITH_DX12)
    list(APPEND caustica_engine_engine_src
        include/backend/dx12/GpuDevice_DX12.h
        src/backend/dx12/GpuDevice_DX12.cpp)
endif()

if (CAUSTICA_WITH_VULKAN)
    list(APPEND caustica_engine_engine_src
        include/backend/vulkan/GpuDevice_VK.h
        src/backend/vulkan/GpuDevice_VK.cpp)
endif()

if (CAUSTICA_WITH_AFTERMATH)
    list(APPEND caustica_engine_engine_src src/engine/aftermath/AftermathCrashDump.cpp)
endif()

if (MSVC)
    list(APPEND caustica_engine_engine_src caustica.natvis)
endif()

# Platform layer (Window abstraction)
list(APPEND caustica_engine_engine_src
    include/platform/window.h
    include/platform/glfw_window.h
    src/platform/window.cpp
    src/platform/glfw_window.cpp
)

# Platform layer: Input dispatch (keyboard/mouse/joystick)
list(APPEND caustica_engine_engine_src
    include/platform/Input.h
    src/platform/Input.cpp
)

# Backend layer: GpuDevice + SwapChain
list(APPEND caustica_engine_engine_src
    include/backend/GpuDevice.h
    include/backend/SwapChain.h
        include/backend/GpuDevice.h
        src/backend/GpuDevice.cpp
)

# Engine layer: Application (message loop, frame pacing)
list(APPEND caustica_engine_engine_src
    include/engine/Application.h
    src/engine/Application.cpp
)

# Render passes and infrastructure are now in caustica_engine_render_src (GLOB)

add_library(causticaEngine STATIC
    ${caustica_engine_core_util_src}
    ${caustica_engine_scene_src}
    ${caustica_engine_audio_src}
    ${caustica_engine_animation_src}
    ${caustica_engine_imgui_src}
    ${caustica_engine_engine_src}
    ${caustica_engine_assets_src}
    ${caustica_engine_render_src}
)

target_include_directories(causticaEngine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "$<$<BOOL:${CAUSTICA_WITH_STREAMLINE}>:${CMAKE_CURRENT_SOURCE_DIR}/src/engine/streamline>"
        "${CMAKE_SOURCE_DIR}/External/Nrd/Include"
        "${CMAKE_SOURCE_DIR}/External/Rtxdi/Include"
        "${CMAKE_SOURCE_DIR}/External/Omm/libraries/omm-gpu-nvrhi"
        "${CMAKE_SOURCE_DIR}/External/Omm/libraries/omm-lib/include"
)

target_link_libraries(causticaEngine
    PUBLIC
        causticaBase
        nvrhi
        stb
        cgltf
        ShaderMakeBlob
        glfw
        imgui
)

if (NOT WIN32)
    target_link_libraries(causticaEngine PUBLIC stdc++fs dl pthread)
endif()

if (WIN32)
    target_compile_definitions(causticaEngine PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(causticaEngine PRIVATE CAUSTICA_FORCE_DISCRETE_GPU=$<BOOL:${CAUSTICA_FORCE_DISCRETE_GPU}>)
endif()
if (MSVC)
    target_compile_options(causticaEngine PRIVATE /utf-8)
endif()

target_compile_definitions(causticaEngine
    PUBLIC
        CAUSTICA_WITH_DX11=$<BOOL:${CAUSTICA_WITH_DX11}>
        CAUSTICA_WITH_DX12=$<BOOL:${CAUSTICA_WITH_DX12}>
        CAUSTICA_WITH_VULKAN=$<BOOL:${CAUSTICA_WITH_VULKAN}>
        CAUSTICA_WITH_STATIC_SHADERS=$<BOOL:${CAUSTICA_WITH_STATIC_SHADERS}>
        CAUSTICA_WITH_AFTERMATH=$<BOOL:${CAUSTICA_WITH_AFTERMATH}>
        CAUSTICA_WITH_STREAMLINE=$<BOOL:${CAUSTICA_WITH_STREAMLINE}>
        CAUSTICA_WITH_DLSS=$<BOOL:${CAUSTICA_WITH_DLSS}>
)

if (CAUSTICA_WITH_AUDIO)
    target_link_libraries(causticaEngine PUBLIC Xaudio2)
    target_compile_definitions(causticaEngine PUBLIC CAUSTICA_WITH_AUDIO)
endif()

if (CAUSTICA_WITH_TINYEXR)
    target_link_libraries(causticaEngine PUBLIC tinyexr)
    target_compile_definitions(causticaEngine PUBLIC CAUSTICA_WITH_TINYEXR)
endif()

if (CAUSTICA_WITH_STREAMLINE)
    target_link_libraries(causticaEngine PUBLIC streamline)

    if (CAUSTICA_WITH_VULKAN AND TARGET glfw)
        target_compile_definitions(glfw PRIVATE _GLFW_VULKAN_LIBRARY="sl.interposer.dll")
    endif()

    if (STREAMLINE_FEATURE_DEEPDVC)
        target_compile_definitions(causticaEngine PRIVATE STREAMLINE_FEATURE_DEEPDVC)
    endif()
    if (STREAMLINE_FEATURE_DIRECTSR)
        target_compile_definitions(causticaEngine PRIVATE STREAMLINE_FEATURE_DIRECTSR)
    endif()
    if (STREAMLINE_FEATURE_DLSS_SR)
        target_compile_definitions(causticaEngine PRIVATE STREAMLINE_FEATURE_DLSS_SR)
    endif()
    if (STREAMLINE_FEATURE_DLSS_FG)
        target_compile_definitions(causticaEngine PRIVATE STREAMLINE_FEATURE_DLSS_FG)
    endif()
    if (STREAMLINE_FEATURE_DLSS_RR)
        target_compile_definitions(causticaEngine PRIVATE STREAMLINE_FEATURE_DLSS_RR)
    endif()
    if (STREAMLINE_FEATURE_IMGUI)
        target_compile_definitions(causticaEngine PRIVATE STREAMLINE_FEATURE_IMGUI)
    endif()
    if (STREAMLINE_FEATURE_NIS)
        target_compile_definitions(causticaEngine PRIVATE STREAMLINE_FEATURE_NIS)
    endif()
    if (STREAMLINE_FEATURE_NVPERF)
        target_compile_definitions(causticaEngine PRIVATE STREAMLINE_FEATURE_NVPERF)
    endif()
    if (STREAMLINE_FEATURE_REFLEX)
        target_compile_definitions(causticaEngine PRIVATE STREAMLINE_FEATURE_REFLEX)
    endif()
endif()

if (CAUSTICA_WITH_DX11)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causticaEngine PUBLIC nvrhi_d3d11)
    endif()
    target_link_libraries(causticaEngine PUBLIC d3d11 dxgi dxguid)
endif()

if (CAUSTICA_WITH_DX12)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causticaEngine PUBLIC nvrhi_d3d12)
    endif()
    target_link_libraries(causticaEngine PUBLIC d3d12 dxgi dxguid)
endif()

if (CAUSTICA_WITH_VULKAN)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causticaEngine PUBLIC nvrhi_vk)
    endif()

    if (TARGET Vulkan-Headers)
        target_link_libraries(causticaEngine PUBLIC Vulkan-Headers)
    elseif (TARGET Vulkan::Headers)
        target_link_libraries(causticaEngine PUBLIC Vulkan::Headers)
    endif()
endif()

if (CAUSTICA_WITH_DLSS AND TARGET DLSS)
    target_link_libraries(causticaEngine PUBLIC DLSS)
endif()

if (CAUSTICA_WITH_STATIC_SHADERS)
    target_include_directories(causticaEngine PRIVATE "${CAUSTICA_ENGINE_STATIC_SHADERS_DIR}")
endif()

if (TARGET causticaEngineShaders)
    add_dependencies(causticaEngine causticaEngineShaders)
endif()

set_target_properties(causticaEngine PROPERTIES FOLDER "caustica/caustica")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    ${caustica_engine_core_util_src}
    ${caustica_engine_scene_src}
    ${caustica_engine_audio_src}
    ${caustica_engine_animation_src}
    ${caustica_engine_imgui_src}
    ${caustica_engine_engine_src}
    ${caustica_engine_render_src}
)
