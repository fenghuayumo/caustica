#include <assets/AssetSystem.h>
#include <assets/loader/TextureLoader.h>
#include <scene/loader/RuntimeMeshLoader.h>

namespace caustica
{

RuntimeMeshLoadResult AssetSystem::LoadRuntimeMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& path)
{
    AssetId id = RegisterMesh(path);
    m_Registry.SetState(id, AssetState::Loading);

    RuntimeMeshLoadResult result = ::caustica::LoadRuntimeMeshFile(params, path);
    m_Registry.SetState(id, result ? AssetState::Loaded : AssetState::Failed);
    return result;
}

RuntimeMeshLoadResult AssetSystem::LoadRuntimeGltfMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& path)
{
    AssetId id = RegisterMesh(path);
    m_Registry.SetState(id, AssetState::Loading);

    RuntimeMeshLoadResult result = ::caustica::LoadRuntimeGltfMeshFile(params, path);
    m_Registry.SetState(id, result ? AssetState::Loaded : AssetState::Failed);
    return result;
}

RuntimeMeshLoadResult AssetSystem::LoadRuntimeObjMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& path)
{
    AssetId id = RegisterMesh(path);
    m_Registry.SetState(id, AssetState::Loading);

    RuntimeMeshLoadResult result = ::caustica::LoadRuntimeObjMeshFile(params, path);
    m_Registry.SetState(id, result ? AssetState::Loaded : AssetState::Failed);
    return result;
}

} // namespace caustica
