#include <scene/Scene.h>
#include <scene/SceneImport.h>
#include <scene/SceneRenderExtract.h>
#include <scene/SceneComponentBuilders.h>
#include <scene/SceneLightAccess.h>
#include <scene/SceneObjects.h>
#include <scene/SceneAnimation.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/loader/GltfImporter.h>
#include <scene/loader/ObjImporter.h>
#include <scene/loader/CausUsdImporter.h>
#include <scene/loader/UrdfImporter.h>
#include <scene/scene_utils.h>
#include <core/ThreadContext.h>
#include <core/json.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/string_utils.h>
#include <cassert>
#include <rhi/common/misc.h>
#include <json/json.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <variant>

using namespace caustica::math;
#include <shaders/bindless.h>

using namespace caustica;
using namespace caustica;

static SceneLoadingStats g_LoadingStats;

namespace
{
    std::string TrimCopy(const std::string& value)
    {
        const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
            return std::isspace(ch);
        }).base();
        if (begin >= end)
            return {};
        return std::string(begin, end);
    }

    std::string ToLowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return char(std::tolower(ch));
        });
        return value;
    }

    bool IsBuiltinModelReference(const std::string& modelName)
    {
        return ToLowerCopy(TrimCopy(modelName)).rfind("builtin:", 0) == 0;
    }

    std::string NormalizeBuiltinModelName(std::string modelName)
    {
        modelName = ToLowerCopy(TrimCopy(modelName));
        constexpr const char* prefix = "builtin:";
        if (modelName.rfind(prefix, 0) == 0)
            modelName.erase(0, std::strlen(prefix));

        for (char& ch : modelName)
        {
            if (ch == '-' || ch == ' ')
                ch = '_';
        }

        return modelName;
    }

    struct BuiltinVertex
    {
        float3 position;
        float3 normal;
        float4 tangent;
        float2 texcoord;
    };

    struct BuiltinMeshData
    {
        std::string name;
        std::string materialName;
        float3 baseColor = 1.0f;
        std::vector<BuiltinVertex> vertices;
        std::vector<uint32_t> indices;
    };

    void AddVertex(BuiltinMeshData& mesh, const float3& position, const float3& normal, const float4& tangent, const float2& texcoord)
    {
        mesh.vertices.push_back({ position, normal, tangent, texcoord });
    }

    void AddQuad(
        BuiltinMeshData& mesh,
        const std::array<float3, 4>& positions,
        const float3& normal,
        const float4& tangent)
    {
        const uint32_t base = uint32_t(mesh.vertices.size());
        AddVertex(mesh, positions[0], normal, tangent, float2(0.0f, 0.0f));
        AddVertex(mesh, positions[1], normal, tangent, float2(0.0f, 1.0f));
        AddVertex(mesh, positions[2], normal, tangent, float2(1.0f, 1.0f));
        AddVertex(mesh, positions[3], normal, tangent, float2(1.0f, 0.0f));
        mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }

    BuiltinMeshData MakePlaneMesh()
    {
        BuiltinMeshData mesh;
        mesh.name = "BuiltinPlane";
        mesh.materialName = "Mat_BuiltinPlane";
        mesh.baseColor = float3(0.72f, 0.72f, 0.66f);
        AddQuad(mesh, {
            float3(-3.0f, 0.0f, -3.0f),
            float3(-3.0f, 0.0f,  3.0f),
            float3( 3.0f, 0.0f,  3.0f),
            float3( 3.0f, 0.0f, -3.0f),
        }, float3(0.0f, 1.0f, 0.0f), float4(1.0f, 0.0f, 0.0f, 1.0f));
        return mesh;
    }

    BuiltinMeshData MakeCubeMesh()
    {
        BuiltinMeshData mesh;
        mesh.name = "BuiltinCube";
        mesh.materialName = "Mat_BuiltinCube";
        mesh.baseColor = float3(0.1f, 0.42f, 0.95f);

        AddQuad(mesh, { float3(-0.5f, 1.0f, -0.5f), float3(-0.5f, 1.0f,  0.5f), float3( 0.5f, 1.0f,  0.5f), float3( 0.5f, 1.0f, -0.5f) }, float3( 0.0f,  1.0f,  0.0f), float4(1.0f, 0.0f, 0.0f, 1.0f));
        AddQuad(mesh, { float3(-0.5f, 0.0f, -0.5f), float3( 0.5f, 0.0f, -0.5f), float3( 0.5f, 0.0f,  0.5f), float3(-0.5f, 0.0f,  0.5f) }, float3( 0.0f, -1.0f,  0.0f), float4(1.0f, 0.0f, 0.0f, 1.0f));
        AddQuad(mesh, { float3(-0.5f, 0.0f,  0.5f), float3( 0.5f, 0.0f,  0.5f), float3( 0.5f, 1.0f,  0.5f), float3(-0.5f, 1.0f,  0.5f) }, float3( 0.0f,  0.0f,  1.0f), float4(1.0f, 0.0f, 0.0f, 1.0f));
        AddQuad(mesh, { float3(-0.5f, 0.0f, -0.5f), float3(-0.5f, 1.0f, -0.5f), float3( 0.5f, 1.0f, -0.5f), float3( 0.5f, 0.0f, -0.5f) }, float3( 0.0f,  0.0f, -1.0f), float4(1.0f, 0.0f, 0.0f, 1.0f));
        AddQuad(mesh, { float3( 0.5f, 0.0f, -0.5f), float3( 0.5f, 1.0f, -0.5f), float3( 0.5f, 1.0f,  0.5f), float3( 0.5f, 0.0f,  0.5f) }, float3( 1.0f,  0.0f,  0.0f), float4(0.0f, 0.0f, 1.0f, 1.0f));
        AddQuad(mesh, { float3(-0.5f, 0.0f, -0.5f), float3(-0.5f, 0.0f,  0.5f), float3(-0.5f, 1.0f,  0.5f), float3(-0.5f, 1.0f, -0.5f) }, float3(-1.0f,  0.0f,  0.0f), float4(0.0f, 0.0f, 1.0f, 1.0f));

        return mesh;
    }

    BuiltinMeshData MakeSphereMesh()
    {
        constexpr float pi = 3.14159265358979323846f;
        constexpr int rings = 16;
        constexpr int segments = 32;
        constexpr float radius = 0.5f;

        BuiltinMeshData mesh;
        mesh.name = "BuiltinSphere";
        mesh.materialName = "Mat_BuiltinSphere";
        mesh.baseColor = float3(0.95f, 0.3f, 0.18f);

        for (int ring = 0; ring <= rings; ++ring)
        {
            const float v = float(ring) / float(rings);
            const float theta = v * pi;
            const float sinTheta = std::sin(theta);
            const float cosTheta = std::cos(theta);

            for (int segment = 0; segment <= segments; ++segment)
            {
                const float u = float(segment) / float(segments);
                const float phi = u * 2.0f * pi;
                const float sinPhi = std::sin(phi);
                const float cosPhi = std::cos(phi);
                const float3 normal = float3(sinTheta * cosPhi, cosTheta, sinTheta * sinPhi);
                const float3 tangent = normalize(float3(-sinPhi, 0.0f, cosPhi));
                AddVertex(mesh, normal * radius + float3(0.0f, radius, 0.0f), normal, float4(tangent, 1.0f), float2(u, v));
            }
        }

        for (int ring = 0; ring < rings; ++ring)
        {
            for (int segment = 0; segment < segments; ++segment)
            {
                const uint32_t a = uint32_t(ring * (segments + 1) + segment);
                const uint32_t b = uint32_t((ring + 1) * (segments + 1) + segment);
                const uint32_t c = b + 1;
                const uint32_t d = a + 1;
                mesh.indices.insert(mesh.indices.end(), { a, b, d, d, b, c });
            }
        }

        return mesh;
    }

    std::vector<BuiltinMeshData> MakeBuiltinMeshes(const std::string& builtinName)
    {
        const std::string name = NormalizeBuiltinModelName(builtinName);
        if (name == "plane")
            return { MakePlaneMesh() };
        if (name == "cube")
            return { MakeCubeMesh() };
        if (name == "sphere")
            return { MakeSphereMesh() };
        if (name == "plane_cube" || name == "default" || name == "default_scene")
            return { MakePlaneMesh(), MakeCubeMesh() };

        caustica::error("Unknown builtin primitive model '%s'", builtinName.c_str());
        return {};
    }

    void AppendMeshToBuffers(const BuiltinMeshData& src, BufferGroup& buffers)
    {
        for (const BuiltinVertex& vertex : src.vertices)
        {
            buffers.positionData.push_back(vertex.position);
            buffers.normalData.push_back(vectorToSnorm8(vertex.normal));
            buffers.tangentData.push_back(vectorToSnorm8(vertex.tangent));
            buffers.texcoord1Data.push_back(vertex.texcoord);
        }

        for (uint32_t index : src.indices)
            buffers.indexData.push_back(index);
    }
}

const SceneLoadingStats& Scene::getLoadingStats()
{
    return g_LoadingStats;
}

box3 Scene::getSceneBounds() const
{
    assertLogicThread();

    if (!m_EntityWorld || !ecs::isValid(m_EntityWorld->root()))
        return box3::empty();

    const auto* bounds = m_EntityWorld->world().get<scene::BoundsComponent>(m_EntityWorld->root());
    if (!bounds)
        return box3::empty();

    const box3& globalBounds = bounds->globalBounds;
    const bool finite = dm::all(dm::isfinite(globalBounds.m_mins)) && dm::all(dm::isfinite(globalBounds.m_maxs));
    return finite ? globalBounds : box3::empty();
}

void Scene::prepareForUnload()
{
    assertLogicThread();

    m_LogicExtractCache.clear();
    m_LogicExtractCacheValid = false;
    m_RenderSnapshot.clear();
    m_Models.clear();

    if (m_EntityWorld)
    {
        auto releaseMeshCpu = [](const std::shared_ptr<MeshInfo>& mesh) {
            if (!mesh)
                return;
            mesh->asset = nullptr;
            mesh->buffers.reset();
        };

        for (const std::shared_ptr<MeshInfo>& mesh : m_EntityWorld->getMeshes())
        {
            releaseMeshCpu(mesh);
            if (mesh)
                releaseMeshCpu(mesh->skinPrototype);
        }
        for (const std::shared_ptr<Material>& material : m_EntityWorld->getMaterials())
        {
            if (material)
                material->asset = nullptr;
        }
    }

    m_Asset = nullptr;
}

const ResourceTracker<Material>& Scene::getMaterials() const
{
    assertLogicThread();

    if (m_EntityWorld)
    {
        m_EntityWorld->ensureSceneResourcesSynced();
        return m_EntityWorld->getMaterials();
    }

    static const ResourceTracker<Material> s_empty;
    return s_empty;
}

const ResourceTracker<MeshInfo>& Scene::getMeshes() const
{
    assertLogicThread();

    if (m_EntityWorld)
    {
        m_EntityWorld->ensureSceneResourcesSynced();
        return m_EntityWorld->getMeshes();
    }

    static const ResourceTracker<MeshInfo> s_empty;
    return s_empty;
}

size_t Scene::getGeometryCount() const
{
    assertLogicThread();
    return m_EntityWorld ? m_EntityWorld->getGeometryCount() : 0;
}

size_t Scene::getMaxGeometryCountPerMesh() const
{
    assertLogicThread();
    return m_EntityWorld ? m_EntityWorld->getMaxGeometryCountPerMesh() : 0;
}

size_t Scene::getGeometryInstancesCount() const
{
    assertLogicThread();
    return m_EntityWorld ? m_EntityWorld->getGeometryInstancesCount() : 0;
}

namespace
{
const scene::SceneRenderData* publishedEntityListSource(const scene::SceneRenderSnapshot& snapshot,
    uint32_t gpuFrameIndex)
{
    if (gpuFrameIndex != UINT32_MAX)
        return &snapshot.readBufferForFrame(gpuFrameIndex);

    // After endGpuReadFrame (editor pick / UI), use the last published Extract list so
    // instance indices match the frame that produced the pick feedback.
    const uint32_t latest = snapshot.latestExtractedFrameIndex();
    if (latest != UINT32_MAX)
        return &snapshot.readBufferForFrame(latest);

    return nullptr;
}
} // namespace

const std::vector<ecs::Entity>& Scene::getMeshInstances() const
{
    if (const auto* published = publishedEntityListSource(
            m_RenderSnapshot, m_gpuReadFrameIndex.load(std::memory_order_acquire)))
    {
        return published->meshInstanceEntities;
    }

    assertLogicThread();
    m_LogicQueryMeshInstances.clear();
    if (m_EntityWorld)
    {
        // Match Extract / refreshInstanceIndices: entity-id order for pick/index lookups.
        m_EntityWorld->world().each<scene::MeshInstanceComponent, scene::GlobalTransformComponent,
            scene::BoundsComponent, scene::SceneContentComponent>(
            [&](ecs::Entity entity, scene::MeshInstanceComponent&, scene::GlobalTransformComponent&,
                scene::BoundsComponent&, scene::SceneContentComponent&)
            {
                m_LogicQueryMeshInstances.push_back(entity);
            });
        std::sort(m_LogicQueryMeshInstances.begin(), m_LogicQueryMeshInstances.end(),
            [](ecs::Entity a, ecs::Entity b) {
                return static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
            });
    }
    return m_LogicQueryMeshInstances;
}

const std::vector<ecs::Entity>& Scene::getSkinnedMeshInstances() const
{
    if (const auto* published = publishedEntityListSource(
            m_RenderSnapshot, m_gpuReadFrameIndex.load(std::memory_order_acquire)))
    {
        return published->skinnedMeshInstanceEntities;
    }

    assertLogicThread();
    m_LogicQuerySkinnedMeshInstances.clear();
    if (m_EntityWorld)
    {
        // Match ExtractSkinnedMeshes: EnTT iteration order (no sort).
        m_EntityWorld->world().each<scene::SkinnedMeshComponent, scene::MeshInstanceComponent,
            scene::GlobalTransformComponent>(
            [&](ecs::Entity entity, scene::SkinnedMeshComponent&, scene::MeshInstanceComponent&,
                scene::GlobalTransformComponent&)
            {
                m_LogicQuerySkinnedMeshInstances.push_back(entity);
            });
    }
    return m_LogicQuerySkinnedMeshInstances;
}

const std::vector<ecs::Entity>& Scene::getLightEntities() const
{
    if (const auto* published = publishedEntityListSource(
            m_RenderSnapshot, m_gpuReadFrameIndex.load(std::memory_order_acquire)))
    {
        return published->lightEntities;
    }

    assertLogicThread();
    m_LogicQueryLightEntities.clear();
    if (m_EntityWorld)
    {
        // Match ExtractLightsFull: lights without GlobalTransform are not renderable.
        auto& world = m_EntityWorld->world();
        world.each<scene::DirectionalLightComponent, scene::GlobalTransformComponent>(
            [&](ecs::Entity entity, scene::DirectionalLightComponent&, scene::GlobalTransformComponent&) {
                m_LogicQueryLightEntities.push_back(entity);
            });
        world.each<scene::SpotLightComponent, scene::GlobalTransformComponent>(
            [&](ecs::Entity entity, scene::SpotLightComponent&, scene::GlobalTransformComponent&) {
                m_LogicQueryLightEntities.push_back(entity);
            });
        world.each<scene::PointLightComponent, scene::GlobalTransformComponent>(
            [&](ecs::Entity entity, scene::PointLightComponent&, scene::GlobalTransformComponent&) {
                m_LogicQueryLightEntities.push_back(entity);
            });
        world.each<scene::EnvironmentLightComponent, scene::GlobalTransformComponent>(
            [&](ecs::Entity entity, scene::EnvironmentLightComponent&, scene::GlobalTransformComponent&) {
                m_LogicQueryLightEntities.push_back(entity);
            });
    }
    return m_LogicQueryLightEntities;
}

const std::vector<ecs::Entity>& Scene::getCameraEntities() const
{
    if (const auto* published = publishedEntityListSource(
            m_RenderSnapshot, m_gpuReadFrameIndex.load(std::memory_order_acquire)))
    {
        return published->cameraEntities;
    }

    assertLogicThread();
    if (!m_EntityWorld)
    {
        static const std::vector<ecs::Entity> empty;
        return empty;
    }
    return m_EntityWorld->cameraEntitiesInRegistrationOrder();
}

const std::vector<ecs::Entity>& Scene::getAnimationEntities() const
{
    if (const auto* published = publishedEntityListSource(
            m_RenderSnapshot, m_gpuReadFrameIndex.load(std::memory_order_acquire)))
    {
        return published->animationEntities;
    }

    assertLogicThread();
    m_LogicQueryAnimationEntities.clear();
    if (m_EntityWorld)
    {
        m_EntityWorld->world().each<scene::AnimationComponent>(
            [&](ecs::Entity entity, scene::AnimationComponent&) {
                m_LogicQueryAnimationEntities.push_back(entity);
            });
    }
    return m_LogicQueryAnimationEntities;
}

const scene::SceneRenderData& Scene::getRenderData() const
{
    return getRenderSnapshotForRead();
}

const scene::SceneRenderData& Scene::getRenderDataForFrame(uint32_t frameIndex) const
{
    return m_RenderSnapshot.readBufferForFrame(frameIndex);
}

const scene::SceneRenderData& Scene::getRenderSnapshotForRead() const
{
    const uint32_t gpuFrameIndex = m_gpuReadFrameIndex.load(std::memory_order_acquire);
    assert(gpuFrameIndex != UINT32_MAX &&
        "Scene::getRenderData requires beginGpuReadFrame, or use getRenderDataForFrame");
    return m_RenderSnapshot.readBufferForFrameOrLatest(gpuFrameIndex);
}

void Scene::beginGpuReadFrame(uint32_t frameIndex)
{
    m_gpuReadFrameIndex.store(frameIndex, std::memory_order_release);
}

void Scene::endGpuReadFrame()
{
    m_gpuReadFrameIndex.store(UINT32_MAX, std::memory_order_release);
}

bool Scene::hasSceneTransformsChanged(uint32_t frameIndex) const
{
    return m_RenderSnapshot.publishedStateForFrame(frameIndex).transformsChanged;
}

bool Scene::hasSceneLightsChanged(uint32_t frameIndex) const
{
    return m_RenderSnapshot.publishedStateForFrame(frameIndex).lightsChanged;
}

bool Scene::hasSceneStructureChanged(uint32_t frameIndex) const
{
    const scene::SceneRenderPublishState& state = m_RenderSnapshot.publishedStateForFrame(frameIndex);
    return state.structureGeneration > m_structureGpu.consumedGeneration();
}

void Scene::acknowledgeGpuStructureConsumed(uint32_t frameIndex)
{
    const uint64_t generation =
        m_RenderSnapshot.publishedStateForFrame(frameIndex).structureGeneration;
    m_structureGpu.acknowledgeGpuStructureConsumed(generation);
}

void Scene::freezeCommittedFromLogicCache()
{
    if (!m_LogicExtractCacheValid)
        return;
    m_structureGpu.freezeCommittedFromLogicCache(m_LogicExtractCache);
}

void Scene::finishStructureGpuBuild(
    uint32_t frameIndex, std::shared_ptr<const scene::SceneRenderData> built)
{
    acknowledgeGpuStructureConsumed(frameIndex);
    m_structureGpu.finishStructureGpuBuild(std::move(built));
}

void Scene::attachDirectionalLightToRoot(scene::DirectionalLightComponent component, const std::string& name)
{
    if (!m_EntityWorld || !ecs::isValid(m_EntityWorld->root()))
        return;

    ecs::Entity entity = m_EntityWorld->createEntity(name, m_EntityWorld->root());
    m_EntityWorld->setDirectionalLight(entity, std::move(component));
    m_EntityWorld->rebuildPathsFromRoot();
}

void Scene::attachSpotLightToRoot(scene::SpotLightComponent component, const std::string& name)
{
    if (!m_EntityWorld || !ecs::isValid(m_EntityWorld->root()))
        return;

    ecs::Entity entity = m_EntityWorld->createEntity(name, m_EntityWorld->root());
    m_EntityWorld->setSpotLight(entity, std::move(component));
    m_EntityWorld->rebuildPathsFromRoot();
}

void Scene::attachPointLightToRoot(scene::PointLightComponent component, const std::string& name)
{
    if (!m_EntityWorld || !ecs::isValid(m_EntityWorld->root()))
        return;

    ecs::Entity entity = m_EntityWorld->createEntity(name, m_EntityWorld->root());
    m_EntityWorld->setPointLight(entity, std::move(component));
    m_EntityWorld->rebuildPathsFromRoot();
}

void Scene::attachEnvironmentLightToRoot(scene::EnvironmentLightComponent component, const std::string& name)
{
    if (!m_EntityWorld || !ecs::isValid(m_EntityWorld->root()))
        return;

    ecs::Entity entity = m_EntityWorld->createEntity(name, m_EntityWorld->root());
    m_EntityWorld->setEnvironmentLight(entity, std::move(component));
    m_EntityWorld->rebuildPathsFromRoot();
}

Scene::Scene(
    caustica::rhi::Device* /*device*/,
    ShaderFactory& /*shaderFactory*/,
    std::shared_ptr<IFileSystem> fs,
    std::shared_ptr<TextureLoader> textureCache,
    std::shared_ptr<SceneTypeFactory> sceneTypeFactory)
    : m_fs(std::move(fs))
    , m_SceneTypeFactory(std::move(sceneTypeFactory))
    , m_TextureLoader(std::move(textureCache))
{
    m_EntityWorld = std::make_unique<scene::SceneEntityWorld>();

    if (!m_SceneTypeFactory)
        m_SceneTypeFactory = std::make_shared<SceneTypeFactory>();

    m_GltfImporter = std::make_shared<GltfImporter>(m_fs, m_SceneTypeFactory);
    m_ObjImporter = std::make_shared<ObjImporter>(m_SceneTypeFactory);
    m_CausUsdImporter = std::make_shared<CausUsdImporter>(m_SceneTypeFactory);
    m_UrdfImporter = std::make_shared<UrdfImporter>(m_SceneTypeFactory);

}

void Scene::resetToOwnedScratchRegistry()
{
    if (m_EntityWorld && !m_EntityWorld->ownsRegistry())
        m_EntityWorld->resetScene();
    m_EntityWorld = std::make_unique<scene::SceneEntityWorld>();
}

void Scene::adoptLiveEcs(ecs::World& liveWorld)
{
    if (!m_EntityWorld)
        return;
    m_EntityWorld->adoptInto(liveWorld, m_SceneTypeFactory.get());
}

bool Scene::load(const std::filesystem::path& sceneFileName, bool asyncTextures)
{
    // Model import is always serial: texture and typed-asset registration are
    // shared across models and are not safe to mutate from concurrent importer tasks.
    g_LoadingStats.ObjectsLoaded = 0;
    g_LoadingStats.ObjectsTotal = 0;

    resetToOwnedScratchRegistry();
    m_LogicExtractCache.clear();
    m_LogicExtractCacheValid = false;

    if (isDirectMeshSceneFile(sceneFileName))
    {
        m_textureSearchDirectory = sceneFileName.parent_path();
        ++g_LoadingStats.ObjectsTotal;
        m_Models.resize(1);
        loadModelAsync(0, sceneFileName, asyncTextures);

        const auto& modelResult = m_Models[0];
        if (!modelResult.entityWorld || !ecs::isValid(modelResult.rootEntity))
            return false;

        m_EntityWorld->importSubtree(ecs::NullEntity, *modelResult.entityWorld, modelResult.rootEntity, m_SceneTypeFactory.get());
        m_EntityWorld->rebuildPathsFromRoot();
    }
    else
    {
        std::filesystem::path scenePath = sceneFileName.parent_path();

        Json::Value documentRoot;
        if (!caustica::json::loadFromFile(*m_fs, sceneFileName, documentRoot))
            return false;

        if (!loadJsonDocument(documentRoot, scenePath, asyncTextures))
            return false;
    }

    return true;
}

bool Scene::loadFromJsonString(const std::string& sceneJson, const std::filesystem::path& scenePath)
{
    g_LoadingStats.ObjectsLoaded = 0;
    g_LoadingStats.ObjectsTotal = 0;

    resetToOwnedScratchRegistry();
    m_LogicExtractCache.clear();
    m_LogicExtractCacheValid = false;

    Json::CharReaderBuilder readerBuilder;
    Json::Value documentRoot;
    std::string errors;
    std::istringstream stream(sceneJson);
    if (!Json::parseFromStream(readerBuilder, stream, &documentRoot, &errors))
    {
        caustica::error("Unable to parse inline scene JSON: %s", errors.c_str());
        return false;
    }

    return loadJsonDocument(documentRoot, scenePath, false);
}

bool Scene::loadJsonDocument(Json::Value documentRoot, const std::filesystem::path& scenePath, bool asyncTextures)
{
    m_textureSearchDirectory = scenePath;

    if (!m_EntityWorld)
        m_EntityWorld = std::make_unique<scene::SceneEntityWorld>();

    m_EntityWorld->createEntity("SceneRoot");

    if (!documentRoot.isObject())
    {
        caustica::error("Unrecognized structure of the scene description.");
        return false;
    }

    return loadEntities(std::move(documentRoot), scenePath, asyncTextures);
}

void Scene::loadModelAsync(
    uint32_t index,
    const std::filesystem::path& fileName,
    bool asyncTextures)
{
    SceneImportResult result;
    loadModelFile(fileName, asyncTextures, result);
    ++g_LoadingStats.ObjectsLoaded;
    m_Models[index] = result;
}

bool Scene::loadModelFile(
    const std::filesystem::path& fileName,
    bool asyncTextures,
    SceneImportResult& result)
{
    std::string ext = fileName.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".obj")
        return m_ObjImporter->load(fileName, *m_TextureLoader, g_LoadingStats, asyncTextures, result, m_textureSearchDirectory);
    if (ext == ".usd" || ext == ".usda" || ext == ".usdc")
        return m_CausUsdImporter->load(fileName, *m_TextureLoader, g_LoadingStats, asyncTextures, result, m_textureSearchDirectory);
    if (ext == ".urdf")
        return m_UrdfImporter->load(fileName, *m_TextureLoader, g_LoadingStats, asyncTextures, result, m_textureSearchDirectory);

    return m_GltfImporter->load(fileName, *m_TextureLoader, g_LoadingStats, asyncTextures, result, m_textureSearchDirectory);
}

SceneImportResult Scene::loadOrGetPrefab(const std::string& source, bool asyncTextures)
{
    if (source.empty())
        return {};

    for (const auto& loaded : m_Models)
    {
        if (loaded.source == source)
        {
            if (!loaded.entityWorld || !ecs::isValid(loaded.rootEntity))
            {
                caustica::error("Cyclic prefab reference '%s'.", source.c_str());
                return {};
            }
            return loaded;
        }
    }

    SceneImportResult result;
    result.source = source;
    m_Models.push_back(result);
    ++g_LoadingStats.ObjectsTotal;

    if (IsBuiltinModelReference(source))
    {
        result = loadBuiltinModel(source);
        result.source = source;
        ++g_LoadingStats.ObjectsLoaded;
    }
    else if (isPrefabAssetPath(source))
    {
        const std::filesystem::path fileName = resolveSceneMediaPath(source, m_textureSearchDirectory);
        Json::Value document;
        const bool loaded = (m_fs && caustica::json::loadFromFile(*m_fs, fileName, document))
            || caustica::json::loadFromFile(fileName, document);
        if (!loaded || !document.isObject())
        {
            caustica::error("Failed to load prefab '%s'.", fileName.generic_string().c_str());
            m_Models.pop_back();
            return {};
        }

        result.entityWorld = std::make_shared<scene::SceneEntityWorld>();
        std::string prefabName;
        if (document["name"].isString())
            prefabName = document["name"].asString();
        else
        {
            prefabName = std::filesystem::path(source).stem().string();
            if (pathEndsWithIgnoreCase(prefabName, ".prefab"))
                prefabName = std::filesystem::path(prefabName).stem().string();
        }
        result.rootEntity = result.entityWorld->createEntity(prefabName);
        if (!instantiateEntities(*result.entityWorld, result.rootEntity, document, asyncTextures))
        {
            m_Models.pop_back();
            return {};
        }
        result.entityWorld->rebuildPathsFromRoot();
        result.source = source;
        ++g_LoadingStats.ObjectsLoaded;
    }
    else
    {
        const std::filesystem::path fileName = resolveSceneMediaPath(source, m_textureSearchDirectory);
        loadModelFile(fileName, asyncTextures, result);
        result.source = source;
        ++g_LoadingStats.ObjectsLoaded;
    }

    m_Models.back() = result;
    return result;
}

ecs::Entity Scene::instantiatePrefab(
    const std::string& source,
    ecs::Entity parent,
    const std::string& name,
    bool asyncTextures,
    scene::SceneEntityWorld* destWorld,
    const std::unordered_map<std::string, std::string>& materials)
{
    const SceneImportResult loaded = loadOrGetPrefab(source, asyncTextures);
    if (!loaded.entityWorld || !ecs::isValid(loaded.rootEntity))
        return ecs::NullEntity;

    scene::SceneEntityWorld& world = destWorld ? *destWorld : *m_EntityWorld;
    ecs::Entity entity = world.importSubtree(
        parent, *loaded.entityWorld, loaded.rootEntity, m_SceneTypeFactory.get());
    if (!ecs::isValid(entity))
        return ecs::NullEntity;

    if (!name.empty())
    {
        if (auto* nameComp = world.world().get<scene::NameComponent>(entity))
            nameComp->value = name;
    }

    world.world().emplace<scene::PrefabInstanceComponent>(
        entity, scene::PrefabInstanceComponent{ source, materials });
    applyMaterialSlots(world, entity, materials);
    return entity;
}

void Scene::applyMaterialSlots(
    scene::SceneEntityWorld& world,
    ecs::Entity root,
    const std::unordered_map<std::string, std::string>& slots)
{
    if (!ecs::isValid(root) || slots.empty())
        return;

    auto applyToMesh = [&](const std::shared_ptr<MeshInfo>& mesh)
    {
        if (!mesh)
            return;
        for (const auto& geometry : mesh->geometries)
        {
            if (!geometry || !geometry->material)
                continue;
            auto it = slots.find(geometry->material->name);
            if (it == slots.end())
                it = slots.find("*");
            if (it != slots.end())
            {
                geometry->material->overrideSource = it->second;
            }
        }
    };

    std::function<void(ecs::Entity)> walk = [&](ecs::Entity entity)
    {
        if (const auto* mesh = world.world().get<scene::MeshInstanceComponent>(entity))
            applyToMesh(mesh->mesh);
        if (const auto* skinned = world.world().get<scene::SkinnedMeshComponent>(entity))
            applyToMesh(skinned->prototypeMesh);
        if (const auto* children = world.world().get<scene::ChildrenComponent>(entity))
        {
            for (ecs::Entity child : children->children)
                walk(child);
        }
    };
    walk(root);
}

SceneImportResult Scene::loadBuiltinModel(const std::string& builtinName)
{
    SceneImportResult result;

    std::vector<BuiltinMeshData> builtinMeshes = MakeBuiltinMeshes(builtinName);
    if (builtinMeshes.empty())
        return result;

    auto buffers = std::make_shared<BufferGroup>();
    auto mesh = m_SceneTypeFactory->createMesh();
    mesh->name = NormalizeBuiltinModelName(builtinName);
    mesh->buffers = buffers;

    for (const BuiltinMeshData& builtinMesh : builtinMeshes)
    {
        auto material = m_SceneTypeFactory->createMaterial();
        material->name = builtinMesh.materialName;
        material->modelFileName = std::string("builtin:") + NormalizeBuiltinModelName(builtinName);
        material->baseOrDiffuseColor = builtinMesh.baseColor;
        material->roughness = 0.65f;
        material->metalness = 0.0f;
        material->doubleSided = true;

        const uint32_t indexOffset = uint32_t(buffers->indexData.size()) - mesh->indexOffset;
        const uint32_t vertexOffset = uint32_t(buffers->positionData.size()) - mesh->vertexOffset;

        AppendMeshToBuffers(builtinMesh, *buffers);

        auto geometry = m_SceneTypeFactory->createMeshGeometry();
        geometry->material = material;
        geometry->indexOffsetInMesh = indexOffset;
        geometry->vertexOffsetInMesh = vertexOffset;
        geometry->numIndices = uint32_t(builtinMesh.indices.size());
        geometry->numVertices = uint32_t(builtinMesh.vertices.size());
        geometry->type = MeshGeometryPrimitiveType::Triangles;

        box3 bounds = box3::empty();
        for (const BuiltinVertex& vertex : builtinMesh.vertices)
            bounds |= vertex.position;

        geometry->objectSpaceBounds = bounds;
        mesh->objectSpaceBounds |= bounds;
        mesh->totalIndices += geometry->numIndices;
        mesh->totalVertices += geometry->numVertices;
        mesh->geometries.push_back(geometry);
    }

    result.entityWorld = std::make_shared<scene::SceneEntityWorld>();
    ecs::Entity rootEntity = result.entityWorld->createEntity(mesh->name);
    result.entityWorld->setMeshInstance(rootEntity, mesh);
    result.entityWorld->rebuildPathsFromRoot();
    result.rootEntity = rootEntity;
    result.source = std::string("builtin:") + NormalizeBuiltinModelName(builtinName);
    return result;
}

void Scene::attachLeafFromJson(
    scene::SceneEntityWorld& world,
    ecs::Entity entity,
    const Json::Value& src)
{
    const auto& leafTypeNode = src["type"];
    if (!leafTypeNode.isString())
        return;

    const std::string type = leafTypeNode.asString();

    if (scene::isJsonLightLeafType(type))
    {
        if (auto component = scene::makeLightComponentFromJson(type, src))
        {
            std::visit(
                [&](auto&& light)
                {
                    using T = std::decay_t<decltype(light)>;
                    if constexpr (std::is_same_v<T, scene::DirectionalLightComponent>)
                        world.setDirectionalLight(entity, std::move(light));
                    else if constexpr (std::is_same_v<T, scene::SpotLightComponent>)
                        world.setSpotLight(entity, std::move(light));
                    else if constexpr (std::is_same_v<T, scene::PointLightComponent>)
                        world.setPointLight(entity, std::move(light));
                    else if constexpr (std::is_same_v<T, scene::EnvironmentLightComponent>)
                        world.setEnvironmentLight(entity, std::move(light));
                },
                std::move(*component));
        }
        else
            caustica::warning("Failed to build light leaf type '%s'.", type.c_str());
        return;
    }

    if (scene::isJsonCameraLeafType(type))
    {
        if (auto component = scene::makeCameraComponentFromJson(type, src))
            world.setCamera(entity, std::move(*component));
        else
            caustica::warning("Failed to build camera leaf type '%s'.", type.c_str());
        return;
    }

    auto leaf = m_SceneTypeFactory->createLeaf(type);
    if (!leaf)
    {
        caustica::warning("Unknown leaf type '%s', skipping.", type.c_str());
        return;
    }

    if (type == "GaussianSplat" || type == "GaussianSplats" || type == "3DGaussianSplat")
    {
        auto splat = std::static_pointer_cast<GaussianSplat>(leaf);
        splat->load(src);
        world.setGaussianSplat(entity, *splat);
    }
    else if (type == "SceneSettings" || type == "SampleSettings")
    {
        auto settings = std::static_pointer_cast<SceneSettings>(leaf);
        settings->load(src);
        world.setSceneSettings(entity, *settings);
    }
    else if (type == "GameSettings")
    {
        auto settings = std::static_pointer_cast<GameSettings>(leaf);
        settings->load(src);
        world.setGameSettings(entity, *settings);
    }
    else
    {
        caustica::warning("Unsupported leaf type '%s' in scene JSON.", type.c_str());
    }
}

static dm::float4 ReadUpToFloat4(const Json::Value& node)
{
    if (node.isNumeric())
        return dm::float4(node.asFloat());

    if (node.isArray())
    {
        float4 result = float4::zero();
        for (int i = 0; i < std::min(4, int(node.size())); i++)
        {
            result[i] = node[i].asFloat();
        }
        return result;
    }

    return float4::zero();
}

void Scene::loadAnimations(const Json::Value& nodeList)
{
    ecs::Entity animationContainer = ecs::NullEntity;

    for (const auto& animationNode : nodeList)
    {
        scene::AnimationComponent component;
        component.editorAuthored = animationNode["editorAuthored"].asBool();
        std::string animationName;

        const auto& nameNode = animationNode["name"];
        if (nameNode.isString())
            animationName = nameNode.asString();

        const auto& channelsNode = animationNode["channels"];
        if (channelsNode.isArray())
        {
            int channelIndex = -1;
            for (const auto& channelSrc : channelsNode)
            {
                ++channelIndex;

                const auto& sampler = std::make_shared<animation::Sampler>();

                const auto& modeNode = channelSrc["mode"];
                if (modeNode.isString())
                {
                    if (modeNode.asString() == "step")
                        sampler->setInterpolationMode(animation::InterpolationMode::Step);
                    else if (modeNode.asString() == "linear")
                        sampler->setInterpolationMode(animation::InterpolationMode::Linear);
                    else if (modeNode.asString() == "slerp")
                        sampler->setInterpolationMode(animation::InterpolationMode::Slerp);
                    else if (modeNode.asString() == "hermite")
                        sampler->setInterpolationMode(animation::InterpolationMode::HermiteSpline);
                    else if (modeNode.asString() == "catmull-rom")
                        sampler->setInterpolationMode(animation::InterpolationMode::CatmullRomSpline);
                    else
                        caustica::warning("Unknown interpolation mode '%s' specified for animation '%s' channel %d.",
                            modeNode.asCString(), animationName.c_str(), channelIndex);
                }
                else
                {
                    sampler->setInterpolationMode(animation::InterpolationMode::Step);
                    caustica::warning("Interpolation mode is not specified for animation '%s' channel %d, using step.",
                        animationName.c_str(), channelIndex);
                }

                const auto& attributeNode = channelSrc["attribute"];
                AnimationAttribute attribute = AnimationAttribute::Undefined;
                if (attributeNode.isString() && !attributeNode.asString().empty())
                {
                    if (attributeNode.asString() == "translation")
                        attribute = AnimationAttribute::Translation;
                    else if (attributeNode.asString() == "rotation")
                        attribute = AnimationAttribute::Rotation;
                    else if (attributeNode.asString() == "scaling")
                        attribute = AnimationAttribute::Scaling;
                    else if (attributeNode.asString() == "visibility"
                        || attributeNode.asString() == "visible")
                        attribute = AnimationAttribute::Visibility;
                    else
                        attribute = AnimationAttribute::LeafProperty;
                }
                else
                {
                    caustica::warning("Attribute is not specified for animation '%s' channel %d, ignoring.",
                        animationName.c_str(), channelIndex);
                    continue;
                }

                int keyframeIndex = -1;
                for (const auto& dataPoint : channelSrc["data"])
                {
                    ++keyframeIndex;

                    const auto& timeNode = dataPoint["time"];
                    if (!timeNode.isNumeric())
                    {
                        caustica::warning("Invalid keyframe %d in animation '%s' channel %d.",
                            keyframeIndex, animationName.c_str(), channelIndex);
                        continue;
                    }

                    animation::Keyframe keyframe;
                    keyframe.time = timeNode.asFloat();
                    keyframe.value = ReadUpToFloat4(dataPoint["value"]);
                    keyframe.inTangent = ReadUpToFloat4(dataPoint["inTangent"]);
                    keyframe.outTangent = ReadUpToFloat4(dataPoint["outTangent"]);

                    sampler->addKeyframe(keyframe);
                }

                auto processTarget = [this, &component, &sampler, attribute, &attributeNode, channelIndex, &animationName](
                                         const Json::Value& targetNode) {
                    if (!targetNode.isString())
                    {
                        if (!targetNode.isNull())
                            caustica::warning("Target specification for animation '%s' channel %d is not a string.",
                                animationName.c_str(), channelIndex);
                        return;
                    }

                    std::string targetName = targetNode.asString();
                    if (caustica::string_utils::starts_with(targetName, "material:"))
                    {
                        targetName = targetName.substr(9);

                        std::shared_ptr<Material> material;
                        for (const auto& it : m_EntityWorld->getMaterials())
                        {
                            if (it->name == targetName)
                            {
                                material = it;
                                break;
                            }
                        }

                        if (material)
                        {
                            scene::AnimationChannelData channelData;
                            channelData.sampler = sampler;
                            channelData.targetMaterial = material;
                            channelData.attribute = AnimationAttribute::LeafProperty;
                            channelData.leafPropertyName = attributeNode.asString();
                            scene::addAnimationChannel(component, std::move(channelData));
                        }
                        else
                        {
                            caustica::warning("Target material '%s' for animation '%s' channel %d not found.",
                                targetName.c_str(), animationName.c_str(), channelIndex);
                        }
                    }
                    else
                    {
                        ecs::Entity target = m_EntityWorld->findEntity(targetNode.asString());
                        if (ecs::isValid(target))
                        {
                            scene::AnimationChannelData channelData;
                            channelData.sampler = sampler;
                            channelData.targetEntity = target;
                            channelData.attribute = attribute;
                            if (attribute == AnimationAttribute::LeafProperty)
                                channelData.leafPropertyName = attributeNode.asString();
                            scene::addAnimationChannel(component, std::move(channelData));
                        }
                        else
                        {
                            caustica::warning("Target entity '%s' for animation '%s' channel %d not found.",
                                targetNode.asCString(), animationName.c_str(), channelIndex);
                        }
                    }
                };

                const auto& targetNode = channelSrc["target"];
                if (!targetNode.isNull())
                    processTarget(targetNode);
                else if (channelSrc["targets"].isArray())
                {
                    for (const auto& targetArrayItem : channelSrc["targets"])
                        processTarget(targetArrayItem);
                }
            }
        }

        if (!component.channels.empty())
        {
            if (!ecs::isValid(animationContainer))
                animationContainer = m_EntityWorld->createEntity("Animations", m_EntityWorld->root());

            ecs::Entity animEntity = m_EntityWorld->createEntity(animationName, animationContainer);
            m_EntityWorld->setAnimation(animEntity, std::move(component));
        }
        else
        {
            caustica::warning("Animation '%s' processed with no valid channels, ignoring.",
                animationName.c_str());
        }
    }
}

bool Scene::loadCustomData(Json::Value& /*rootNode*/, bool /*asyncTextures*/)
{
    // Reserved for derived classes
    return true;
}

void Scene::refreshEntityWorldForFrame(uint32_t frameIndex)
{
    assertLogicThread();

    if (!m_EntityWorld)
        return;

    if (!m_EntityWorld->hasPendingStructureChanges()
        && !m_EntityWorld->hasPendingTransformChanges()
        && !m_EntityWorld->hasPendingLightChanges())
        return;

    scene::SceneRenderPublishState& pending = m_RenderSnapshot.pendingState();
    pending.structureChanged = m_EntityWorld->hasPendingStructureChanges();
    pending.transformsChanged = m_EntityWorld->hasPendingTransformChanges();
    pending.lightsChanged = m_EntityWorld->hasPendingLightChanges();
    pending.frameIndex = frameIndex;
    if (pending.structureChanged)
        pending.structureGeneration = m_structureGpu.bumpPublishedGeneration();
    else
        pending.structureGeneration = m_structureGpu.publishedGeneration();

    m_EntityWorld->refresh(frameIndex);
}

const scene::SceneRenderData& Scene::extractAndPublishForGpuSetup(
    uint32_t frameIndex,
    const scene::FrameExtractInputs* frameInputs)
{
    extractAndPublishRenderSnapshot(frameIndex, frameInputs);
    return m_RenderSnapshot.readBufferForFrame(frameIndex);
}

void Scene::extractLogicRenderCache(uint32_t frameIndex)
{
    assertLogicThread();

    if (!m_EntityWorld)
        return;

    scene::SceneRenderPublishState& pending = m_RenderSnapshot.pendingState();
    const bool hasPendingChanges =
        m_EntityWorld->hasPendingStructureChanges()
        || m_EntityWorld->hasPendingTransformChanges()
        || m_EntityWorld->hasPendingLightChanges();

    scene::SceneRenderExtractFlags flags{
        .structureChanged = !m_LogicExtractCacheValid,
        .transformsChanged = !m_LogicExtractCacheValid,
        .lightsChanged = !m_LogicExtractCacheValid,
    };

    if (hasPendingChanges)
    {
        refreshEntityWorldForFrame(frameIndex);
        flags.structureChanged = pending.structureChanged || !m_LogicExtractCacheValid;
        flags.transformsChanged = pending.transformsChanged || !m_LogicExtractCacheValid;
        flags.lightsChanged = pending.lightsChanged || !m_LogicExtractCacheValid;
    }
    else if (pending.frameIndex != frameIndex)
    {
        // Carry structure-flush across no-op frames until GPU consumes it.
        // Do not treat this as an extract structure rebuild — proxies are already current.
        pending.structureGeneration = m_structureGpu.publishedGeneration();
        pending.structureChanged = pending.structureGeneration > m_structureGpu.consumedGeneration();
        pending.transformsChanged = false;
        pending.lightsChanged = false;
        pending.frameIndex = frameIndex;
    }

    scene::extractSceneRenderData(*m_EntityWorld, m_LogicExtractCache, frameIndex, flags);
    m_LogicExtractCacheValid = true;
}

scene::SceneRenderData* Scene::logicExtractCache()
{
    assertLogicThread();
    return m_EntityWorld ? &m_LogicExtractCache : nullptr;
}

void Scene::publishRenderSnapshot(
    uint32_t frameIndex, const scene::FrameExtractInputs* frameInputs)
{
    assertLogicThread();

    if (!m_EntityWorld)
        return;

    scene::SceneRenderData& writeBuffer = m_RenderSnapshot.writeBufferForFrame(frameIndex);
    writeBuffer = m_LogicExtractCache;
    if (frameInputs)
        scene::extractFrameRenderState(*frameInputs, writeBuffer);

    m_RenderSnapshot.publish(frameIndex);

    // ChangeDetection contract: mutate -> hierarchy refresh -> Extract(Changed<>) -> end tick.
    m_EntityWorld->endChangeDetectionFrame();
}

void Scene::extractAndPublishRenderSnapshot(
    uint32_t frameIndex, const scene::FrameExtractInputs* frameInputs)
{
    extractLogicRenderCache(frameIndex);
    if (m_EntityWorld)
        scene::extractGaussianSplatProxies(*m_EntityWorld, m_LogicExtractCache);
    publishRenderSnapshot(frameIndex, frameInputs);
}

bool Scene::wasRenderSnapshotExtractedOnLogicThread(uint32_t frameIndex) const
{
    return m_RenderSnapshot.wasExtractedForFrame(frameIndex);
}

uint32_t Scene::latestPublishedRenderFrameIndex() const
{
    return m_RenderSnapshot.latestExtractedFrameIndex();
}

void Scene::syncRenderSnapshotGpuIndices(uint32_t /*frameIndex*/)
{
    // Instance indices are assigned during logic-thread refresh and copied into
    // MeshInstanceRenderProxy at Extract. render thread must not patch from ECS.
}


// =============================================================================
// processNodesRecursive - post-load scene traversal (merged from ExtendedScene)
// =============================================================================

void Scene::processNodesRecursive()
{
    if (!m_EntityWorld || !ecs::isValid(m_EntityWorld->root()))
        return;

    auto& world = m_EntityWorld->world();

    world.each<scene::SceneSettingsComponent>([this](ecs::Entity, scene::SceneSettingsComponent& component)
    {
        m_loadedSettings = component.settings;
    });

    if (m_loadedSettings && m_loadedSettings->startingCameraId && !m_loadedSettings->startingCameraId->empty())
    {
        const std::string wanted = *m_loadedSettings->startingCameraId;
        const auto& cameras = m_EntityWorld->cameraEntitiesInRegistrationOrder();
        for (size_t i = 0; i < cameras.size(); ++i)
        {
            bool match = m_EntityWorld->getEntityName(cameras[i]) == wanted;
            if (!match)
            {
                if (const auto* authoring = world.tryGet<scene::SceneAuthoringIdComponent>(cameras[i]))
                    match = authoring->id == wanted;
            }
            if (!match)
            {
                const std::string path = m_EntityWorld->getEntityPath(cameras[i]).generic_string();
                match = path == wanted || path.ends_with("/" + wanted);
            }
            if (match)
            {
                m_loadedSettings->startingCamera = static_cast<int>(i) - 1;
                break;
            }
        }
    }

    world.each<scene::GameSettingsComponent>([this](ecs::Entity, scene::GameSettingsComponent& component)
    {
        m_loadedGameSettings = component.settings;
    });

    auto bindProxyMeshes = [this, &world](ecs::Entity lightEntity, const std::vector<std::string>& proxies)
    {
        if (proxies.empty())
            return;

        for (const auto& proxyPath : proxies)
        {
            ecs::Entity proxyEntity = m_EntityWorld->entityForPath(proxyPath);
            if (!ecs::isValid(proxyEntity))
                proxyEntity = m_EntityWorld->entityForPath(std::filesystem::path("/") / proxyPath);

            if (ecs::isValid(proxyEntity))
            {
                auto* mesh = world.get<scene::MeshInstanceComponent>(proxyEntity);
                if (mesh)
                    mesh->proxiedAnalyticLight = lightEntity;
            }
        }
    };

    world.each<scene::SpotLightComponent>([&](ecs::Entity lightEntity, scene::SpotLightComponent& component)
    {
        bindProxyMeshes(lightEntity, component.proxies);
    });
    world.each<scene::PointLightComponent>([&](ecs::Entity lightEntity, scene::PointLightComponent& component)
    {
        bindProxyMeshes(lightEntity, component.proxies);
    });
}
