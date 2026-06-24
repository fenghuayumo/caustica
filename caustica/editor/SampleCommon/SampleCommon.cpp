#include "SampleCommon.h"

#include <core/json.h>
#include <core/log.h>
#include <scene/SceneTypes.h>

#include <json/json.h>
#include <fstream>

using namespace caustica::math;

// =============================================================================
// NVRHI utility
// =============================================================================

uint64_t GetEstimatedTextureSize(const nvrhi::TextureDesc& desc)
{
    nvrhi::FormatInfo fi = nvrhi::getFormatInfo(desc.format);

    uint64_t pixelsCount = 0;

    uint32_t w = desc.width;
    uint32_t h = desc.height;
    uint32_t d = desc.depth;

    for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
    {
        pixelsCount += size_t(w) * h * d * desc.arraySize;
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
        d = std::max(1u, d >> 1);
    }

    return pixelsCount / fi.blockSize * fi.bytesPerBlock;
}

// =============================================================================
// CompressTextures (depends on LoadedTexture from scene layer)
// =============================================================================

bool CompressTextures(
    std::map<std::shared_ptr<caustica::LoadedTexture>, TextureCompressionType>& uncompressedTextures)
{
    std::string batchFileName = std::string(getenv("localappdata")) +
        "\\temp\\caustica_compressor.bat";
    std::ofstream batchFile(batchFileName, std::ios_base::trunc);
    if (!batchFile.is_open())
    {
        caustica::message(caustica::Severity::Error, "Unable to write %s", batchFileName.c_str());
        return false;
    }

    std::string cmdLine;
    cmdLine += "ECHO: \n";
    cmdLine += "WHERE nvtt_export \n";
    cmdLine += "IF %ERRORLEVEL% NEQ 0 (goto :error_tool)\n";
    cmdLine += "ECHO: \n";
    cmdLine += "ECHO nvtt_export exists in the Path, proceeding with compression (this might take a while!) \n";
    cmdLine += "ECHO: \n";

    unsigned int i = 0;
    unsigned int totalCount = static_cast<unsigned int>(uncompressedTextures.size());
    for (auto it : uncompressedTextures)
    {
        auto texture = it.first;
        std::string inPath = texture->path;
        std::string outPath = std::filesystem::path(inPath).replace_extension(".dds").string();

        cmdLine += "ECHO converting texture " + std::to_string(++i) + " "
            + " out of " + std::to_string(totalCount) + "\n";
        cmdLine += "nvtt_export -f 23 ";

        if (it.second == TextureCompressionType::Normalmap)
            cmdLine += " --no-mip-gamma-correct";
        else if (it.second == TextureCompressionType::GenericLinear)
            cmdLine += " --no-mip-gamma-correct";
        else if (it.second == TextureCompressionType::GenericSRGB)
            cmdLine += " --mip-gamma-correct";

        cmdLine += " -o \"" + outPath + "\" \"" + inPath + "\"\n";
    }

    cmdLine += "ECHO:\npause\nECHO on\nexit /b 0\n";
    cmdLine += ":error_tool\n";
    cmdLine += "ECHO !! nvtt_export.exe not found !!\n";
    cmdLine += "ECHO nvtt_export.exe is part of https://developer.nvidia.com/nvidia-texture-tools-exporter\n";
    cmdLine += "ECHO Please install and add to PATH, then retry!\n";
    cmdLine += "pause\nECHO on\nexit /b 1\n";

    batchFile << cmdLine;
    batchFile.close();

    std::string startCmd = " \"\" " + batchFileName;
    std::system(startCmd.c_str());
    return true;
}
