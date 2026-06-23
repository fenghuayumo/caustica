# =============================================================================
# 4-Layer Architecture: Platform + Core + Engine
# One include/ dir + one src/ dir, each organized by layer.
# =============================================================================

# --- Sources by layer ---
set(SOURCES_PLATFORM
    src/platform/os.cpp
    src/platform/window.cpp
    src/platform/file_system.cpp
    src/platform/timer.cpp
    src/platform/glfw/glfw_window.cpp
)
if (WIN32)
    list(APPEND SOURCES_PLATFORM src/platform/windows/windows_os.cpp)
endif()
if (UNIX AND NOT APPLE)
    list(APPEND SOURCES_PLATFORM src/platform/unix/unix_os.cpp)
endif()
if (APPLE)
    list(APPEND SOURCES_PLATFORM src/platform/macos/mac_os.cpp)
endif()

set(SOURCES_CORE
    src/core/command_line.cpp
    src/core/core_system.cpp
    src/core/job_system.cpp
    src/core/memory_manager.cpp
)

set(SOURCES_ENGINE
    src/engine/time_step.cpp
    src/engine/engine.cpp
    src/engine/application.cpp
    src/engine/input.cpp
    src/engine/scene_manager.cpp
)

set(SOURCES_RENDER
    src/render/renderer.cpp
)

# --- Header groups for IDE display ---
file(GLOB_RECURSE HEADERS_PLATFORM include/platform/*.h)
file(GLOB_RECURSE HEADERS_CORE     include/core/*.h)
file(GLOB_RECURSE HEADERS_ENGINE   include/engine/*.h)
file(GLOB_RECURSE HEADERS_RENDER   include/render/*.h)

# =============================================================================
# Layer 1: Platform
# =============================================================================
add_library(causticaPlatform STATIC
    ${SOURCES_PLATFORM} ${HEADERS_PLATFORM}
)
target_include_directories(causticaPlatform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(causticaPlatform PUBLIC glfw)
if (WIN32)
    target_compile_definitions(causticaPlatform PRIVATE NOMINMAX)
    target_link_libraries(causticaPlatform PRIVATE dwmapi)
    if (MSVC)
        target_compile_options(causticaPlatform PRIVATE /utf-8)
    endif()
endif()
set_target_properties(causticaPlatform PROPERTIES FOLDER "caustica/source")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${SOURCES_PLATFORM} ${HEADERS_PLATFORM})

# =============================================================================
# Layer 2: Core System
# =============================================================================
add_library(causticaCoreSystem STATIC
    ${SOURCES_CORE} ${HEADERS_CORE}
)
target_include_directories(causticaCoreSystem PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(causticaCoreSystem PUBLIC causticaPlatform)
if (WIN32)
    target_compile_definitions(causticaCoreSystem PRIVATE NOMINMAX)
    if (MSVC)
        target_compile_options(causticaCoreSystem PRIVATE /utf-8)
    endif()
endif()
set_target_properties(causticaCoreSystem PROPERTIES FOLDER "caustica/source")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${SOURCES_CORE} ${HEADERS_CORE})

# =============================================================================
# Layer 3: Engine
# =============================================================================
add_library(causticaEngine2 STATIC
    ${SOURCES_ENGINE} ${HEADERS_ENGINE}
)
target_include_directories(causticaEngine2
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../include"   # existing caustica engine headers
)
target_link_libraries(causticaEngine2
    PUBLIC
        causticaCoreSystem
        causticaPlatform
        causticaEngine
)
if (WIN32)
    target_compile_definitions(causticaEngine2 PRIVATE NOMINMAX)
    if (MSVC)
        target_compile_options(causticaEngine2 PRIVATE /utf-8)
    endif()
endif()
set_target_properties(causticaEngine2 PROPERTIES FOLDER "caustica/source")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${SOURCES_ENGINE} ${HEADERS_ENGINE})

# =============================================================================
# Layer 4: Renderer
# =============================================================================
add_library(causticaRender STATIC
    ${SOURCES_RENDER} ${HEADERS_RENDER}
)
target_include_directories(causticaRender
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../include"   # existing caustica engine headers
        "${CMAKE_CURRENT_SOURCE_DIR}/../../include/engine"  # existing rhi/nvrhi headers path
)
target_link_libraries(causticaRender
    PUBLIC
        causticaEngine2
        causticaEngine
        nvrhi
)
if (WIN32)
    target_compile_definitions(causticaRender PRIVATE NOMINMAX)
    if (MSVC)
        target_compile_options(causticaRender PRIVATE /utf-8)
    endif()
endif()
set_target_properties(causticaRender PROPERTIES FOLDER "caustica/source")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${SOURCES_RENDER} ${HEADERS_RENDER})
