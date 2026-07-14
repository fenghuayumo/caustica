#pragma once

#include <math/math.h>

#include <filesystem>
#include <vector>

namespace caustica
{
    struct StlMeshData
    {
        std::vector<dm::float3> positions;
        std::vector<dm::float3> normals;
        std::vector<uint32_t> indices;
        dm::box3 bounds = dm::box3::empty();

        [[nodiscard]] bool empty() const { return positions.empty() || indices.empty(); }
    };

    // Loads ASCII or binary STL into triangle mesh data (positions + smooth/face normals).
    bool LoadStlFile(const std::filesystem::path& filePath, StlMeshData& outMesh);
}
