#pragma once

#include <assets/RuntimeMeshLoadTypes.h>

#include <filesystem>

namespace caustica
{

RuntimeMeshLoadResult LoadRuntimeMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath);
RuntimeMeshLoadResult LoadRuntimeGltfMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath);
RuntimeMeshLoadResult LoadRuntimeObjMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath);

} // namespace caustica
