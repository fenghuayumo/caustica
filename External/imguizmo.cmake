#
# ImGuizmo - immediate mode 3D gizmo for scene editing (translate / rotate / scale)
#

set(imguizmo_srcs
    ${CMAKE_CURRENT_SOURCE_DIR}/ImGuizmo/src/ImGuizmo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ImGuizmo/src/ImGuizmo.h)

add_library(imguizmo STATIC ${imguizmo_srcs})
target_include_directories(imguizmo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ImGuizmo/src)
target_link_libraries(imguizmo PUBLIC imgui)
