# Editor / sample application sources (UI, game logic, path tracer app)

file(GLOB_RECURSE caustica_editor_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/render/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/render/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/SampleCommon/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SampleCommon/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/SampleGame/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SampleGame/*.h"
)

# Entry point is compiled into the executable, not causticaApp
set(caustica_editor_entry_source "${CMAKE_CURRENT_SOURCE_DIR}/app/Main.cpp")
list(REMOVE_ITEM caustica_editor_sources ${caustica_editor_entry_source})

if (NOT WIN32)
    list(REMOVE_ITEM caustica_editor_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/SampleCommon/SplashScreen.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/SampleCommon/SplashScreen.h"
    )
endif()

set(CAUSTICA_EDITOR_SOURCES ${caustica_editor_sources} PARENT_SCOPE)
set(CAUSTICA_EDITOR_ENTRY_SOURCE ${caustica_editor_entry_source} PARENT_SCOPE)
