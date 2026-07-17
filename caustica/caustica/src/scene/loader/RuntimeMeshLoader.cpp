#include <scene/loader/RuntimeMeshLoader.h>

#include <scene/loader/GltfImporter.h>
#include <scene/loader/ObjImporter.h>
#include <scene/loader/CausUsdImporter.h>
#include <scene/loader/UrdfImporter.h>
#include <assets/loader/TextureLoader.h>
#include <core/log.h>
#include <core/vfs/VFS.h>
#include <scene/SceneImport.h>
#include <scene/SceneEcs.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace caustica
{

namespace
{

std::filesystem::path MakeAbsolutePath(const std::filesystem::path& filePath)
{
    return filePath.is_absolute() ? filePath : std::filesystem::absolute(filePath);
}

std::string LowercaseExtension(const std::filesystem::path& filePath)
{
    std::string ext = filePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

RuntimeMeshLoadResult FailedRuntimeMeshLoad(const std::filesystem::path& filePath)
{
    RuntimeMeshLoadResult result;
    result.SourcePath = filePath;
    return result;
}

} // namespace

RuntimeMeshLoadResult loadRuntimeMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath)
{
    if (!params.TextureCache || !params.SceneTypes)
    {
        caustica::error("Cannot load mesh: scene type factory or texture cache not initialized.");
        return FailedRuntimeMeshLoad(filePath);
    }

    const std::filesystem::path absPath = MakeAbsolutePath(filePath);
    if (!std::filesystem::exists(absPath))
    {
        caustica::error("File does not exist: '%s'", absPath.string().c_str());
        return FailedRuntimeMeshLoad(absPath);
    }

    const std::string ext = LowercaseExtension(absPath);
    if (ext == ".gltf" || ext == ".glb")
        return loadRuntimeGltfMeshFile(params, absPath);
    if (ext == ".obj")
        return loadRuntimeObjMeshFile(params, absPath);
    if (ext == ".urdf")
        return loadRuntimeUrdfMeshFile(params, absPath);
    if (ext == ".usd" || ext == ".usda" || ext == ".usdc")
    {
        if (!params.TextureCache || !params.SceneTypes)
            return FailedRuntimeMeshLoad(absPath);

        auto importer = std::make_shared<caustica::CausUsdImporter>(params.SceneTypes);
        caustica::SceneLoadingStats stats;
        auto importResult = std::make_shared<caustica::SceneImportResult>();
        if (!importer->load(absPath, *params.TextureCache, stats, nullptr, *importResult, params.TextureSearchDirectory))
        {
            caustica::error("CausUsdImporter failed to load '%s'", absPath.string().c_str());
            return FailedRuntimeMeshLoad(absPath);
        }
        if (!ecs::isValid(importResult->rootEntity) || !importResult->entityWorld)
        {
            caustica::error("USD import produced an empty scene: '%s'", absPath.string().c_str());
            return FailedRuntimeMeshLoad(absPath);
        }

        RuntimeMeshLoadResult result;
        result.Success = true;
        result.SourcePath = absPath;
        result.ImportResult = std::move(importResult);
        return result;
    }

    caustica::error("Unsupported mesh file type '%s'.", ext.c_str());
    return FailedRuntimeMeshLoad(absPath);
}

RuntimeMeshLoadResult loadRuntimeGltfMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath)
{
    if (!params.TextureCache || !params.SceneTypes)
        return FailedRuntimeMeshLoad(filePath);

    auto fs = std::make_shared<caustica::NativeFileSystem>();
    auto importer = std::make_shared<caustica::GltfImporter>(fs, params.SceneTypes);

    caustica::SceneLoadingStats stats;
    auto importResult = std::make_shared<caustica::SceneImportResult>();

    if (!importer->load(filePath, *params.TextureCache, stats, nullptr, *importResult, params.TextureSearchDirectory))
    {
        caustica::error("GltfImporter failed to load '%s'", filePath.string().c_str());
        return FailedRuntimeMeshLoad(filePath);
    }

    if (!ecs::isValid(importResult->rootEntity) || !importResult->entityWorld)
    {
        caustica::error("GltfImporter produced no root entity for '%s'", filePath.string().c_str());
        return FailedRuntimeMeshLoad(filePath);
    }

    if (auto* name = importResult->entityWorld->world().get<scene::NameComponent>(importResult->rootEntity))
        name->value = filePath.stem().string();

    return RuntimeMeshLoadResult{
        .Success = true,
        .SourcePath = filePath,
        .ImportResult = importResult,
    };
}

RuntimeMeshLoadResult loadRuntimeObjMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath)
{
    if (!params.TextureCache || !params.SceneTypes)
        return FailedRuntimeMeshLoad(filePath);

    caustica::ObjImporter importer(params.SceneTypes);

    caustica::SceneLoadingStats stats;
    auto importResult = std::make_shared<caustica::SceneImportResult>();
    if (!importer.load(filePath, *params.TextureCache, stats, nullptr, *importResult))
        return FailedRuntimeMeshLoad(filePath);

    if (!ecs::isValid(importResult->rootEntity) || !importResult->entityWorld)
        return FailedRuntimeMeshLoad(filePath);

    return RuntimeMeshLoadResult{
        .Success = true,
        .SourcePath = filePath,
        .ImportResult = importResult,
    };
}

RuntimeMeshLoadResult loadRuntimeUrdfMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath)
{
    if (!params.TextureCache || !params.SceneTypes)
        return FailedRuntimeMeshLoad(filePath);

    caustica::UrdfImporter importer(params.SceneTypes);

    caustica::SceneLoadingStats stats;
    auto importResult = std::make_shared<caustica::SceneImportResult>();
    if (!importer.load(filePath, *params.TextureCache, stats, nullptr, *importResult, params.TextureSearchDirectory))
    {
        caustica::error("UrdfImporter failed to load '%s'", filePath.string().c_str());
        return FailedRuntimeMeshLoad(filePath);
    }

    if (!ecs::isValid(importResult->rootEntity) || !importResult->entityWorld)
    {
        caustica::error("URDF import produced an empty scene: '%s'", filePath.string().c_str());
        return FailedRuntimeMeshLoad(filePath);
    }

    return RuntimeMeshLoadResult{
        .Success = true,
        .SourcePath = filePath,
        .ImportResult = importResult,
    };
}

} // namespace caustica
