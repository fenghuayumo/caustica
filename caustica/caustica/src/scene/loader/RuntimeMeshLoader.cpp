#include <scene/loader/RuntimeMeshLoader.h>

#include <scene/loader/GltfImporter.h>
#include <scene/loader/ObjImporter.h>
#include <assets/loader/TextureLoader.h>
#include <core/log.h>
#include <core/vfs/VFS.h>
#include <scene/SceneGraph.h>

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

RuntimeMeshLoadResult LoadRuntimeMeshFile(
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
        return LoadRuntimeGltfMeshFile(params, absPath);
    if (ext == ".obj")
        return LoadRuntimeObjMeshFile(params, absPath);

    caustica::error("Unsupported mesh file type '%s'.", ext.c_str());
    return FailedRuntimeMeshLoad(absPath);
}

RuntimeMeshLoadResult LoadRuntimeGltfMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath)
{
    if (!params.TextureCache || !params.SceneTypes)
        return FailedRuntimeMeshLoad(filePath);

    auto fs = std::make_shared<caustica::NativeFileSystem>();
    auto importer = std::make_shared<caustica::GltfImporter>(fs, params.SceneTypes);

    caustica::SceneLoadingStats stats;
    auto importResult = std::make_shared<caustica::SceneImportResult>();

    if (!importer->Load(filePath, *params.TextureCache, stats, nullptr, *importResult, params.TextureSearchDirectory))
    {
        caustica::error("GltfImporter failed to load '%s'", filePath.string().c_str());
        return FailedRuntimeMeshLoad(filePath);
    }

    if (!importResult->rootNode)
    {
        caustica::error("GltfImporter produced no root node for '%s'", filePath.string().c_str());
        return FailedRuntimeMeshLoad(filePath);
    }

    importResult->rootNode->SetName(filePath.stem().string());

    return RuntimeMeshLoadResult{
        .Success = true,
        .SourcePath = filePath,
        .ImportResult = importResult,
    };
}

RuntimeMeshLoadResult LoadRuntimeObjMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath)
{
    if (!params.TextureCache || !params.SceneTypes)
        return FailedRuntimeMeshLoad(filePath);

    caustica::ObjImporter importer(params.SceneTypes);

    caustica::SceneLoadingStats stats;
    auto importResult = std::make_shared<caustica::SceneImportResult>();
    if (!importer.Load(filePath, *params.TextureCache, stats, nullptr, *importResult))
        return FailedRuntimeMeshLoad(filePath);

    if (!importResult->rootNode)
        return FailedRuntimeMeshLoad(filePath);

    return RuntimeMeshLoadResult{
        .Success = true,
        .SourcePath = filePath,
        .ImportResult = importResult,
    };
}

} // namespace caustica
