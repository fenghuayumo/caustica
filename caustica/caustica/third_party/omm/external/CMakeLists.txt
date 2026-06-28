set(LZ4_BUILD_CLI OFF)
set(LZ4_BUILD_LEGACY_LZ4C OFF)

add_subdirectory("glm")
set_property(TARGET glm PROPERTY FOLDER "caustica/third_party")

add_subdirectory("lz4/build/cmake")
if (TARGET lz4_static)
    set_property(TARGET lz4_static PROPERTY FOLDER "caustica/third_party")
endif()
if (TARGET lz4_static AND NOT TARGET lz4)
    add_library(lz4 ALIAS lz4_static)
endif()

set(XXHASH_BUILD_XXHSUM OFF)
add_subdirectory("xxHash/build/cmake")
if (TARGET xxhash)
    set_property(TARGET xxhash PROPERTY FOLDER "caustica/third_party")
endif()

if (NOT TARGET stb_lib)
    add_library(stb_lib INTERFACE)
    target_include_directories(stb_lib INTERFACE $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/External/stb>)
endif()
