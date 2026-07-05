#include <render/Core/TextureUtils.h>

#include <core/log.h>
#include <scene/SceneTypes.h> // for LoadedTexture

#include <filesystem>
#include <fstream>
#include <string>

bool compressTextures(
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
    cmdLine += "ECHO: \nWHERE nvtt_export \n";
    cmdLine += "IF %ERRORLEVEL% NEQ 0 (goto :error_tool)\n";
    cmdLine += "ECHO: \nECHO nvtt_export exists, proceeding with compression.\nECHO: \n";

    unsigned int i = 0;
    unsigned int totalCount = static_cast<unsigned int>(uncompressedTextures.size());
    for (auto it : uncompressedTextures)
    {
        auto texture = it.first;
        std::string inPath = texture->path;
        std::string outPath = std::filesystem::path(inPath).replace_extension(".dds").string();
        cmdLine += "ECHO converting texture " + std::to_string(++i) + " out of " + std::to_string(totalCount) + "\n";
        cmdLine += "nvtt_export -f 23 ";
        if (it.second == TextureCompressionType::Normalmap)
            cmdLine += " --no-mip-gamma-correct";
        else if (it.second == TextureCompressionType::GenericLinear)
            cmdLine += " --no-mip-gamma-correct";
        else
            cmdLine += " --mip-gamma-correct";
        cmdLine += " -o \"" + outPath + "\" \"" + inPath + "\"\n";
    }

    cmdLine += "ECHO:\npause\nECHO on\nexit /b 0\n:error_tool\n";
    cmdLine += "ECHO !! nvtt_export.exe not found !!\n";
    cmdLine += "ECHO Install NVIDIA Texture Tools Exporter and add to PATH.\n";
    cmdLine += "pause\nECHO on\nexit /b 1\n";

    batchFile << cmdLine;
    batchFile.close();

    std::string startCmd = " \"\" " + batchFileName;
    std::system(startCmd.c_str());
    return true;
}
