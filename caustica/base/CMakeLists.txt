# ---------------------------------------------------------------------------
# causticaBase — Foundation library (zero rendering dependencies)
#
# Contains core utilities (VFS, chunk, threading, logging, string utils)
# and math primitives (vector, matrix, quaternion, color, bounding, etc.).
# Depends only on C++20 std + jsoncpp (pure data library).
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

file(GLOB caustica_base_math_headers
    include/math/*.h
)

file(GLOB caustica_base_math_sources
    src/math/*.cpp
)

# Exclude Windows-specific VFS backend on non-Windows platforms
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

set_target_properties(causticaBase PROPERTIES FOLDER "caustica/base")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    ${caustica_base_core_headers}
    ${caustica_base_core_sources}
    ${caustica_base_math_headers}
    ${caustica_base_math_sources}
)
