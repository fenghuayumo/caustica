# Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.


include(../External/cmake/compileshaders.cmake)
file(GLOB_RECURSE shaders "*.hlsl" "*.hlsli")
file(GLOB_RECURSE shared_sources "*.cpp" "*.h" "*.md")
list(FILTER shaders EXCLUDE REGEX "/Framework/")
list(FILTER shared_sources EXCLUDE REGEX "/Framework/")
# Separate out shared sources from entry point files
list(REMOVE_ITEM shared_sources 
    "${CMAKE_CURRENT_SOURCE_DIR}/AdvancedSample.cpp"
)

# Strip the Python sources from the generic shared_sources list - they are handled
# below using nanobind helpers (only added when RTXPT_WITH_PYTHON is enabled).
file(GLOB rtxpt_python_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/Python/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Python/*.h"
)
if (rtxpt_python_sources)
    list(REMOVE_ITEM shared_sources ${rtxpt_python_sources})
endif()

if (NOT WIN32)
    list(REMOVE_ITEM shared_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/SampleCommon/SplashScreen.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/SampleCommon/SplashScreen.h"
    )
endif()

# The two NB_MODULE entry points are mutually exclusive (one defines the
# embed-mode `caustica` module that lives inside caustica.exe, the other defines
# the Python extension `caustica.pyd`).  They are added to their respective
# targets later - never to causticaCore.
set(rtxpt_python_core_sources ${rtxpt_python_sources})
list(REMOVE_ITEM rtxpt_python_core_sources
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

if(DONUT_WITH_AFTERMATH OR DONUT_EMBED_SHADER_PDBS)
    set(shadermake_options "--embedPDB")
#else()
#    set(shadermake_options --PDB)
endif()

#set(shadermake_options --verbose)

if(CAUSTICA_DISTRIBUTION_BUILD)
    string(APPEND shadermake_options " --stripReflection")
endif()

if(RTXPT_D3D_AGILITY_SDK_VERSION AND (RTXPT_D3D_AGILITY_SDK_VERSION GREATER_EQUAL "619"))
    string(APPEND shadermake_options " -m 6_9")
endif()

# SPIR-V builds cannot compile the DX12 OMM shader variants in shaders.cfg.
set(rtxpt_shader_config_dxil "${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg")
set(rtxpt_shader_config_vk "${CMAKE_CURRENT_SOURCE_DIR}/shaders_vk.cfg")
file(READ "${rtxpt_shader_config_dxil}" rtxpt_shader_config_vk_contents)
string(REGEX REPLACE "[^\n]*NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=1[^\n]*\n?" "" rtxpt_shader_config_vk_contents "${rtxpt_shader_config_vk_contents}")
file(WRITE "${rtxpt_shader_config_vk}" "${rtxpt_shader_config_vk_contents}")

set(rtxpt_shader_compile_common
    TARGET caustica_shaders
    SOURCES ${shaders}
    FOLDER ${folder}
    INCLUDES ${CMAKE_SOURCE_DIR}/External/Nrd/Shaders/Include ${CMAKE_SOURCE_DIR}/External/Rtxdi/Include ${CMAKE_SOURCE_DIR}/External
    SHADERMAKE_OPTIONS ${shadermake_options}
)
if (WIN32)
    donut_compile_shaders(${rtxpt_shader_compile_common}
        CONFIG ${rtxpt_shader_config_dxil}
        DXIL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/${project}/dxil
        SHADERMAKE_OPTIONS_DXIL ${SHADERMAKE_OPTIONS_DXIL}
    )
    donut_compile_shaders(${rtxpt_shader_compile_common}
        CONFIG ${rtxpt_shader_config_vk}
        SPIRV_DXC ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/${project}/spirv
        SHADERMAKE_OPTIONS_SPIRV ${SHADERMAKE_OPTIONS_SPIRV}
    )
else()
    donut_compile_shaders(${rtxpt_shader_compile_common}
        CONFIG ${rtxpt_shader_config_vk}
        SPIRV_DXC ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderPrecompiled/${project}/spirv
        SHADERMAKE_OPTIONS_SPIRV ${SHADERMAKE_OPTIONS_SPIRV}
    )
endif()


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

# shader files - main caustica
set(SOURCE_DIR_RTXPT_SHADERS "${PROJECT_SOURCE_DIR}/caustica/Shaders")
set(DEST_DIR_RTXPT_SHADERS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Source/caustica")
# shader files - localized framework shader dependencies
set(SOURCE_DIR_DONUT_SHADERS "${PROJECT_SOURCE_DIR}/caustica/Framework/include/donut/shaders")
set(DEST_DIR_DONUT_SHADERS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Source/External/donut/shaders")
# shader files - NVAPI shader dependencies
set(SOURCE_DIR_NVAPI_SHADERS "${PROJECT_SOURCE_DIR}/External/NVAPI")
set(DEST_DIR_NVAPI_SHADERS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ShaderDynamic/Source/External/NVAPI")
file(GLOB SOURCE_DIR_NVAPI_SHADERS_FILES "${SOURCE_DIR_NVAPI_SHADERS}/nvHLSL*.h" "${SOURCE_DIR_NVAPI_SHADERS}/nvShader*.h")
#message("SourceDir=${SOURCE_DIR_RTXPT_SHADERS}, DestDir=${DEST_DIR_RTXPT_SHADERS}")
add_custom_target(ShaderDynamicAssets_CopyAlways
#    OUTPUT "${DYNSH_COPY3_MARKER}" <- let this run every build; it will only update changed
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_DIR_RTXPT_SHADERS}
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${SOURCE_DIR_RTXPT_SHADERS} ${DEST_DIR_RTXPT_SHADERS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_DIR_DONUT_SHADERS}
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${SOURCE_DIR_DONUT_SHADERS} ${DEST_DIR_DONUT_SHADERS}
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

# Create a static library for shared code (used by both samples)
add_subdirectory(Framework)
add_library(causticaCore STATIC ${shared_sources})
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(causticaCore PRIVATE
        -Wno-error=deprecated-declarations
        -Wno-error=changes-meaning
        -Wno-error=attributes
        -Wno-error=format-security
        -Wno-error=format-zero-length
        $<$<CXX_COMPILER_ID:GNU>:-Wno-error=stringop-overflow>
        $<$<CXX_COMPILER_ID:GNU>:-Wno-error=unused-result>
    )
endif()
target_link_libraries(causticaCore PUBLIC cxxopts causticaFramework NRD Rtxdi omm-lib omm-gpu-nvrhi)
target_include_directories(causticaCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "../External/Nrd/Include" ${CMAKE_SOURCE_DIR}/External/Rtxdi/Include)
target_compile_definitions(causticaCore PUBLIC RTXPT_D3D12_WITH_NVAPI=$<BOOL:${NVRHI_WITH_NVAPI}>)
target_compile_definitions(causticaCore PUBLIC RTXPT_WITH_OIDN=$<BOOL:${RTXPT_WITH_OIDN}>)
target_compile_definitions(causticaCore PUBLIC RTXPT_WITH_PYTHON=$<BOOL:${RTXPT_WITH_PYTHON}>)
target_compile_definitions(causticaCore PUBLIC RTXPT_WITH_NATIVE_DLSS=$<BOOL:${RTXPT_WITH_NATIVE_DLSS}>)
if (DONUT_WITH_STREAMLINE OR RTXPT_WITH_NATIVE_DLSS)
    target_compile_definitions(causticaCore PUBLIC RTXPT_WITH_ANY_DLSS=1)
else()
    target_compile_definitions(causticaCore PUBLIC RTXPT_WITH_ANY_DLSS=0)
endif()
if (RTXPT_WITH_OIDN)
    target_link_libraries(causticaCore PUBLIC OpenImageDenoise)
endif()

# -------------------------------------------------------------------
# Embedded Python scripting (nanobind + CPython development library)
# -------------------------------------------------------------------
if (RTXPT_WITH_PYTHON AND rtxpt_python_core_sources)
    # Shared bindings + scripting host go into causticaCore so both the host
    # executable (embed mode) and the standalone Python extension can pull
    # them in.  The NB_MODULE() entry points themselves live in their own
    # translation units which are added later to the matching targets.
    target_sources(causticaCore PRIVATE ${rtxpt_python_core_sources})

    # nanobind ships a static helper target named `nanobind-static`.  Link it
    # PUBLIC so the embed-mode entry point (compiled into caustica.exe) can use it.
    if (NOT TARGET nanobind-static)
        nanobind_build_library(nanobind-static)
    endif()
    target_link_libraries(causticaCore PUBLIC nanobind-static Python::Python)
    target_include_directories(causticaCore PUBLIC ${Python_INCLUDE_DIRS})
    if (MSVC)
        target_compile_options(causticaCore PRIVATE /bigobj)
    endif()

    # Copy a few example python scripts next to the binary at build time
    file(GLOB rtxpt_python_examples "${CMAKE_CURRENT_SOURCE_DIR}/Python/Examples/*.py")
    if (rtxpt_python_examples)
        add_custom_target(causticaPythonExamples ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/PythonExamples"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${rtxpt_python_examples} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/PythonExamples"
            COMMENT "Copying caustica Python example scripts"
        )
        add_dependencies(causticaCore causticaPythonExamples)
        set_target_properties(causticaPythonExamples PROPERTIES FOLDER ${folder})
    endif()
endif()

add_dependencies(causticaCore caustica_shaders nrd_shaders omm_shaders ShaderDynamicAssets ShaderDynamicAssets_CopyAlways)
set_target_properties(causticaCore PROPERTIES FOLDER ${folder})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${shaders})
# AgilitySDK support for causticaCore
if (RTXPT_D3D_AGILITY_SDK_PATH AND RTXPT_D3D_AGILITY_SDK_VERSION)
    if (NOT DEFINED RTXPT_D3D_AGILITY_SDK_VERSION)
        message(FATAL_ERROR "RTXPT_D3D_AGILITY_SDK_VERSION must be defined when RTXPT_D3D_AGILITY_SDK_PATH is defined")
    endif()
    target_compile_definitions(causticaCore PUBLIC RTXPT_D3D_AGILITY_SDK_VERSION=${RTXPT_D3D_AGILITY_SDK_VERSION})
    target_compile_definitions(causticaCore PRIVATE NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=$<BOOL:${NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP}>)
    target_include_directories(causticaCore PRIVATE ${RTXPT_D3D_AGILITY_SDK_PATH}/build/native/include)
endif()

if (RTXPT_WITH_OIDN)
    if (WIN32)
        file(GLOB RTXPT_OIDN_RUNTIME_LIBS "${RTXPT_OIDN_LIBRARY_DIR}/*.dll")
    else()
        file(GLOB RTXPT_OIDN_RUNTIME_LIBS "${RTXPT_OIDN_LIBRARY_DIR}/*.so*")
    endif()
endif()

# Macro to create sample executables with common settings
macro(add_caustica_sample TARGET_NAME MAIN_SOURCE)
    set(_extra_sources "")
    if (RTXPT_WITH_PYTHON)
        # Embed-mode NB_MODULE has to live in a target that does NOT also link
        # the Python extension's entry point - hence we attach it to each
        # executable individually instead of causticaCore.
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
    target_link_libraries(${TARGET_NAME} PRIVATE causticaCore)
    add_dependencies(${TARGET_NAME} caustica_shaders nrd_shaders omm_shaders ShaderDynamicAssets ShaderDynamicAssets_CopyAlways)
    set_target_properties(${TARGET_NAME} PROPERTIES 
        FOLDER ${folder}
        OUTPUT_NAME_DEBUG "${TARGET_NAME}D"
        OUTPUT_NAME_RELEASE "${TARGET_NAME}"
    )

    if (RTXPT_WITH_OIDN)
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RTXPT_OIDN_RUNTIME_LIBS} "$<TARGET_FILE_DIR:${TARGET_NAME}>"
            COMMAND_EXPAND_LISTS
        )
    endif()
    
    # AgilitySDK support for this target
    if (RTXPT_D3D_AGILITY_SDK_PATH AND RTXPT_D3D_AGILITY_SDK_VERSION)
        target_compile_definitions(${project} PUBLIC RTXPT_D3D_AGILITY_SDK_VERSION=${RTXPT_D3D_AGILITY_SDK_VERSION})
        add_custom_command(
            TARGET ${project} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${project}>/D3D12/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RTXPT_D3D_AGILITY_SDK_PATH}/build/native/bin/x64/D3D12Core.dll "$<TARGET_FILE_DIR:${project}>/D3D12/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RTXPT_D3D_AGILITY_SDK_PATH}/build/native/bin/x64/d3d12SDKLayers.dll "$<TARGET_FILE_DIR:${project}>/D3D12/"
        )
        target_compile_definitions(${project} PRIVATE NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=$<BOOL:${NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP}>)
        target_include_directories(${project} PRIVATE ${RTXPT_D3D_AGILITY_SDK_PATH}/build/native/include)
    endif()
endmacro()

add_caustica_sample(caustica "AdvancedSample.cpp")

# -------------------------------------------------------------------
# Python extension module: caustica.pyd
# -------------------------------------------------------------------
# Allows scripts to drive the renderer headlessly:
#   import caustica
#   r = caustica.Renderer(width=1280, height=720, headless=True,
#                      scene='bistro-programmer-art.scene.json')
#   r.step_until_accumulated()
#   r.save_screenshot('frame.png')
if (RTXPT_WITH_PYTHON AND TARGET nanobind-static)
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

    target_link_libraries(caustica_py PRIVATE causticaCore)
    target_compile_definitions(caustica_py PRIVATE RTXPT_BUILDING_EXTENSION=1)

    add_dependencies(caustica_py caustica_shaders nrd_shaders omm_shaders ShaderDynamicAssets ShaderDynamicAssets_CopyAlways)

    if (RTXPT_WITH_OIDN)
        add_custom_command(
            TARGET caustica_py POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RTXPT_OIDN_RUNTIME_LIBS} "$<TARGET_FILE_DIR:caustica_py>"
            COMMAND_EXPAND_LISTS
        )
    endif()

    if (RTXPT_D3D_AGILITY_SDK_PATH AND RTXPT_D3D_AGILITY_SDK_VERSION)
        target_compile_definitions(caustica_py PUBLIC RTXPT_D3D_AGILITY_SDK_VERSION=${RTXPT_D3D_AGILITY_SDK_VERSION})
        add_custom_command(
            TARGET caustica_py POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:caustica_py>/D3D12/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RTXPT_D3D_AGILITY_SDK_PATH}/build/native/bin/x64/D3D12Core.dll "$<TARGET_FILE_DIR:caustica_py>/D3D12/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RTXPT_D3D_AGILITY_SDK_PATH}/build/native/bin/x64/d3d12SDKLayers.dll "$<TARGET_FILE_DIR:caustica_py>/D3D12/"
        )
        target_compile_definitions(caustica_py PRIVATE NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=$<BOOL:${NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP}>)
        target_include_directories(caustica_py PRIVATE ${RTXPT_D3D_AGILITY_SDK_PATH}/build/native/include)
    endif()
endif()
