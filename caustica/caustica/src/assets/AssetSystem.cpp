#include <assets/AssetSystem.h>
#include <assets/loader/TextureLoader.h>
#include <core/PathUtils.h>
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
        // Canonicalize the path component so the same file referenced with
        // different spellings (case, separators, ..) maps to one key. The
        // pointer suffix keeps session-local registrations distinct; durable
        // cross-session AssetIds are tracked separately.
        stream << (sourcePath.empty() ? std::string("__memory_asset__") : canonicalAssetKey(sourcePath))
               << "::" << typeName << "::" << name << "::"
               << reinterpret_cast<std::uintptr_t>(pointer);
        return std::filesystem::path(stream.str());
    }
}

void AssetSystem::initialize(
    caustica::rhi::Device* device,
    std::shared_ptr<IFileSystem> fileSystem,
    std::shared_ptr<IDescriptorTableManager> descriptorTable)
{
    m_textureLoader = std::make_shared<TextureLoader>(
        device,
        std::move(fileSystem),
        std::move(descriptorTable),
        m_registry,
        m_images);
    m_initialized = true;
    caustica::info("AssetSystem initialized");
}

void AssetSystem::shutdown()
{
    if (!m_initialized)
        return;

    // Detach before clearing stores / dropping the shared_ptr so any leftover
    // Scene/shared holders cannot UAF m_registry/m_images in ~TextureLoader.
    if (m_textureLoader)
        m_textureLoader->detachFromStores();

    m_artifactCache.clear();
    m_hotReload.clear();
    m_dependencies.clear();
    m_scenes.clear();
    m_prefabs.clear();
    m_materials.clear();
    m_meshes.clear();
    m_images.clear();
    m_textureLoader.reset();
    m_initialized = false;
}

Handle<ImageAsset> AssetSystem::loadTextureFromFile(
    const std::filesystem::path& path,
    bool sRGB,
    render::RenderDevice* renderDevice,
    caustica::rhi::CommandList* commandList)
{
    return m_textureLoader->loadTextureFromFile(path, sRGB, renderDevice, commandList);
}

Handle<ImageAsset> AssetSystem::loadTextureFromFileDeferred(
    const std::filesystem::path& path,
    bool sRGB)
{
    return m_textureLoader->loadTextureFromFileDeferred(path, sRGB);
}

Handle<ImageAsset> AssetSystem::loadTextureFromFileAsync(
    const std::filesystem::path& path,
    bool sRGB)
{
    return m_textureLoader->loadTextureFromFileAsync(path, sRGB);
}

Handle<ImageAsset> AssetSystem::loadTextureFromMemory(
    const std::shared_ptr<IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB,
    render::RenderDevice* renderDevice,
    caustica::rhi::CommandList* commandList)
{
    return m_textureLoader->loadTextureFromMemory(data, name, mimeType, sRGB, renderDevice, commandList);
}

Handle<ImageAsset> AssetSystem::loadTextureFromMemoryDeferred(
    const std::shared_ptr<IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB)
{
    return m_textureLoader->loadTextureFromMemoryDeferred(data, name, mimeType, sRGB);
}

Handle<ImageAsset> AssetSystem::loadTextureFromMemoryAsync(
    const std::shared_ptr<IBlob>& data,
    const std::string& name,
    const std::string& mimeType,
    bool sRGB)
{
    return m_textureLoader->loadTextureFromMemoryAsync(data, name, mimeType, sRGB);
}

std::shared_ptr<ImageAsset> AssetSystem::getLoadedTexture(const std::filesystem::path& path)
{
    return m_textureLoader->getLoadedTexture(path);
}

bool AssetSystem::unloadTexture(const Handle<ImageAsset>& texture)
{
    return m_textureLoader->unloadTexture(texture);
}

Handle<MeshAsset> AssetSystem::registerMeshAsset(
    const std::shared_ptr<MeshInfo>& mesh,
    const std::filesystem::path& sourcePath,
    const std::string& name)
{
    if (!mesh)
        return {};

    const std::filesystem::path assetPath = MakeTypedAssetPath(sourcePath, "mesh", name, mesh.get());
    AssetId id = m_registry.registerAsset(assetPath, AssetType::Mesh);

    auto asset = std::make_shared<MeshAsset>();
    asset->id = id;
    asset->name = name;
    asset->sourcePath = sourcePath;
    asset->mesh = mesh;

    if (!sourcePath.empty() && std::filesystem::exists(sourcePath))
        m_hotReload.watch(id, sourcePath);

    return m_meshes.insert(id, std::move(asset));
}

Handle<MaterialAsset> AssetSystem::registerMaterialAsset(
    const std::shared_ptr<Material>& material,
    const std::filesystem::path& sourcePath,
    const std::string& name)
{
    if (!material)
        return {};

    const std::filesystem::path assetPath = MakeTypedAssetPath(sourcePath, "material", name, material.get());
    AssetId id = m_registry.registerAsset(assetPath, AssetType::Material);

    auto asset = std::make_shared<MaterialAsset>();
    asset->id = id;
    asset->name = name;
    asset->sourcePath = sourcePath;
    asset->material = material;

    if (!sourcePath.empty() && std::filesystem::exists(sourcePath))
        m_hotReload.watch(id, sourcePath);

    return m_materials.insert(id, std::move(asset));
}

Handle<SceneAsset> AssetSystem::registerSceneAsset(
    const std::shared_ptr<Scene>& scene,
    const std::filesystem::path& sourcePath,
    const std::string& name)
{
    if (!scene)
        return {};

    const std::filesystem::path assetPath = MakeTypedAssetPath(sourcePath, "scene", name, scene.get());
    AssetId id = m_registry.registerAsset(assetPath, AssetType::Scene);

    auto asset = std::make_shared<SceneAsset>();
    asset->id = id;
    asset->name = name;
    asset->sourcePath = sourcePath;
    asset->scene = scene;

    if (!sourcePath.empty() && std::filesystem::exists(sourcePath))
        m_hotReload.watch(id, sourcePath);

    return m_scenes.insert(id, std::move(asset));
}

Handle<ScenePrefabAsset> AssetSystem::registerScenePrefab(
    const std::shared_ptr<SceneImportResult>& importResult,
    const std::filesystem::path& sourcePath,
    const std::string& name)
{
    if (!importResult)
        return {};

    if (Handle<ScenePrefabAsset> existing = findScenePrefab(sourcePath))
        return existing;

    const std::filesystem::path assetPath = sourcePath.empty()
        ? MakeTypedAssetPath({}, "prefab", name, importResult.get())
        : sourcePath;
    AssetId id = m_registry.registerAsset(assetPath, AssetType::Prefab);

    auto asset = std::make_shared<ScenePrefabAsset>();
    asset->id = id;
    asset->name = name.empty() ? assetPath.stem().string() : name;
    asset->sourcePath = sourcePath;
    asset->import = importResult;

    if (!sourcePath.empty() && std::filesystem::exists(sourcePath))
        m_hotReload.watch(id, sourcePath);

    m_registry.setState(id, AssetState::Loaded);
    return m_prefabs.insert(id, std::move(asset));
}

Handle<ScenePrefabAsset> AssetSystem::findScenePrefab(const std::filesystem::path& sourcePath) const
{
    if (sourcePath.empty())
        return {};
    const AssetId id = m_registry.findByPath(sourcePath);
    if (!id)
        return {};
    return m_prefabs.handle(id);
}

void AssetSystem::clearSceneAssets()
{
    m_hotReload.clear();
    m_dependencies.clear();
    m_scenes.clear();
    m_prefabs.clear();
    m_materials.clear();
    m_meshes.clear();
}

void AssetSystem::addDependency(AssetId asset, AssetId dependency)
{
    m_dependencies.addDependency(asset, dependency);
}

std::vector<HotReloadChange> AssetSystem::pollHotReloadChanges()
{
    return m_hotReload.pollChangedFiles();
}

bool AssetSystem::processRenderingThreadCommands(render::RenderDevice& renderDevice, float timeLimitMilliseconds)
{
    return m_textureLoader->processRenderingThreadCommands(renderDevice, timeLimitMilliseconds);
}

void AssetSystem::loadingFinished()
{
    m_textureLoader->loadingFinished();
}

} // namespace caustica
