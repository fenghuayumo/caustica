#pragma once

#include <scene/GaussianSplatData.h>

#include <filesystem>
#include <string>
#include <vector>

namespace caustica
{

struct GaussianSplatDataset
{
    std::vector<GaussianSplatData> splats;
    std::vector<math::float4> shCoefficients;
    uint32_t shDegree = 0;
    std::string sourcePath;
};

// Loads a 3D Gaussian Splatting .ply file into CPU-side splat data.
bool loadGaussianSplatPly(
    const std::filesystem::path& filePath,
    bool convertRdfToRub,
    GaussianSplatDataset& outDataset);

} // namespace caustica
