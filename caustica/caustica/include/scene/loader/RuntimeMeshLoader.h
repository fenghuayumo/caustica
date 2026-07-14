#pragma once

#include <assets/RuntimeMeshLoadTypes.h>

#include <filesystem>

namespace caustica
{

RuntimeMeshLoadResult loadRuntimeMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath);
RuntimeMeshLoadResult loadRuntimeGltfMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath);
RuntimeMeshLoadResult loadRuntimeObjMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath);
RuntimeMeshLoadResult loadRuntimeUrdfMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath);

} // namespace caustica
