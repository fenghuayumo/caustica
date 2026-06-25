# ============================================================================
# causticaLib — Unified engine library
# ============================================================================
# All engine modules (core, math, events, platform, backend, scene,
# animation, assets, render, engine, imgui, audio) compile into a single
# static library.  The editor application (causticaLib.exe) links against it.
#
# Directory layout (under include/ and src/):
#   core/       — Foundation utilities (logging, threading, VFS, JSON, etc.)
#   math/       — Math library (vector, matrix, quaternion, bounding volumes)
#   events/     — Event system (key, mouse, application events)
#   platform/   — OS abstraction (window, OS, file dialogs)
#   backend/    — GPU device wrappers (DX11, DX12, Vulkan)
#   scene/      — Scene graph, ECS, camera, game model
#   animation/  — Keyframe animation
#   assets/     — Asset system (loaders, cache, registry, pipeline)
#   render/     — Render passes, world renderer, GPU utilities
#   engine/     — Application framework, console, splash screen
#   imgui/      — Dear ImGui integration
#   audio/      — Audio engine
#
# NVRHI lives under include/backend/rhi/ and src/backend/rhi/ and is built
# separately via its own CMakeLists.txt.
# ============================================================================

# ── Gather all engine sources ─────────────────────────────────────────────
# Use GLOB_RECURSE for modules with subdirectories that are ALWAYS compiled.
# engine/ subdirs (aftermath, streamline) are conditional — GLOB non-recursive
# and add them explicitly below.

file(GLOB_RECURSE caustica_headers
    include/core/*.h
    include/math/*.h
    include/events/*.h
    include/platform/*.h
    include/scene/*.h
    include/animation/*.h
    include/assets/*.h
    include/render/*.h
    include/imgui/*.h
    include/audio/*.h
)
# engine headers are flat (no conditional subdirs in include/)
file(GLOB caustica_engine_headers include/engine/*.h)
list(APPEND caustica_headers ${caustica_engine_headers})

file(GLOB_RECURSE caustica_sources
    src/core/*.cpp
    src/math/*.cpp
    src/platform/*.cpp
    src/scene/*.cpp
    src/animation/*.cpp
    src/assets/*.cpp
    src/render/*.cpp
    src/render/*.c
    src/imgui/*.cpp
    src/audio/*.cpp
)
# engine sources: flat .cpp/.c only (aftermath/streamline are conditional)
file(GLOB caustica_engine_flat_src src/engine/*.cpp src/engine/*.c)
list(APPEND caustica_sources ${caustica_engine_flat_src})

# Backend wrappers (not NVRHI itself — that lives under src/backend/rhi/)
file(GLOB caustica_backend_headers
    include/backend/*.h
)
list(FILTER caustica_backend_headers EXCLUDE REGEX ".*/backend/rhi/")

file(GLOB caustica_backend_sources
    src/backend/*.cpp
)
list(FILTER caustica_backend_sources EXCLUDE REGEX ".*/backend/rhi/")

# Conditional: DX11 backend
if (CAUSTICA_WITH_DX11)
    file(GLOB caustica_backend_dx11
        include/backend/dx11/*.h
        src/backend/dx11/*.cpp
    )
    list(APPEND caustica_backend_headers ${caustica_backend_dx11})
    list(APPEND caustica_backend_sources ${caustica_backend_dx11})
endif()

# Conditional: DX12 backend
if (CAUSTICA_WITH_DX12)
    file(GLOB caustica_backend_dx12
        include/backend/dx12/*.h
        src/backend/dx12/*.cpp
    )
    list(APPEND caustica_backend_headers ${caustica_backend_dx12})
    list(APPEND caustica_backend_sources ${caustica_backend_dx12})
endif()

# Conditional: Vulkan backend
if (CAUSTICA_WITH_VULKAN)
    file(GLOB caustica_backend_vulkan
        include/backend/vulkan/*.h
        src/backend/vulkan/*.cpp
    )
    list(APPEND caustica_backend_headers ${caustica_backend_vulkan})
    list(APPEND caustica_backend_sources ${caustica_backend_vulkan})
endif()

# Conditional: Aftermath crash dump
if (CAUSTICA_WITH_AFTERMATH)
    list(APPEND caustica_sources src/engine/aftermath/AftermathCrashDump.cpp)
endif()

# Conditional: Streamline integration
if (CAUSTICA_WITH_STREAMLINE)
    list(APPEND caustica_sources
        src/engine/streamline/StreamlineIntegration.cpp
        src/engine/streamline/StreamlineIntegration.h
    )
endif()

# Platform-specific exclusions
if (NOT WIN32)
    list(FILTER caustica_headers EXCLUDE REGEX ".*/WinResFS\\.h$")
    list(FILTER caustica_sources EXCLUDE REGEX ".*/WinResFS\\.cpp$")
endif()

# MSVC natvis
if (MSVC)
    list(APPEND caustica_sources caustica.natvis)
endif()

# ── Unified engine library ────────────────────────────────────────────────

add_library(causticaLib STATIC
    ${caustica_headers}
    ${caustica_sources}
    ${caustica_backend_headers}
    ${caustica_backend_sources}
)

target_include_directories(causticaLib
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE
        "$<$<BOOL:${CAUSTICA_WITH_STREAMLINE}>:${CMAKE_CURRENT_SOURCE_DIR}/src/engine/streamline>"
        "${CMAKE_SOURCE_DIR}/External/Nrd/Include"
        "${CMAKE_SOURCE_DIR}/External/Rtxdi/Include"
        "${CMAKE_SOURCE_DIR}/External/Omm/libraries/omm-gpu-nvrhi"
        "${CMAKE_SOURCE_DIR}/External/Omm/libraries/omm-lib/include"
)

target_link_libraries(causticaLib
    PUBLIC
        jsoncpp_static
        cxxopts
        nvrhi
        stb
        cgltf
        ShaderMakeBlob
        glfw
        imgui
        NRD
        Rtxdi
        omm-lib
        omm-gpu-nvrhi
)

# ── Compile options ────────────────────────────────────────────────────────

if (MSVC)
    target_compile_options(causticaLib PRIVATE /utf-8)
endif()

if (WIN32)
    target_compile_definitions(causticaLib PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(causticaLib PRIVATE CAUSTICA_FORCE_DISCRETE_GPU=$<BOOL:${CAUSTICA_FORCE_DISCRETE_GPU}>)
endif()

# ── Compile definitions (propagated to editor) ─────────────────────────────

target_compile_definitions(causticaLib
    PUBLIC
        CAUSTICA_WITH_DX11=$<BOOL:${CAUSTICA_WITH_DX11}>
        CAUSTICA_WITH_DX12=$<BOOL:${CAUSTICA_WITH_DX12}>
        CAUSTICA_WITH_VULKAN=$<BOOL:${CAUSTICA_WITH_VULKAN}>
        CAUSTICA_WITH_STATIC_SHADERS=$<BOOL:${CAUSTICA_WITH_STATIC_SHADERS}>
        CAUSTICA_WITH_AFTERMATH=$<BOOL:${CAUSTICA_WITH_AFTERMATH}>
        CAUSTICA_WITH_STREAMLINE=$<BOOL:${CAUSTICA_WITH_STREAMLINE}>
        CAUSTICA_WITH_DLSS=$<BOOL:${CAUSTICA_WITH_DLSS}>
)

# ── Optional dependencies ──────────────────────────────────────────────────

if (CAUSTICA_WITH_AUDIO)
    target_link_libraries(causticaLib PUBLIC Xaudio2)
    target_compile_definitions(causticaLib PUBLIC CAUSTICA_WITH_AUDIO)
endif()

if (CAUSTICA_WITH_TINYEXR)
    target_link_libraries(causticaLib PUBLIC tinyexr)
    target_compile_definitions(causticaLib PUBLIC CAUSTICA_WITH_TINYEXR)
endif()

if (CAUSTICA_WITH_OIDN)
    target_link_libraries(causticaLib PUBLIC OpenImageDenoise)
endif()

if (CAUSTICA_WITH_DLSS AND TARGET DLSS)
    target_link_libraries(causticaLib PUBLIC DLSS)
endif()

if (CAUSTICA_WITH_STATIC_SHADERS)
    target_include_directories(causticaLib PRIVATE "${CAUSTICA_ENGINE_STATIC_SHADERS_DIR}")
endif()

if (CAUSTICA_WITH_STREAMLINE)
    target_link_libraries(causticaLib PUBLIC streamline)
    if (CAUSTICA_WITH_VULKAN AND TARGET glfw)
        target_compile_definitions(glfw PRIVATE _GLFW_VULKAN_LIBRARY="sl.interposer.dll")
    endif()
    foreach(_feat IN ITEMS DEEPDVC DIRECTSR DLSS_SR DLSS_FG DLSS_RR IMGUI NIS NVPERF REFLEX)
        if (STREAMLINE_FEATURE_${_feat})
            target_compile_definitions(causticaLib PRIVATE STREAMLINE_FEATURE_${_feat})
        endif()
    endforeach()
endif()

# ── Backend-specific dependencies ──────────────────────────────────────────

if (NOT WIN32)
    target_link_libraries(causticaLib PUBLIC stdc++fs dl pthread)
endif()

if (CAUSTICA_WITH_DX11)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causticaLib PUBLIC nvrhi_d3d11)
    endif()
    target_link_libraries(causticaLib PUBLIC d3d11 dxgi dxguid)
endif()

if (CAUSTICA_WITH_DX12)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causticaLib PUBLIC nvrhi_d3d12)
    endif()
    target_link_libraries(causticaLib PUBLIC d3d12 dxgi dxguid)
endif()

if (CAUSTICA_WITH_VULKAN)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causticaLib PUBLIC nvrhi_vk)
    endif()
    if (TARGET Vulkan-Headers)
        target_link_libraries(causticaLib PUBLIC Vulkan-Headers)
    elseif (TARGET Vulkan::Headers)
        target_link_libraries(causticaLib PUBLIC Vulkan::Headers)
    endif()
endif()

# ── Shader dependencies ────────────────────────────────────────────────────

if (TARGET causticaEngineShaders)
    add_dependencies(causticaLib causticaEngineShaders)
endif()

# ── IDE folder ─────────────────────────────────────────────────────────────

set_target_properties(causticaLib PROPERTIES FOLDER "causticaLib/causticaLib")
