# ============================================================================
# caustica Engine — Modular library targets
# ============================================================================
# Each engine module (math, core, events, platform, backend, scene,
# animation, assets, render, engine, imgui, audio) is now a separate
# static library with explicit link dependencies.
#
# All module targets use the `caus` prefix to avoid name collisions with
# the existing `causticaApp` application library and `caustica` executable.
#
# Legacy: `causticaLib` is preserved as an INTERFACE library that
# aggregates all modules for backward compatibility.
#
# Dependency graph (top-down):
#   causEngine → causRender → causScene → causAssets
#              → causBackend → causPlatform → causEvents
#              → causCore → causMath
#   causImgui   → causCore, causBackend
#   causAnimation → causMath
#   causAudio   → causMath
#
# Directory layout (under include/ and src/):
#   math/       — Math library (vector, matrix, quaternion, bounding volumes)
#   core/       — Foundation utilities (logging, threading, VFS, JSON, etc.)
#   events/     — Event system (key, mouse, application events)
#   platform/   — OS abstraction (window, GLFW, OS)
#   backend/    — GPU device wrappers (DX11, DX12, Vulkan)
#   scene/      — Scene graph, camera, game model types
#   animation/  — Keyframe animation
#   assets/     — Asset system (loaders, cache, registry)
#   render/     — Render passes, world renderer, GPU utilities
#   engine/     — Application framework, console, splash screen
#   imgui/      — Dear ImGui integration (nvrhi backend)
#   audio/      — Audio engine
# ============================================================================

# ═══════════════════════════════════════════════════════════════════════════
# Common configuration
# ═══════════════════════════════════════════════════════════════════════════

set(CAUSTICA_COMMON_INCLUDE_PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

set(CAUSTICA_COMMON_INCLUDE_PRIVATE
    "${CMAKE_SOURCE_DIR}/External/Nrd/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rtxdi/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/omm/libraries/omm-gpu-nvrhi"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/omm/libraries/omm-lib/include"
)

set(CAUSTICA_COMMON_COMPILE_DEFS
    CAUSTICA_WITH_DX11=$<BOOL:${CAUSTICA_WITH_DX11}>
    CAUSTICA_WITH_DX12=$<BOOL:${CAUSTICA_WITH_DX12}>
    CAUSTICA_WITH_VULKAN=$<BOOL:${CAUSTICA_WITH_VULKAN}>
    CAUSTICA_WITH_STATIC_SHADERS=$<BOOL:${CAUSTICA_WITH_STATIC_SHADERS}>
    CAUSTICA_WITH_AFTERMATH=$<BOOL:${CAUSTICA_WITH_AFTERMATH}>
    CAUSTICA_WITH_STREAMLINE=$<BOOL:${CAUSTICA_WITH_STREAMLINE}>
    CAUSTICA_WITH_DLSS=$<BOOL:${CAUSTICA_WITH_DLSS}>
)

function(caus_configure_static TARGET_NAME)
    target_include_directories(${TARGET_NAME}
        PUBLIC  ${CAUSTICA_COMMON_INCLUDE_PUBLIC}
        PRIVATE ${CAUSTICA_COMMON_INCLUDE_PRIVATE}
    )
    target_compile_definitions(${TARGET_NAME} PUBLIC ${CAUSTICA_COMMON_COMPILE_DEFS})
    if (MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /utf-8)
        target_compile_definitions(${TARGET_NAME} PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
        if (WIN32)
            target_compile_definitions(${TARGET_NAME} PRIVATE CAUSTICA_FORCE_DISCRETE_GPU=$<BOOL:${CAUSTICA_FORCE_DISCRETE_GPU}>)
        endif()
    endif()
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "causticaLib/modules")
endfunction()

function(caus_configure_interface TARGET_NAME)
    target_include_directories(${TARGET_NAME}
        INTERFACE ${CAUSTICA_COMMON_INCLUDE_PUBLIC}
        INTERFACE ${CAUSTICA_COMMON_INCLUDE_PRIVATE}
    )
    target_compile_definitions(${TARGET_NAME} INTERFACE ${CAUSTICA_COMMON_COMPILE_DEFS})
    if (MSVC)
        target_compile_definitions(${TARGET_NAME} INTERFACE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    endif()
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "causticaLib/modules")
endfunction()

# ═══════════════════════════════════════════════════════════════════════════
# Third-party render libraries (RTXDI, OMM — vendored in-engine)
# ═══════════════════════════════════════════════════════════════════════════

add_subdirectory(third_party)

# ═══════════════════════════════════════════════════════════════════════════
# FOUNDATION LAYER — no dependencies on other caustica modules
# ═══════════════════════════════════════════════════════════════════════════

# ── causMath ──────────────────────────────────────────────────────────────
# Pure math library. Only depends on STL.

file(GLOB caus_math_headers include/math/*.h)
file(GLOB caus_math_sources src/math/*.cpp)

add_library(causMath STATIC ${caus_math_headers} ${caus_math_sources})
caus_configure_static(causMath)

# ── causCore ──────────────────────────────────────────────────────────────
# Foundation utilities: logging, threading (JobSystem/ThreadPool), VFS,
# JSON, Timer, UUID, command line, format, string/path/file utils, chunk I/O.
# Depends on math for serialization and formatting helpers.

file(GLOB caus_core_headers include/core/*.h)
file(GLOB caus_core_sources src/core/*.cpp)
file(GLOB_RECURSE caus_core_sub_headers
    include/core/chunk/*.h
    include/core/vfs/*.h
    include/core/console/*.h
)
file(GLOB_RECURSE caus_core_sub_sources
    src/core/chunk/*.cpp
    src/core/vfs/*.cpp
    src/core/console/*.cpp
)
list(APPEND caus_core_headers ${caus_core_sub_headers})
list(APPEND caus_core_sources ${caus_core_sub_sources})

if (NOT WIN32)
    list(FILTER caus_core_headers EXCLUDE REGEX ".*/WinResFS\\.h$")
    list(FILTER caus_core_sources EXCLUDE REGEX ".*/WinResFS\\.cpp$")
endif()

add_library(causCore STATIC ${caus_core_headers} ${caus_core_sources})
caus_configure_static(causCore)
target_link_libraries(causCore
    PUBLIC causMath
    PUBLIC jsoncpp_static cxxopts
)

# ── causEvents ────────────────────────────────────────────────────────────
# Event system (Event, EventDispatcher, key/mouse/application events).
# Header-only. Depends on causCore for key_codes types.

file(GLOB caus_events_headers include/events/*.h)

add_library(causEvents INTERFACE ${caus_events_headers})
caus_configure_interface(causEvents)
target_link_libraries(causEvents INTERFACE causCore)

# ── causAnimation ─────────────────────────────────────────────────────────
# Keyframe animation. Depends only on math.

file(GLOB caus_animation_headers include/animation/*.h)
file(GLOB caus_animation_sources src/animation/*.cpp)

add_library(causAnimation STATIC ${caus_animation_headers} ${caus_animation_sources})
caus_configure_static(causAnimation)
target_link_libraries(causAnimation PUBLIC causCore causMath)

file(GLOB_RECURSE caus_ecs_headers include/ecs/*.h)

add_library(causEcs INTERFACE ${caus_ecs_headers})
caus_configure_interface(causEcs)
target_link_libraries(causEcs INTERFACE causCore causMath EnTT::EnTT)

# ── causAudio ─────────────────────────────────────────────────────────────
# Audio engine. Depends only on math. Optional: XAudio2 on Windows.

file(GLOB caus_audio_headers include/audio/*.h)
file(GLOB caus_audio_sources src/audio/*.cpp)

add_library(causAudio STATIC ${caus_audio_headers} ${caus_audio_sources})
caus_configure_static(causAudio)
target_link_libraries(causAudio PUBLIC causCore causMath)
if (CAUSTICA_WITH_AUDIO)
    target_link_libraries(causAudio PUBLIC Xaudio2)
    target_compile_definitions(causAudio PUBLIC CAUSTICA_WITH_AUDIO)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# SERVICE LAYER — depend on Foundation modules + external libs
# ═══════════════════════════════════════════════════════════════════════════

# ── causPlatform ──────────────────────────────────────────────────────────
# OS abstraction: Window, GlfwWindow, OS utilities.

file(GLOB_RECURSE caus_platform_headers include/platform/*.h)
file(GLOB_RECURSE caus_platform_sources src/platform/*.cpp)

add_library(causPlatform STATIC ${caus_platform_headers} ${caus_platform_sources})
caus_configure_static(causPlatform)
target_link_libraries(causPlatform
    PUBLIC causCore causEvents
    PUBLIC glfw
)

# ── causBackend ───────────────────────────────────────────────────────────
# GPU device wrappers (DX11/DX12/Vulkan) + nvrhi integration.
# Does NOT include the nvrhi RHI implementation itself (separate CMake build).

# Core backend files
file(GLOB caus_backend_headers_base include/backend/*.h)
file(GLOB caus_backend_sources_base src/backend/*.cpp)
# Exclude nvrhi implementation (built separately)
list(FILTER caus_backend_headers_base EXCLUDE REGEX ".*/backend/rhi/")
list(FILTER caus_backend_sources_base EXCLUDE REGEX ".*/backend/rhi/")

set(caus_backend_headers ${caus_backend_headers_base})
set(caus_backend_sources ${caus_backend_sources_base})

# Conditional: DX11
if (CAUSTICA_WITH_DX11)
    file(GLOB dx11_h include/backend/dx11/*.h)
    file(GLOB dx11_cpp src/backend/dx11/*.cpp)
    list(APPEND caus_backend_headers ${dx11_h})
    list(APPEND caus_backend_sources ${dx11_cpp})
endif()

# Conditional: DX12
if (CAUSTICA_WITH_DX12)
    file(GLOB dx12_h include/backend/dx12/*.h)
    file(GLOB dx12_cpp src/backend/dx12/*.cpp)
    list(APPEND caus_backend_headers ${dx12_h})
    list(APPEND caus_backend_sources ${dx12_cpp})
endif()

# Conditional: Vulkan
if (CAUSTICA_WITH_VULKAN)
    file(GLOB vk_h include/backend/vulkan/*.h)
    file(GLOB vk_cpp src/backend/vulkan/*.cpp)
    list(APPEND caus_backend_headers ${vk_h})
    list(APPEND caus_backend_sources ${vk_cpp})
endif()

# MSVC debug visualizer
if (MSVC)
    list(APPEND caus_backend_sources caustica.natvis)
endif()

add_library(causBackend STATIC ${caus_backend_headers} ${caus_backend_sources})
caus_configure_static(causBackend)
target_link_libraries(causBackend
    PUBLIC causCore causMath causPlatform
    PUBLIC nvrhi
)
# Backend-specific link deps
if (CAUSTICA_WITH_DX11)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causBackend PUBLIC nvrhi_d3d11)
    endif()
    target_link_libraries(causBackend PUBLIC d3d11 dxgi dxguid)
endif()
if (CAUSTICA_WITH_DX12)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causBackend PUBLIC nvrhi_d3d12)
    endif()
    target_link_libraries(causBackend PUBLIC d3d12 dxgi dxguid)
    if (CAUSTICA_D3D_AGILITY_SDK_PATH AND CAUSTICA_D3D_AGILITY_SDK_VERSION)
        target_compile_definitions(causBackend PUBLIC CAUSTICA_D3D_AGILITY_SDK_VERSION=${CAUSTICA_D3D_AGILITY_SDK_VERSION})
        target_include_directories(causBackend PRIVATE ${CAUSTICA_D3D_AGILITY_SDK_PATH}/build/native/include)
    endif()
endif()
if (CAUSTICA_WITH_VULKAN)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causBackend PUBLIC nvrhi_vk)
    endif()
    if (TARGET Vulkan-Headers)
        target_link_libraries(causBackend PUBLIC Vulkan-Headers)
    elseif (TARGET Vulkan::Headers)
        target_link_libraries(causBackend PUBLIC Vulkan::Headers)
    endif()
endif()
if (CAUSTICA_WITH_STREAMLINE)
    target_link_libraries(causBackend PUBLIC streamline)
    target_include_directories(causBackend PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/streamline"
    )
endif()

# ── causImgui ─────────────────────────────────────────────────────────────
# Dear ImGui integration (nvrhi backend, console, renderer).

file(GLOB caus_imgui_headers include/imgui/*.h)
file(GLOB caus_imgui_sources src/imgui/*.cpp)

add_library(causImgui STATIC ${caus_imgui_headers} ${caus_imgui_sources})
caus_configure_static(causImgui)
target_link_libraries(causImgui
    PUBLIC causCore causBackend causAssets
    PUBLIC imgui
)

# ═══════════════════════════════════════════════════════════════════════════
# FEATURE LAYER — depend on Service + Foundation modules
# ═══════════════════════════════════════════════════════════════════════════

# ── causAssets ────────────────────────────────────────────────────────────
# Asset system: registry, cache, file watcher, loaders (glTF, OBJ, DDS, IES,
# texture, shader compiler).

file(GLOB caus_assets_headers include/assets/*.h)
file(GLOB caus_assets_sources src/assets/*.cpp)
file(GLOB_RECURSE caus_assets_loader_headers include/assets/loader/*.h)
file(GLOB_RECURSE caus_assets_loader_sources src/assets/loader/*.cpp)
list(APPEND caus_assets_headers ${caus_assets_loader_headers})
list(APPEND caus_assets_sources ${caus_assets_loader_sources})

add_library(causAssets STATIC ${caus_assets_headers} ${caus_assets_sources})
caus_configure_static(causAssets)
target_link_libraries(causAssets
    PUBLIC causCore causBackend
    PUBLIC jsoncpp_static stb cgltf ShaderMakeBlob
)

# ── causScene ─────────────────────────────────────────────────────────────
# Scene graph, scene management, camera.

file(GLOB caus_scene_headers include/scene/*.h)
file(GLOB caus_scene_sources src/scene/*.cpp)
file(GLOB_RECURSE caus_scene_sub_headers
    include/scene/camera/*.h
    include/scene/loader/*.h
)
file(GLOB_RECURSE caus_scene_sub_sources
    src/scene/camera/*.cpp
    src/scene/loader/*.cpp
)
list(APPEND caus_scene_headers ${caus_scene_sub_headers})
list(APPEND caus_scene_sources ${caus_scene_sub_sources})

add_library(causScene STATIC ${caus_scene_headers} ${caus_scene_sources})
caus_configure_static(causScene)
target_link_libraries(causScene
    PUBLIC causCore causMath causAnimation causEcs causAssets causBackend
)

# ── causRender ────────────────────────────────────────────────────────────
# Render passes, world renderer, GPU utilities, path tracer, post-processing,
# denoising (NRD / OIDN), RTXDI, Gaussian splatting, GPU sort, debugging.

file(GLOB_RECURSE caus_render_headers include/render/*.h)
file(GLOB_RECURSE caus_render_sources src/render/*.cpp src/render/*.c)

add_library(causRender STATIC ${caus_render_headers} ${caus_render_sources})
caus_configure_static(causRender)
target_link_libraries(causRender
    PUBLIC causScene causBackend causImgui causAssets
    PUBLIC jsoncpp_static NRD Rtxdi omm-lib omm-gpu-nvrhi
)
# Optional render dependencies
if (CAUSTICA_WITH_OIDN)
    target_link_libraries(causRender PUBLIC OpenImageDenoise)
endif()
if (CAUSTICA_WITH_DLSS AND TARGET DLSS)
    target_link_libraries(causRender PUBLIC DLSS)
endif()
if (CAUSTICA_WITH_TINYEXR)
    target_link_libraries(causRender PUBLIC tinyexr)
    target_compile_definitions(causRender PUBLIC CAUSTICA_WITH_TINYEXR)
endif()

# ── causEngine ────────────────────────────────────────────────────────────
# Application framework, console, splash screen, Aftermath, Streamline.

file(GLOB caus_engine_headers include/engine/*.h)
file(GLOB caus_engine_sources src/engine/*.cpp src/engine/*.c)

# Conditional: Aftermath GPU crash dump
if (CAUSTICA_WITH_AFTERMATH)
    list(APPEND caus_engine_sources src/engine/aftermath/AftermathCrashDump.cpp)
endif()

# Conditional: Streamline (DLSS / Reflex / DLSS-FG / NIS)
if (CAUSTICA_WITH_STREAMLINE)
    list(APPEND caus_engine_sources
        src/engine/streamline/StreamlineIntegration.cpp
        src/engine/streamline/StreamlineIntegration.h
    )
endif()

add_library(causEngine STATIC ${caus_engine_headers} ${caus_engine_sources})
caus_configure_static(causEngine)
target_link_libraries(causEngine
    PUBLIC causEvents causPlatform causBackend causRender causImgui
    PUBLIC cxxopts
)
# Streamline private include path
if (CAUSTICA_WITH_STREAMLINE)
    target_include_directories(causEngine PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/streamline"
    )
endif()
# Streamline integration
if (CAUSTICA_WITH_STREAMLINE)
    target_link_libraries(causEngine PUBLIC streamline)
    if (CAUSTICA_WITH_VULKAN AND TARGET glfw)
        target_compile_definitions(glfw PRIVATE _GLFW_VULKAN_LIBRARY="sl.interposer.dll")
    endif()
    foreach(_feat IN ITEMS DEEPDVC DIRECTSR DLSS_SR DLSS_FG DLSS_RR IMGUI NIS NVPERF REFLEX)
        if (STREAMLINE_FEATURE_${_feat})
            target_compile_definitions(causEngine PRIVATE STREAMLINE_FEATURE_${_feat})
        endif()
    endforeach()
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Aggregate target — backward compatibility
# ═══════════════════════════════════════════════════════════════════════════
# causticaLib is an INTERFACE library that re-exports all modules.
# External targets linking causticaLib will transitively link everything.
# This preserves backward compatibility with the parent CMakeLists.txt
# which does:  target_link_libraries(causticaApp PUBLIC causticaLib)

add_library(causticaLib INTERFACE)
target_link_libraries(causticaLib INTERFACE
    causMath
    causCore
    causEvents
    causAnimation
    causEcs
    causAudio
    causPlatform
    causBackend
    causImgui
    causAssets
    causScene
    causRender
    causEngine
)
caus_configure_interface(causticaLib)

if (NOT WIN32)
    target_link_libraries(causticaLib INTERFACE stdc++fs dl pthread)
endif()

set_target_properties(causticaLib PROPERTIES FOLDER "causticaLib")

# ═══════════════════════════════════════════════════════════════════════════
# Shader dependencies
# ═══════════════════════════════════════════════════════════════════════════

if (TARGET causticaEngineShaders)
    add_dependencies(causRender causticaEngineShaders)
endif()

if (CAUSTICA_WITH_STATIC_SHADERS)
    target_include_directories(causRender PRIVATE "${CAUSTICA_ENGINE_STATIC_SHADERS_DIR}")
endif()

# OMM runtime shader compilation (shaders live under third_party/omm)
util_generate_shader_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/omm.cfg"
    "third_party/omm/libraries/omm-lib/shaders"
    "-D COMPILER_DXC=1"
)

set(omm_shader_compile_args
    TARGET omm_shaders
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/omm.cfg
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/omm/libraries/omm-lib/include
    FOLDER "caustica/third_party"
    SPIRV_DXC ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/omm/spirv
)
if (WIN32)
    list(APPEND omm_shader_compile_args
        DXIL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/omm/dxil
    )
endif()
caustica_compile_shaders(${omm_shader_compile_args})
add_dependencies(causRender omm_shaders)
