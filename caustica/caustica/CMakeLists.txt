# ---------------------------------------------------------------------------

file(GLOB caustica_engine_core_src
    include/core/chunk/*.h
    include/core/vfs/TarFile.h
    include/core/vfs/VFS.h
    include/core/*.h
    src/core/chunk/*.cpp
    src/core/vfs/TarFile.cpp
    src/core/vfs/VFS.cpp
    src/core/*.cpp
)

file(GLOB caustica_engine_math_src
    include/math/*.h
    src/math/*.cpp
)

file(GLOB caustica_engine_engine_src
    include/engine/*.h
    src/engine/*.cpp
    src/engine/*.c
    src/engine/*.h
)

# Asset layer: loading, caching (DIVSHOT-style 4-layer architecture)
file(GLOB caustica_engine_assets_src
    include/assets/loader/*.h
    src/assets/loader/*.cpp
    include/assets/cache/*.h
    src/assets/cache/*.cpp
)

file(GLOB caustica_engine_render_src
    LIST_DIRECTORIES false
    include/render/*.h
    src/render/*.cpp
)

list(FILTER caustica_engine_render_src EXCLUDE REGEX ".*/ToneMappingPasses\\.(h|cpp)$")

if (WIN32)
    list(APPEND caustica_engine_core_src
        include/core/vfs/WinResFS.h
        src/core/vfs/WinResFS.cpp
    )
endif()

if (DONUT_WITH_STREAMLINE)
    list(APPEND caustica_engine_engine_src
        src/engine/streamline/StreamlineIntegration.cpp
        src/engine/streamline/StreamlineIntegration.h
    )
endif()

if (DONUT_WITH_DX11)
    list(APPEND caustica_engine_engine_src
        include/backend/dx11/GpuDevice_DX11.h
        src/backend/dx11/GpuDevice_DX11.cpp)
endif()

if (DONUT_WITH_DX12)
    list(APPEND caustica_engine_engine_src
        include/backend/dx12/GpuDevice_DX12.h
        src/backend/dx12/GpuDevice_DX12.cpp)
endif()

if (DONUT_WITH_VULKAN)
    list(APPEND caustica_engine_engine_src
        include/backend/vulkan/GpuDevice_VK.h
        src/backend/vulkan/GpuDevice_VK.cpp)
endif()

if (DONUT_WITH_AFTERMATH)
    list(APPEND caustica_engine_engine_src src/engine/aftermath/AftermathCrashDump.cpp)
endif()

if (MSVC)
    list(APPEND caustica_engine_core_src donut.natvis)
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

# Engine layer: IRenderPass (render pass interface, extracted from GpuDevice)
list(APPEND caustica_engine_engine_src
    include/engine/IRenderPass.h
    src/engine/IRenderPass.cpp
)

# Renderer layer: pass management
list(APPEND caustica_engine_engine_src
    include/render/RenderPassManager.h
    src/render/RenderPassManager.cpp
)

add_library(causticaEngine STATIC
    ${caustica_engine_core_src}
    ${caustica_engine_math_src}
    ${caustica_engine_engine_src}
    ${caustica_engine_assets_src}
    ${caustica_engine_render_src}
)

target_include_directories(causticaEngine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "$<$<BOOL:${DONUT_WITH_STREAMLINE}>:${CMAKE_CURRENT_SOURCE_DIR}/src/engine/streamline>"
)

target_link_libraries(causticaEngine
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
    target_link_libraries(causticaEngine PUBLIC stdc++fs dl pthread)
endif()

if (WIN32)
    target_compile_definitions(causticaEngine PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(causticaEngine PRIVATE DONUT_FORCE_DISCRETE_GPU=$<BOOL:${DONUT_FORCE_DISCRETE_GPU}>)
endif()
if (MSVC)
    target_compile_options(causticaEngine PRIVATE /utf-8)
endif()

target_compile_definitions(causticaEngine
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
    target_link_libraries(causticaEngine PUBLIC Xaudio2)
    target_compile_definitions(causticaEngine PUBLIC DONUT_WITH_AUDIO)
endif()

if (DONUT_WITH_TINYEXR)
    target_link_libraries(causticaEngine PUBLIC tinyexr)
    target_compile_definitions(causticaEngine PUBLIC DONUT_WITH_TINYEXR)
endif()

if (DONUT_WITH_STREAMLINE)
    target_link_libraries(causticaEngine PUBLIC streamline)

    if (DONUT_WITH_VULKAN AND TARGET glfw)
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

if (DONUT_WITH_DX11)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causticaEngine PUBLIC nvrhi_d3d11)
    endif()
    target_link_libraries(causticaEngine PUBLIC d3d11 dxgi dxguid)
endif()

if (DONUT_WITH_DX12)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causticaEngine PUBLIC nvrhi_d3d12)
    endif()
    target_link_libraries(causticaEngine PUBLIC d3d12 dxgi dxguid)
endif()

if (DONUT_WITH_VULKAN)
    if (NOT NVRHI_BUILD_SHARED)
        target_link_libraries(causticaEngine PUBLIC nvrhi_vk)
    endif()

    if (TARGET Vulkan-Headers)
        target_link_libraries(causticaEngine PUBLIC Vulkan-Headers)
    elseif (TARGET Vulkan::Headers)
        target_link_libraries(causticaEngine PUBLIC Vulkan::Headers)
    endif()
endif()

if (DONUT_WITH_DLSS AND TARGET DLSS)
    target_link_libraries(causticaEngine PUBLIC DLSS)
endif()

if (DONUT_WITH_STATIC_SHADERS)
    target_include_directories(causticaEngine PRIVATE "${CAUSTICA_ENGINE_STATIC_SHADERS_DIR}")
endif()

if (TARGET causticaEngineShaders)
    add_dependencies(causticaEngine causticaEngineShaders)
endif()

set_target_properties(causticaEngine PROPERTIES FOLDER "caustica/caustica")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    ${caustica_engine_core_src}
    ${caustica_engine_math_src}
    ${caustica_engine_engine_src}
    ${caustica_engine_render_src}
)
