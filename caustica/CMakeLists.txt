include(../External/cmake/compileshaders.cmake)
file(GLOB_RECURSE shaders "*.hlsl" "*.hlsli")
file(GLOB_RECURSE shared_sources "*.cpp" "*.h" "*.md")
list(FILTER shaders EXCLUDE REGEX "/caustica/(include|src|backend)/")
list(FILTER shared_sources EXCLUDE REGEX "/caustica/(include|src|shaders|backend)/")
list(FILTER shared_sources EXCLUDE REGEX "/application/")
list(FILTER shared_sources EXCLUDE REGEX "/Python/")
list(FILTER shared_sources EXCLUDE REGEX "/source/")

# All render module sources are now in causticaEngine (see caustica/caustica/CMakeLists.txt)

file(GLOB caustica_python_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/Python/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Python/*.h"
)

# The two NB_MODULE entry points are mutually exclusive (one defines the
# embed-mode `caustica` module that lives inside caustica.exe, the other defines
# the Python extension `caustica.pyd`).  They are added to their respective
# targets later - never to causticaApp.
set(caustica_python_core_sources ${caustica_python_sources})
list(REMOVE_ITEM caustica_python_core_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/Python/PythonBindings_Embed.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Python/PythonBindings_Extension.cpp"
)

file(GLOB NRD_INTEGRATION_HEADERS "../External/Nrd/Integration/*.h" "../External/Nrd/Integration/*.hpp")
source_group("RayTracingDenoiser" FILES ${NRD_INTEGRATION_HEADERS})

set(project caustica)
set(folder "caustica")

set(caustica_shaders caustica_shaders)

if (WIN32)
if (CAUSTICA_DISTRIBUTION_BUILD)
    set(SHADERMAKE_OPTIONS_DXIL "")
else()
    set(SHADERMAKE_OPTIONS_DXIL "-D ENABLE_DEBUG_PRINT")
endif()
#set(SHADERMAKE_OPTIONS_SPIRV "-D ENABLE_DEBUG_PRINT") # Disable SPIRV debug printf since it causes DXC to crash as of dxc 1.8.2505
# SPIR-V cannot compile DX12 OMM ray-query variants (see shaders.cfg); match Linux.
set(SHADERMAKE_OPTIONS_SPIRV "-D NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=0 -D SPIRV")
else()
# DXIL is irrelevant on Linux
set(SHADERMAKE_OPTIONS_DXIL "")
# Don't enable DebugPrint with Linux DXC because that leads to segfaults in the compiler (as of v1.8.2407)
# Disable DX12-only OMM ray-query flags for SPIR-V (see shaders.cfg OMM variants).
set(SHADERMAKE_OPTIONS_SPIRV "-D NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=0 -D SPIRV")
endif()

if(CAUSTICA_WITH_AFTERMATH OR CAUSTICA_EMBED_SHADER_PDBS)
    set(shadermake_options "--embedPDB")
#else()
#    set(shadermake_options --PDB)
endif()

#set(shadermake_options --verbose)

if(CAUSTICA_DISTRIBUTION_BUILD)
    string(APPEND shadermake_options " --stripReflection")
endif()

if(CAUSTICA_D3D_AGILITY_SDK_VERSION AND (CAUSTICA_D3D_AGILITY_SDK_VERSION GREATER_EQUAL "619"))
    string(APPEND shadermake_options " -m 6_9")
endif()

# SPIR-V builds cannot compile the DX12 OMM shader variants in shaders.cfg.
set(caustica_shader_config_dxil "${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg")
set(caustica_shader_config_vk "${CMAKE_CURRENT_SOURCE_DIR}/shaders_vk.cfg")
file(READ "${caustica_shader_config_dxil}" caustica_shader_config_vk_contents)
string(REGEX REPLACE "[^\n]*NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=1[^\n]*\n?" "" caustica_shader_config_vk_contents "${caustica_shader_config_vk_contents}")
file(WRITE "${caustica_shader_config_vk}" "${caustica_shader_config_vk_contents}")

set(caustica_shader_compile_common
    TARGET caustica_shaders
    SOURCES ${shaders}
    FOLDER ${folder}
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/caustica ${CMAKE_SOURCE_DIR}/External/Nrd/Shaders/Include ${CMAKE_CURRENT_SOURCE_DIR}/caustica/third_party ${CMAKE_CURRENT_SOURCE_DIR}/caustica/third_party/rtxdi/Include ${CMAKE_SOURCE_DIR}/External
    SHADERMAKE_OPTIONS ${shadermake_options}
)
if (WIN32)
    caustica_compile_shaders(${caustica_shader_compile_common}
        CONFIG ${caustica_shader_config_dxil}
        DXIL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/${project}/dxil
        SHADERMAKE_OPTIONS_DXIL ${SHADERMAKE_OPTIONS_DXIL}
    )
    caustica_compile_shaders(${caustica_shader_compile_common}
        CONFIG ${caustica_shader_config_vk}
        SPIRV_DXC ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/${project}/spirv
        SHADERMAKE_OPTIONS_SPIRV ${SHADERMAKE_OPTIONS_SPIRV}
    )
else()
    caustica_compile_shaders(${caustica_shader_compile_common}
        CONFIG ${caustica_shader_config_vk}
        SPIRV_DXC ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/${project}/spirv
        SHADERMAKE_OPTIONS_SPIRV ${SHADERMAKE_OPTIONS_SPIRV}
    )
endif()


##########################################################################
# Path-tracing libraries (lib_6_6) -> ShaderDynamic/Bin via DXC precompile.
# ShaderMake cannot compile lib_* profiles; Python script matches runtime DXC hash layout.
# PathTracerShaders.cfg documents tier permutations (not consumed by ShaderMake).
set(caustica_pt_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/caustica/shaders")
file(GLOB_RECURSE caustica_pt_shader_sources
    "${caustica_pt_shader_dir}/PathTracer*.hlsl"
    "${caustica_pt_shader_dir}/PathTracer*.hlsli"
)
set_source_files_properties(${caustica_pt_shader_sources} PROPERTIES VS_TOOL_OVERRIDE "None")

option(CAUSTICA_PRECOMPILE_PT_SHADERS "Precompile path-tracing DXC shader bins into ShaderDynamic/Bin" ON)
if (CAUSTICA_PRECOMPILE_PT_SHADERS AND NOT CAUSTICA_DISTRIBUTION_BUILD)
    find_package(Python3 COMPONENTS Interpreter)
    if (Python3_FOUND)
        if (WIN32)
            set(caustica_pt_precompile_api both)
        else()
            set(caustica_pt_precompile_api vulkan)
        endif()
        add_custom_target(causticaPathTracerShaders
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Bin"
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/support/python/precompile_pt_shader_bins.py"
                --shader-api ${caustica_pt_precompile_api}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            DEPENDS ShaderDynamicAssets ShaderDynamicAssets_CopyAlways
            COMMENT "Precompiling path-tracing shader bins (DXC hash layout)"
            SOURCES ${caustica_pt_shader_sources}
        )
    else()
        add_custom_target(causticaPathTracerShaders
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Bin"
            COMMENT "Path-tracing shader precompile skipped (Python3 not found)"
            SOURCES ${caustica_pt_shader_sources}
        )
    endif()
else()
    add_custom_target(causticaPathTracerShaders
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Bin"
        COMMENT "Path-tracing shader precompile disabled"
        SOURCES ${caustica_pt_shader_sources}
    )
endif()
set_target_properties(causticaPathTracerShaders PROPERTIES FOLDER ${folder})


##########################################################################
################ dynamic shader recompile dependencies ###################
if (CAUSTICA_DISTRIBUTION_BUILD)
add_custom_target(ShaderDynamicAssets_CopyAlways
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Bin"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Source"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Tools"
    COMMENT "Pruning runtime shader sources and compiler tools for distribution build"
)
add_custom_target(ShaderDynamicAssets ALL)
else()
set(DYNSH_COPY1_MARKER "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Tools/copyVK.marker")
set(DYNSH_COPY2_MARKER "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Tools/copyDX.marker")
set(DYNSH_COPY3_MARKER "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Source/copy.marker")

# vulkan SDK / SPIR-V dxc (optional)
if (DXC_SPIRV_PATH)
get_filename_component(DXC_SPIRV_PATH_MINUS_EXE ${DXC_SPIRV_PATH} DIRECTORY)
if (WIN32)
    file(GLOB FILES_TO_COPY_2 "${DXC_SPIRV_PATH_MINUS_EXE}/*.exe" "${DXC_SPIRV_PATH_MINUS_EXE}/*.dll" "${DXC_SPIRV_PATH_MINUS_EXE}/*.json")
else()
    file(GLOB FILES_TO_COPY_2 "${DXC_SPIRV_PATH}" "${DXC_SPIRV_PATH_MINUS_EXE}/*.so" "${DXC_SPIRV_PATH_MINUS_EXE}/*.json")
    get_filename_component(DXC_SPIRV_ROOT_DIR ${DXC_SPIRV_PATH_MINUS_EXE} DIRECTORY)
    file(GLOB FILES_TO_COPY_2_LIB "${DXC_SPIRV_ROOT_DIR}/lib/*.so" "${DXC_SPIRV_ROOT_DIR}/lib/*.so.*")
endif()
set(DEST_DIR_2 "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Tools/vk/x64")
set(DEST_LIB_DIR_2 "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Tools/vk/lib")
if (WIN32)
add_custom_command(
    OUTPUT "${DYNSH_COPY1_MARKER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_DIR_2}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FILES_TO_COPY_2} ${DEST_DIR_2}
    COMMAND ${CMAKE_COMMAND} -E touch "${DYNSH_COPY1_MARKER}"
    COMMENT "Copying ${DXC_SPIRV_PATH_MINUS_EXE} to ${DEST_DIR_2}"
)
else()
add_custom_command(
    OUTPUT "${DYNSH_COPY1_MARKER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_DIR_2}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_LIB_DIR_2}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FILES_TO_COPY_2} ${DEST_DIR_2}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FILES_TO_COPY_2_LIB} ${DEST_LIB_DIR_2}
    COMMAND ${CMAKE_COMMAND} -E touch "${DYNSH_COPY1_MARKER}"
    COMMENT "Copying ${DXC_SPIRV_PATH_MINUS_EXE} to ${DEST_DIR_2}"
)
endif()
endif()

# d3d12 dxc (Windows only)
if (WIN32)
set(SOURCE_DIR_1 "${DXC_CUSTOM_PATH}")
set(DEST_DIR_1 "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Tools/d3d12/x64")
add_custom_command(
    OUTPUT "${DYNSH_COPY2_MARKER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_DIR_1}
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${SOURCE_DIR_1} ${DEST_DIR_1}
    COMMAND ${CMAKE_COMMAND} -E touch "${DYNSH_COPY2_MARKER}"
    COMMENT "Copying ${SOURCE_DIR_1} to ${DEST_DIR_1}"
)
else()
add_custom_command(
    OUTPUT "${DYNSH_COPY2_MARKER}"
    COMMAND ${CMAKE_COMMAND} -E touch "${DYNSH_COPY2_MARKER}"
    COMMENT "Skipping D3D12 DXC copy on non-Windows"
)
endif()

# shader files - main caustica and localized framework shader dependencies
set(SOURCE_DIR_CAUSTICA_SHADERS "${PROJECT_SOURCE_DIR}/caustica/caustica/shaders")
set(DEST_DIR_CAUSTICA_SHADERS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Source/caustica/caustica/shaders")
# shader files - NVAPI shader dependencies
set(SOURCE_DIR_NVAPI_SHADERS "${PROJECT_SOURCE_DIR}/External/NVAPI")
set(DEST_DIR_NVAPI_SHADERS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Source/External/NVAPI")
file(GLOB SOURCE_DIR_NVAPI_SHADERS_FILES "${SOURCE_DIR_NVAPI_SHADERS}/nvHLSL*.h" "${SOURCE_DIR_NVAPI_SHADERS}/nvShader*.h")
#message("SourceDir=${SOURCE_DIR_CAUSTICA_SHADERS}, DestDir=${DEST_DIR_CAUSTICA_SHADERS}")
add_custom_target(ShaderDynamicAssets_CopyAlways
#    OUTPUT "${DYNSH_COPY3_MARKER}" <- let this run every build; it will only update changed
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_DIR_CAUSTICA_SHADERS}
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${SOURCE_DIR_CAUSTICA_SHADERS} ${DEST_DIR_CAUSTICA_SHADERS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_DIR_NVAPI_SHADERS}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SOURCE_DIR_NVAPI_SHADERS_FILES} ${DEST_DIR_NVAPI_SHADERS}
#    COMMAND ${CMAKE_COMMAND} -E touch "${DYNSH_COPY3_MARKER}" <- let this run every build; it will only update changed
    COMMENT "Copying shader sources"
)

if (WIN32)
    set(shader_dynamic_copy_deps "${DYNSH_COPY1_MARKER}" "${DYNSH_COPY2_MARKER}")
elseif (DXC_SPIRV_PATH)
    set(shader_dynamic_copy_deps "${DYNSH_COPY1_MARKER}")
else()
    set(shader_dynamic_copy_deps "")
endif()
add_custom_target(ShaderDynamicAssets ALL DEPENDS ${shader_dynamic_copy_deps})
endif()

##########################################################################

# Engine runtime (caustica.lib) + editor application sources
add_subdirectory(caustica)       # builds caustica (engine static library)
add_subdirectory(../application/editor application/editor)  # sets CAUSTICA_EDITOR_SOURCES and CAUSTICA_EDITOR_ENTRY_SOURCE
add_library(causticaApp STATIC ${CAUSTICA_EDITOR_SOURCES})
if (MSVC)
    target_compile_options(causticaApp PRIVATE /utf-8)
endif()
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(causticaApp PRIVATE
        -Wno-error=deprecated-declarations
        -Wno-error=changes-meaning
        -Wno-error=attributes
        -Wno-error=format-security
        -Wno-error=format-zero-length
        $<$<CXX_COMPILER_ID:GNU>:-Wno-error=stringop-overflow>
        $<$<CXX_COMPILER_ID:GNU>:-Wno-error=unused-result>
    )
endif()
target_link_libraries(causticaApp PUBLIC causticaLib)
target_include_directories(causticaApp PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/application/editor"
    "${CMAKE_SOURCE_DIR}/application/editor/app"
    "${CMAKE_SOURCE_DIR}/application/editor/ui"
    "${CMAKE_SOURCE_DIR}/application/editor/game"
)
target_compile_definitions(causticaApp PUBLIC CAUSTICA_D3D12_WITH_NVAPI=$<BOOL:${NVRHI_WITH_NVAPI}>)
target_compile_definitions(causticaApp PUBLIC CAUSTICA_WITH_OIDN=$<BOOL:${CAUSTICA_WITH_OIDN}>)
target_compile_definitions(causticaApp PUBLIC CAUSTICA_WITH_PYTHON=$<BOOL:${CAUSTICA_WITH_PYTHON}>)
target_compile_definitions(causticaApp PUBLIC CAUSTICA_WITH_NATIVE_DLSS=$<BOOL:${CAUSTICA_WITH_NATIVE_DLSS}>)
if (CAUSTICA_WITH_STREAMLINE OR CAUSTICA_WITH_NATIVE_DLSS)
    target_compile_definitions(causticaApp PUBLIC CAUSTICA_WITH_ANY_DLSS=1)
else()
    target_compile_definitions(causticaApp PUBLIC CAUSTICA_WITH_ANY_DLSS=0)
endif()
if (CAUSTICA_WITH_OIDN)
    target_link_libraries(causticaApp PUBLIC OpenImageDenoise)
endif()

# -------------------------------------------------------------------
# Embedded Python scripting (nanobind + CPython development library)
# -------------------------------------------------------------------
if (CAUSTICA_WITH_PYTHON AND caustica_python_core_sources)
    # Shared bindings + scripting host go into causticaApp so both the host
    # executable (embed mode) and the standalone Python extension can pull
    # them in.  The NB_MODULE() entry points themselves live in their own
    # translation units which are added later to the matching targets.
    target_sources(causticaApp PRIVATE ${caustica_python_core_sources})

    # nanobind ships a static helper target named `nanobind-static`.  Link it
    # PUBLIC so the embed-mode entry point (compiled into caustica.exe) can use it.
    if (NOT TARGET nanobind-static)
        nanobind_build_library(nanobind-static)
    endif()
    target_link_libraries(causticaApp PUBLIC nanobind-static Python::Python)
    target_include_directories(causticaApp PUBLIC ${Python_INCLUDE_DIRS})
    if (MSVC)
        target_compile_options(causticaApp PRIVATE /bigobj)
    endif()

    # Copy a few example python scripts next to the binary at build time
    file(GLOB caustica_python_examples "${CMAKE_CURRENT_SOURCE_DIR}/Python/Examples/*.py")
    if (caustica_python_examples)
        add_custom_target(causticaPythonExamples ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/PythonExamples"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${caustica_python_examples} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/PythonExamples"
            COMMENT "Copying caustica Python example scripts"
        )
        add_dependencies(causticaApp causticaPythonExamples)
        set_target_properties(causticaPythonExamples PROPERTIES FOLDER ${folder})
    endif()
endif()

add_dependencies(causticaApp caustica_shaders nrd_shaders omm_shaders causticaPathTracerShaders ShaderDynamicAssets ShaderDynamicAssets_CopyAlways)
set_target_properties(causticaApp PROPERTIES FOLDER ${folder})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${shaders})
# AgilitySDK support for causticaApp
if (CAUSTICA_D3D_AGILITY_SDK_PATH AND CAUSTICA_D3D_AGILITY_SDK_VERSION)
    if (NOT DEFINED CAUSTICA_D3D_AGILITY_SDK_VERSION)
        message(FATAL_ERROR "CAUSTICA_D3D_AGILITY_SDK_VERSION must be defined when CAUSTICA_D3D_AGILITY_SDK_PATH is defined")
    endif()
    target_compile_definitions(causticaApp PUBLIC CAUSTICA_D3D_AGILITY_SDK_VERSION=${CAUSTICA_D3D_AGILITY_SDK_VERSION})
    target_compile_definitions(causticaApp PRIVATE NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=$<BOOL:${NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP}>)
    target_include_directories(causticaApp PRIVATE ${CAUSTICA_D3D_AGILITY_SDK_PATH}/build/native/include)
endif()

if (CAUSTICA_WITH_OIDN)
    if (WIN32)
        file(GLOB CAUSTICA_OIDN_RUNTIME_LIBS "${CAUSTICA_OIDN_LIBRARY_DIR}/*.dll")
    else()
        file(GLOB CAUSTICA_OIDN_RUNTIME_LIBS "${CAUSTICA_OIDN_LIBRARY_DIR}/*.so*")
    endif()
endif()

# Macro to create sample executables with common settings
macro(add_caustica_sample TARGET_NAME MAIN_SOURCE)
    set(_extra_sources "")
    if (CAUSTICA_WITH_PYTHON)
        # Embed-mode NB_MODULE has to live in a target that does NOT also link
        # the Python extension's entry point - hence we attach it to each
        # executable individually instead of causticaApp.
        list(APPEND _extra_sources "${CMAKE_CURRENT_SOURCE_DIR}/Python/PythonBindings_Embed.cpp")
    endif()
    add_executable(${TARGET_NAME} WIN32 ${MAIN_SOURCE} ${_extra_sources})
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${TARGET_NAME} PRIVATE
            -Wno-error=deprecated-declarations
            -Wno-error=changes-meaning
            -Wno-error=attributes
            -Wno-error=format-security
            -Wno-error=format-zero-length
            $<$<CXX_COMPILER_ID:GNU>:-Wno-error=stringop-overflow>
            $<$<CXX_COMPILER_ID:GNU>:-Wno-error=unused-result>
        )
    endif()
    if (MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /utf-8)
    endif()
    target_link_libraries(${TARGET_NAME} PRIVATE causticaApp nvrhi)
    add_dependencies(${TARGET_NAME} caustica_shaders nrd_shaders omm_shaders ShaderDynamicAssets ShaderDynamicAssets_CopyAlways)
    set_target_properties(${TARGET_NAME} PROPERTIES 
        FOLDER ${folder}
        OUTPUT_NAME_DEBUG "${TARGET_NAME}D"
        OUTPUT_NAME_RELEASE "${TARGET_NAME}"
    )

    if (CAUSTICA_WITH_OIDN)
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CAUSTICA_OIDN_RUNTIME_LIBS} "$<TARGET_FILE_DIR:${TARGET_NAME}>"
            COMMAND_EXPAND_LISTS
        )
    endif()
    
    # AgilitySDK support for this target
    if (CAUSTICA_D3D_AGILITY_SDK_PATH AND CAUSTICA_D3D_AGILITY_SDK_VERSION)
        target_compile_definitions(${project} PUBLIC CAUSTICA_D3D_AGILITY_SDK_VERSION=${CAUSTICA_D3D_AGILITY_SDK_VERSION})
        add_custom_command(
            TARGET ${project} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${project}>/D3D12/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CAUSTICA_D3D_AGILITY_SDK_PATH}/build/native/bin/x64/D3D12Core.dll "$<TARGET_FILE_DIR:${project}>/D3D12/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CAUSTICA_D3D_AGILITY_SDK_PATH}/build/native/bin/x64/d3d12SDKLayers.dll "$<TARGET_FILE_DIR:${project}>/D3D12/"
        )
        target_compile_definitions(${project} PRIVATE NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=$<BOOL:${NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP}>)
        target_include_directories(${project} PRIVATE ${CAUSTICA_D3D_AGILITY_SDK_PATH}/build/native/include)
    endif()
endmacro()

add_caustica_sample(caustica "${CAUSTICA_EDITOR_ENTRY_SOURCE}")

# -------------------------------------------------------------------
# Python extension module: caustica.pyd
# -------------------------------------------------------------------
# Allows scripts to drive the renderer headlessly:
#   import caustica
#   r = caustica.Renderer(width=1280, height=720, headless=True,
#                      scene='bistro-programmer-art.scene.json')
#   r.step_until_accumulated()
#   r.save_screenshot('frame.png')
if (CAUSTICA_WITH_PYTHON AND TARGET nanobind-static)
    nanobind_add_module(caustica_py STABLE_ABI
        "${CMAKE_CURRENT_SOURCE_DIR}/Python/PythonBindings_Extension.cpp"
    )
    # Note: nanobind defaults the python module name to the target name; we
    # pin it to `caustica` so users can write `import caustica`.
    # Drop the .pyd next to caustica.exe / the runtime DLLs / ShaderPrecompiled
    # so the extension can find shaders and AgilitySDK redistributables.
    set_target_properties(caustica_py PROPERTIES
        OUTPUT_NAME "caustica"
        FOLDER ${folder}
        LIBRARY_OUTPUT_DIRECTORY                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    )

    target_link_libraries(caustica_py PRIVATE causticaApp)
    target_compile_definitions(caustica_py PRIVATE CAUSTICA_BUILDING_EXTENSION=1)
    if (MSVC)
        target_compile_options(caustica_py PRIVATE /utf-8)
    endif()

    add_dependencies(caustica_py caustica_shaders nrd_shaders omm_shaders ShaderDynamicAssets ShaderDynamicAssets_CopyAlways)

    if (CAUSTICA_WITH_OIDN)
        add_custom_command(
            TARGET caustica_py POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CAUSTICA_OIDN_RUNTIME_LIBS} "$<TARGET_FILE_DIR:caustica_py>"
            COMMAND_EXPAND_LISTS
        )
    endif()

    if (CAUSTICA_D3D_AGILITY_SDK_PATH AND CAUSTICA_D3D_AGILITY_SDK_VERSION)
        target_compile_definitions(caustica_py PUBLIC CAUSTICA_D3D_AGILITY_SDK_VERSION=${CAUSTICA_D3D_AGILITY_SDK_VERSION})
        add_custom_command(
            TARGET caustica_py POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:caustica_py>/D3D12/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CAUSTICA_D3D_AGILITY_SDK_PATH}/build/native/bin/x64/D3D12Core.dll "$<TARGET_FILE_DIR:caustica_py>/D3D12/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CAUSTICA_D3D_AGILITY_SDK_PATH}/build/native/bin/x64/d3d12SDKLayers.dll "$<TARGET_FILE_DIR:caustica_py>/D3D12/"
        )
        target_compile_definitions(caustica_py PRIVATE NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=$<BOOL:${NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP}>)
        target_include_directories(caustica_py PRIVATE ${CAUSTICA_D3D_AGILITY_SDK_PATH}/build/native/include)
    endif()
endif()
