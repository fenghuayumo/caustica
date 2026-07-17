#include <scene/loader/CausUsdImporter.h>
#include "CausUsdOpenUsd.h"

#include <scene/SceneImport.h>
#include <core/log.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <new>
#include <string>

namespace caustica
{

CausUsdImporter::CausUsdImporter(std::shared_ptr<SceneTypeFactory> sceneTypeFactory)
    : m_SceneTypeFactory(std::move(sceneTypeFactory))
{
}

bool CausUsdImporter::load(
    const std::filesystem::path& fileName,
    TextureLoader& /*textureCache*/,
    SceneLoadingStats& /*stats*/,
    ThreadPool* /*threadPool*/,
    SceneImportResult& result,
    const std::filesystem::path& /*sceneDirectory*/) const
{
    try
    {
        std::string ext = fileName.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });

        if (ext != ".usd" && ext != ".usda" && ext != ".usdc")
        {
            caustica::error("Unsupported USD extension '%s' for '%s'", ext.c_str(), fileName.string().c_str());
            return false;
        }

#if CAUSTICA_WITH_OPENUSD
        std::string error;
        if (!LoadSceneFromOpenUsd(fileName, *m_SceneTypeFactory, result, &error))
        {
            caustica::error("OpenUSD load failed for '%s': %s", fileName.string().c_str(), error.c_str());
            return false;
        }
        return true;
#else
        caustica::error(
            "OpenUSD C++ support is disabled; cannot load '%s'. "
            "Configure with CAUSTICA_WITH_OPENUSD=ON and a valid CAUSTICA_USD_ROOT.",
            fileName.string().c_str());
        return false;
#endif
    }
    catch (const std::bad_alloc& ex)
    {
        caustica::error(
            "CausUsdImporter ran out of memory loading '%s' (%s).",
            fileName.string().c_str(),
            ex.what());
        return false;
    }
    catch (const std::exception& ex)
    {
        caustica::error("CausUsdImporter failed: %s", ex.what());
        return false;
    }
}

} // namespace caustica
