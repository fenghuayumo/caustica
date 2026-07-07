#include <assets/AssetSystem.h>
#include <assets/loader/TextureLoader.h>
#include <core/ThreadPool.h>
#include <core/log.h>

#include <cstdint>
#include <sstream>

namespace caustica
{

namespace
{
    std::filesystem::path MakeTypedAssetPath(
        const std::filesystem::path& sourcePath,
        const char* typeName,
        const std::string& name,
        const void* pointer)
    {
        std::ostringstream stream;
        stream << (sourcePath.empty() ? std::string("__memory_asset__") : sourcePath.generic_string())
               << "::" << typeName << "::" << name << "::"
               << reinterpret_cast<std::uintptr_t>(pointer);
        return std::filesystem::path(stream.str());
    }
}

void AssetSystem::initialize(
    nvrhi::IDevice* device,
    std::shared_ptr<IFileSystem> fileSystem,
    std::shared_ptr<IDescriptorTableManager> descriptorTable)
{
    m_TextureLoader = std::make_shared<TextureLoader>(
        device,
        std::move(fileSystem),
        std::move(descriptorTable),
        m_Registry,
        m_Images);
    m_Initialized = true;
    caustica::info("AssetSystem initialized");
}

void AssetSystem::shutdown()
{
    m_ArtifactCache.clear();
    m_HotReload.clear();
    m_Dependencies.clear();
    m_Scenes.clear();
    m_Materials.clear();
    m_Meshes.clear();
    m_Images.clear();
    m_TextureLoader.reset();
    m_Initialized = false;
}

Handle<ImageAsset> AssetSystem::loadTextureFromFile(
    const std::filesystem::path& path,
    bool sRGB,
    render::RenderDevice* renderDevice,
    nvrhi::ICommandList* commandList)
{
    return m_TextureLoader->loadTextureFromFile(path, sRGB, renderDevice, commandList);
}

Handle<ImageAsset> AssetSystem::loadTextureFromFileDeferred(
    const std::filesystem::path& path,
    bool sRGB)
{
    return m_TextureLoader->loadTextureFromFileDeferred(path, sRGB);
}

Handle<ImageAsset> AssetSystem::loadTextureFromFileAsync(
    const std::filesystem::path& path,
    bool sRGB,
    ThreadPool& threadPool)
{
    return m_TextureLoader->loadTextureFromFileAsync(path, sRGB, threadPool);
}

Handle<ImageAsset> AssetSystem::loadTextureFromMemory(
    const std::shared_ptr<IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB,
    render::RenderDevice* renderDevice,
    nvrhi::ICommandList* commandList)
{
    return m_TextureLoader->loadTextureFromMemory(data, name, mimeType, sRGB, renderDevice, commandList);
}

Handle<ImageAsset> AssetSystem::loadTextureFromMemoryDeferred(
    const std::shared_ptr<IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB)
{
    return m_TextureLoader->loadTextureFromMemoryDeferred(data, name, mimeType, sRGB);
}

Handle<ImageAsset> AssetSystem::loadTextureFromMemoryAsync(
    const std::shared_ptr<IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB,
    ThreadPool& threadPool)
{
    return m_TextureLoader->loadTextureFromMemoryAsync(data, name, mimeType, sRGB, threadPool);
}

std::shared_ptr<ImageAsset> AssetSystem::getLoadedTexture(const std::filesystem::path& path)
{
    return m_TextureLoader->getLoadedTexture(path);
}

bool AssetSystem::unloadTexture(const Handle<ImageAsset>& texture)
{
    return m_TextureLoader->unloadTexture(texture);
}

Handle<MeshAsset> AssetSystem::registerMeshAsset(
    const std::shared_ptr<MeshInfo>& mesh,
    const std::filesystem::path& sourcePath,
    const std::string& name)
{
    if (!mesh)
        return {};

    const std::filesystem::path assetPath = MakeTypedAssetPath(sourcePath, "mesh", name, mesh.get());
    AssetId id = m_Registry.registerAsset(assetPath, AssetType::Mesh);

    auto asset = std::make_shared<MeshAsset>();
    asset->id = id;
    asset->name = name;
    asset->sourcePath = sourcePath;
    asset->mesh = mesh;

    if (!sourcePath.empty() && std::filesystem::exists(sourcePath))
        m_HotReload.watch(id, sourcePath);

    return m_Meshes.insert(id, std::move(asset));
}

Handle<MaterialAsset> AssetSystem::registerMaterialAsset(
    const std::shared_ptr<Material>& material,
    const std::filesystem::path& sourcePath,
    const std::string& name)
{
    if (!material)
        return {};

    const std::filesystem::path assetPath = MakeTypedAssetPath(sourcePath, "material", name, material.get());
    AssetId id = m_Registry.registerAsset(assetPath, AssetType::Material);

    auto asset = std::make_shared<MaterialAsset>();
    asset->id = id;
    asset->name = name;
    asset->sourcePath = sourcePath;
    asset->material = material;

    if (!sourcePath.empty() && std::filesystem::exists(sourcePath))
        m_HotReload.watch(id, sourcePath);

    return m_Materials.insert(id, std::move(asset));
}

Handle<SceneAsset> AssetSystem::registerSceneAsset(
    const std::shared_ptr<Scene>& scene,
    const std::filesystem::path& sourcePath,
    const std::string& name)
{
    if (!scene)
        return {};

    const std::filesystem::path assetPath = MakeTypedAssetPath(sourcePath, "scene", name, scene.get());
    AssetId id = m_Registry.registerAsset(assetPath, AssetType::Scene);

    auto asset = std::make_shared<SceneAsset>();
    asset->id = id;
    asset->name = name;
    asset->sourcePath = sourcePath;
    asset->scene = scene;

    if (!sourcePath.empty() && std::filesystem::exists(sourcePath))
        m_HotReload.watch(id, sourcePath);

    return m_Scenes.insert(id, std::move(asset));
}

void AssetSystem::clearSceneAssets()
{
    m_HotReload.clear();
    m_Dependencies.clear();
    m_Scenes.clear();
    m_Materials.clear();
    m_Meshes.clear();
}

void AssetSystem::addDependency(AssetId asset, AssetId dependency)
{
    m_Dependencies.addDependency(asset, dependency);
}

std::vector<HotReloadChange> AssetSystem::pollHotReloadChanges()
{
    return m_HotReload.pollChangedFiles();
}

bool AssetSystem::processRenderingThreadCommands(render::RenderDevice& renderDevice, float timeLimitMilliseconds)
{
    return m_TextureLoader->processRenderingThreadCommands(renderDevice, timeLimitMilliseconds);
}

void AssetSystem::loadingFinished()
{
    m_TextureLoader->loadingFinished();
}

} // namespace caustica
