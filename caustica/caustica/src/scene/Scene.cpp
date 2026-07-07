#include <scene/Scene.h>
#include <scene/SceneImport.h>
#include <scene/SceneRenderExtract.h>
#include <scene/SceneLightAccess.h>
#include <scene/SceneObjects.h>
#include <scene/SceneAnimation.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/loader/GltfImporter.h>
#include <scene/loader/ObjImporter.h>
#include <scene/scene_utils.h>
#include <render/SceneGpuResources.h>
#include <core/ThreadPool.h>
#include <core/json.h>
#include <core/log.h>
#include <core/string_utils.h>
#include <rhi/common/misc.h>
#include <json/json.h>

#include <assets/loader/ShaderFactory.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/skinning_cs.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/skinning_cs.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/skinning_cs.spirv.h"
#endif
#endif

using namespace caustica::math;
#include <shaders/material_cb.h>
#include <shaders/skinning_cb.h>
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

const SceneLoadingStats& Scene::GetLoadingStats()
{
    return g_LoadingStats;
}

box3 Scene::GetSceneBounds() const
{
    if (!m_EntityWorld || !ecs::isValid(m_EntityWorld->root()))
        return box3::empty();

    const auto* bounds = m_EntityWorld->world().get<scene::BoundsComponent>(m_EntityWorld->root());
    if (!bounds)
        return box3::empty();

    const box3& globalBounds = bounds->globalBounds;
    const bool finite = dm::all(dm::isfinite(globalBounds.m_mins)) && dm::all(dm::isfinite(globalBounds.m_maxs));
    return finite ? globalBounds : box3::empty();
}

const ResourceTracker<Material>& Scene::GetMaterials() const
{
    if (m_EntityWorld)
        return m_EntityWorld->GetMaterials();

    static const ResourceTracker<Material> s_empty;
    return s_empty;
}

const ResourceTracker<MeshInfo>& Scene::GetMeshes() const
{
    if (m_EntityWorld)
        return m_EntityWorld->GetMeshes();

    static const ResourceTracker<MeshInfo> s_empty;
    return s_empty;
}

size_t Scene::GetGeometryCount() const
{
    return m_EntityWorld ? m_EntityWorld->GetGeometryCount() : 0;
}

size_t Scene::GetMaxGeometryCountPerMesh() const
{
    return m_EntityWorld ? m_EntityWorld->GetMaxGeometryCountPerMesh() : 0;
}

size_t Scene::GetGeometryInstancesCount() const
{
    return m_EntityWorld ? m_EntityWorld->GetGeometryInstancesCount() : 0;
}

const std::vector<ecs::Entity>& Scene::GetMeshInstances() const
{
    return getRenderSnapshotForRead().meshInstanceEntities;
}

const std::vector<ecs::Entity>& Scene::GetSkinnedMeshInstances() const
{
    return getRenderSnapshotForRead().skinnedMeshInstanceEntities;
}

const std::vector<ecs::Entity>& Scene::GetLightEntities() const
{
    return getRenderSnapshotForRead().lightEntities;
}

const std::vector<ecs::Entity>& Scene::GetCameraEntities() const
{
    return getRenderSnapshotForRead().cameraEntities;
}

const std::vector<ecs::Entity>& Scene::GetAnimationEntities() const
{
    return getRenderSnapshotForRead().animationEntities;
}

const scene::SceneRenderData& Scene::GetRenderData() const
{
    return getRenderSnapshotForRead();
}

const scene::SceneRenderData& Scene::getRenderSnapshotForRead() const
{
    const uint32_t gpuFrameIndex = m_gpuReadFrameIndex.load(std::memory_order_acquire);
    if (gpuFrameIndex != UINT32_MAX)
        return m_RenderSnapshot.readBufferForFrame(gpuFrameIndex);

    const uint32_t latestFrameIndex = m_RenderSnapshot.latestExtractedFrameIndex();
    if (latestFrameIndex != UINT32_MAX)
        return m_RenderSnapshot.readBufferForFrame(latestFrameIndex);

    return m_RenderSnapshot.readBufferForFrame(0);
}

void Scene::beginGpuReadFrame(uint32_t frameIndex)
{
    m_gpuReadFrameIndex.store(frameIndex, std::memory_order_release);
}

void Scene::endGpuReadFrame()
{
    m_gpuReadFrameIndex.store(UINT32_MAX, std::memory_order_release);
}

bool Scene::HasSceneTransformsChanged() const
{
    return m_SceneTransformsChanged;
}

bool Scene::HasSceneStructureChanged() const
{
    return m_SceneStructureChanged;
}

bool Scene::HasSceneTransformsChanged(uint32_t frameIndex) const
{
    return m_RenderSnapshot.publishedStateForFrame(frameIndex).transformsChanged;
}

bool Scene::HasSceneStructureChanged(uint32_t frameIndex) const
{
    return m_RenderSnapshot.publishedStateForFrame(frameIndex).structureChanged;
}

void Scene::AttachLightToRoot(const std::shared_ptr<Light>& light)
{
    if (!m_EntityWorld || !light || !ecs::isValid(m_EntityWorld->root()))
        return;

    ecs::Entity entity = m_EntityWorld->createEntity(light->name, m_EntityWorld->root());
    m_EntityWorld->setLight(entity, light);
    m_EntityWorld->rebuildPathsFromRoot();
}

void Scene::AttachLightToRoot(scene::LightComponent component, const std::string& name)
{
    if (!m_EntityWorld || !ecs::isValid(m_EntityWorld->root()))
        return;

    ecs::Entity entity = m_EntityWorld->createEntity(name, m_EntityWorld->root());
    m_EntityWorld->setLight(entity, std::move(component));
    m_EntityWorld->rebuildPathsFromRoot();
}

Scene::Scene(
    nvrhi::IDevice* device,
    ShaderFactory& shaderFactory,
    std::shared_ptr<IFileSystem> fs,
    std::shared_ptr<TextureLoader> textureCache,
    std::shared_ptr<IDescriptorTableManager> descriptorTable,
    std::shared_ptr<SceneTypeFactory> sceneTypeFactory)
    : m_fs(std::move(fs))
    , m_SceneTypeFactory(std::move(sceneTypeFactory))
    , m_TextureLoader(std::move(textureCache))
    , m_DescriptorTable(std::move(descriptorTable))
{
    m_GpuResources = std::make_shared<render::SceneGpuResources>();
    m_GpuResources->device = device;
    m_EntityWorld = std::make_unique<scene::SceneEntityWorld>();

    if (!m_SceneTypeFactory)
        m_SceneTypeFactory = std::make_shared<SceneTypeFactory>();

    m_GltfImporter = std::make_shared<GltfImporter>(m_fs, m_SceneTypeFactory);
    m_ObjImporter = std::make_shared<ObjImporter>(m_SceneTypeFactory);

    m_GpuResources->enableBindlessResources = !!m_DescriptorTable;
    m_GpuResources->rayTracingSupported = m_GpuResources->device->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct);

    m_GpuResources->skinningShader = shaderFactory.createAutoShader("engine/skinning_cs", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_skinning_cs), nullptr, nvrhi::ShaderType::Compute);

    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SkinningConstants)),
            nvrhi::BindingLayoutItem::RawBuffer_SRV(0),
            nvrhi::BindingLayoutItem::RawBuffer_SRV(1),
            nvrhi::BindingLayoutItem::RawBuffer_UAV(0)
        };

        m_GpuResources->skinningBindingLayout = m_GpuResources->device->createBindingLayout(layoutDesc);
    }

    {
        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.bindingLayouts = { m_GpuResources->skinningBindingLayout };
        pipelineDesc.CS = m_GpuResources->skinningShader;
        m_GpuResources->skinningPipeline = m_GpuResources->device->createComputePipeline(pipelineDesc);
    }
}

bool Scene::Load(const std::filesystem::path& jsonFileName)
{
    // Texture and typed-asset registration are shared across model imports and are
    // not safe to mutate from multiple importer tasks.
    return LoadWithThreadPool(jsonFileName, nullptr);
}

bool Scene::LoadWithThreadPool(const std::filesystem::path& sceneFileName, ThreadPool* threadPool)
{
    g_LoadingStats.ObjectsLoaded = 0;
    g_LoadingStats.ObjectsTotal = 0;

    m_EntityWorld = std::make_unique<scene::SceneEntityWorld>();

    if (IsDirectMeshSceneFile(sceneFileName))
    {
        m_textureSearchDirectory = sceneFileName.parent_path();
        ++g_LoadingStats.ObjectsTotal;
        m_Models.resize(1);
        LoadModelAsync(0, sceneFileName, threadPool);

        if (threadPool)
            threadPool->WaitForTasks();

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
        if (!caustica::json::LoadFromFile(*m_fs, sceneFileName, documentRoot))
            return false;

        if (!LoadJsonDocument(documentRoot, scenePath, threadPool))
            return false;
    }

    return true;
}

bool Scene::LoadFromJsonString(const std::string& sceneJson, const std::filesystem::path& scenePath)
{
    g_LoadingStats.ObjectsLoaded = 0;
    g_LoadingStats.ObjectsTotal = 0;

    m_EntityWorld = std::make_unique<scene::SceneEntityWorld>();

    Json::CharReaderBuilder readerBuilder;
    Json::Value documentRoot;
    std::string errors;
    std::istringstream stream(sceneJson);
    if (!Json::parseFromStream(readerBuilder, stream, &documentRoot, &errors))
    {
        caustica::error("Unable to parse inline scene JSON: %s", errors.c_str());
        return false;
    }

    return LoadJsonDocument(documentRoot, scenePath, nullptr);
}

bool Scene::LoadJsonDocument(Json::Value documentRoot, const std::filesystem::path& scenePath, ThreadPool* threadPool)
{
    m_textureSearchDirectory = scenePath;

    if (!m_EntityWorld)
        m_EntityWorld = std::make_unique<scene::SceneEntityWorld>();

    m_EntityWorld->createEntity("SceneRoot");

    if (documentRoot.isObject())
    {
        if (!LoadCustomData(documentRoot, threadPool))
            return false;

        LoadModels(documentRoot["models"], scenePath, threadPool);
        LoadSceneEntities(documentRoot["graph"], m_EntityWorld->root());
        LoadAnimations(documentRoot["animations"]);
        m_EntityWorld->rebuildPathsFromRoot();
    }
    else
    {
        caustica::error("Unrecognized structure of the scene description.");
        return false;
    }

    return true;
}

void Scene::LoadModelAsync(
    uint32_t index,
    const std::filesystem::path& fileName,
    ThreadPool* threadPool)
{   
    if (threadPool)
    {
        threadPool->AddTask([this, index, threadPool, fileName]()
        {
            SceneImportResult result;
            LoadModelFile(fileName, threadPool, result);
            ++g_LoadingStats.ObjectsLoaded;
            m_Models[index] = result;
        });
    }
    else
    {
        SceneImportResult result;
        LoadModelFile(fileName, threadPool, result);
        ++g_LoadingStats.ObjectsLoaded;
        m_Models[index] = result;
    }
}

bool Scene::LoadModelFile(
    const std::filesystem::path& fileName,
    ThreadPool* threadPool,
    SceneImportResult& result)
{
    std::string ext = fileName.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".obj")
        return m_ObjImporter->Load(fileName, *m_TextureLoader, g_LoadingStats, threadPool, result, m_textureSearchDirectory);

    return m_GltfImporter->Load(fileName, *m_TextureLoader, g_LoadingStats, threadPool, result, m_textureSearchDirectory);
}

void Scene::LoadModels(
    const Json::Value& modelList,
    const std::filesystem::path& scenePath,
    ThreadPool* threadPool)
{
    if (!modelList.isArray())
    {
        return;
    }

    m_Models.resize(modelList.size());
    uint32_t index = 0;
    for (const auto& model : modelList)
    {
        ++g_LoadingStats.ObjectsTotal;

        if (model.isString() && IsBuiltinModelReference(model.asString()))
        {
            m_Models[index] = LoadBuiltinModel(model.asString());
            ++g_LoadingStats.ObjectsLoaded;
        }
        else if (model.isObject() && model["builtin"].isString())
        {
            m_Models[index] = LoadBuiltinModel(model["builtin"].asString());
            ++g_LoadingStats.ObjectsLoaded;
        }
        else
        {
            std::filesystem::path fileName = scenePath / std::filesystem::path(model.asString());
            LoadModelAsync(index, fileName, threadPool);
        }

        ++index;
    }

    if (threadPool)
        threadPool->WaitForTasks();
}

SceneImportResult Scene::LoadBuiltinModel(const std::string& builtinName)
{
    SceneImportResult result;

    std::vector<BuiltinMeshData> builtinMeshes = MakeBuiltinMeshes(builtinName);
    if (builtinMeshes.empty())
        return result;

    auto buffers = std::make_shared<BufferGroup>();
    auto mesh = m_SceneTypeFactory->CreateMesh();
    mesh->name = NormalizeBuiltinModelName(builtinName);
    mesh->buffers = buffers;

    for (const BuiltinMeshData& builtinMesh : builtinMeshes)
    {
        auto material = m_SceneTypeFactory->CreateMaterial();
        material->name = builtinMesh.materialName;
        material->modelFileName = std::string("builtin:") + NormalizeBuiltinModelName(builtinName);
        material->baseOrDiffuseColor = builtinMesh.baseColor;
        material->roughness = 0.65f;
        material->metalness = 0.0f;
        material->doubleSided = true;

        const uint32_t indexOffset = uint32_t(buffers->indexData.size()) - mesh->indexOffset;
        const uint32_t vertexOffset = uint32_t(buffers->positionData.size()) - mesh->vertexOffset;

        AppendMeshToBuffers(builtinMesh, *buffers);

        auto geometry = m_SceneTypeFactory->CreateMeshGeometry();
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
    return result;
}

void Scene::attachLeafFromJson(ecs::Entity entity, const Json::Value& src)
{
    const auto& leafTypeNode = src["type"];
    if (!leafTypeNode.isString())
        return;

    const std::string type = leafTypeNode.asString();
    auto leaf = m_SceneTypeFactory->CreateLeaf(type);
    if (!leaf)
    {
        caustica::warning("Unknown leaf type '%s', skipping.", type.c_str());
        return;
    }

    if (type == "DirectionalLight" || type == "PointLight" || type == "SpotLight" || type == "EnvironmentLight")
    {
        auto light = std::static_pointer_cast<Light>(leaf);
        light->Load(src);
        m_EntityWorld->setLight(entity, light);
    }
    else if (type == "PerspectiveCamera" || type == "PerspectiveCameraEx" || type == "OrthographicCamera")
    {
        auto camera = std::static_pointer_cast<SceneCamera>(leaf);
        camera->Load(src);
        m_EntityWorld->setCamera(entity, camera);
    }
    else if (type == "GaussianSplat" || type == "GaussianSplats" || type == "3DGaussianSplat")
    {
        auto splat = std::static_pointer_cast<GaussianSplat>(leaf);
        splat->Load(src);
        m_EntityWorld->setGaussianSplat(entity, splat);
    }
    else if (type == "SampleSettings")
    {
        auto settings = std::static_pointer_cast<SampleSettings>(leaf);
        settings->Load(src);
        m_EntityWorld->setSampleSettings(entity, settings);
    }
    else if (type == "GameSettings")
    {
        auto settings = std::static_pointer_cast<GameSettings>(leaf);
        settings->Load(src);
        m_EntityWorld->setGameSettings(entity, settings);
    }
    else
    {
        caustica::warning("Unsupported leaf type '%s' in scene JSON.", type.c_str());
    }
}

void Scene::LoadSceneEntities(const Json::Value& nodeList, ecs::Entity parent)
{
    for (const auto& src : nodeList)
    {
        if (!src.isObject())
        {
            caustica::warning("Non-object node in the scene definition.");
            continue;
        }

        std::string nodeName;
        const auto& name = src["name"];
        if (name.isString())
            nodeName = name.asString();

        ecs::Entity customParent = parent;
        const auto& parentNode = src["parent"];
        if (parentNode.isString())
        {
            customParent = m_EntityWorld->findEntity(parentNode.asString());
            if (!ecs::isValid(customParent))
            {
                caustica::warning("Custom parent '%s' specified for node '%s' not found, skipping the node.",
                    parentNode.asCString(), nodeName.c_str());
                continue;
            }
        }
        else if (!parentNode.isNull())
        {
            caustica::warning("Custom parent specification for node '%s' is not a string, ignoring.",
                nodeName.c_str());
        }

        ecs::Entity entity = ecs::NullEntity;

        const auto& modelNode = src["model"];
        if (!modelNode.isNull())
        {
            if (!modelNode.isIntegral())
            {
                caustica::warning("Model references in the scene must be indices into the model array.");
                continue;
            }

            int modelIndex = modelNode.asInt();
            if (modelIndex < 0 || modelIndex >= int(m_Models.size()))
            {
                caustica::warning("Referenced model %d is not defined in the model array.", modelIndex);
                continue;
            }

            const auto& loadedModel = m_Models[modelIndex];
            if (!loadedModel.entityWorld || !ecs::isValid(loadedModel.rootEntity))
                continue;

            entity = m_EntityWorld->importSubtree(customParent, *loadedModel.entityWorld, loadedModel.rootEntity, m_SceneTypeFactory.get());
        }
        else
        {
            entity = m_EntityWorld->createEntity(nodeName, customParent);
        }

        if (!ecs::isValid(entity))
            continue;

        if (!nodeName.empty())
        {
            if (auto* nameComp = m_EntityWorld->world().get<scene::NameComponent>(entity))
                nameComp->value = nodeName;
        }

        const auto& translation = src["translation"];
        if (!translation.isNull())
        {
            double3 value = double3::zero();
            translation >> value;
            m_EntityWorld->setTranslation(entity, value);
        }

        const auto& rotation = src["rotation"];
        if (!rotation.isNull())
        {
            double4 value = double4(0.0, 0.0, 0.0, 1.0);
            rotation >> value;
            m_EntityWorld->setRotation(entity, dm::dquat::fromXYZW(value));
        }
        else
        {
            const auto& euler = src["euler"];
            if (!euler.isNull())
            {
                double3 value = double3::zero();
                euler >> value;
                m_EntityWorld->setRotation(entity, rotationQuat(value));
            }
        }

        const auto& scaling = src["scaling"];
        if (!scaling.isNull())
        {
            double3 value = double3(1.0);
            scaling >> value;
            m_EntityWorld->setScaling(entity, value);
        }

        const auto& children = src["children"];
        if (!children.isNull())
            LoadSceneEntities(children, entity);

        if (src["type"].isString())
            attachLeafFromJson(entity, src);
        else if (!src["type"].isNull())
        {
            caustica::warning("Leaf type specification for node '%s' is not a string, skipping.",
                nodeName.c_str());
        }
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

void Scene::LoadAnimations(const Json::Value& nodeList)
{
    ecs::Entity animationContainer = ecs::NullEntity;

    for (const auto& animationNode : nodeList)
    {
        scene::AnimationComponent component;
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
                        sampler->SetInterpolationMode(animation::InterpolationMode::Step);
                    else if (modeNode.asString() == "linear")
                        sampler->SetInterpolationMode(animation::InterpolationMode::Linear);
                    else if (modeNode.asString() == "slerp")
                        sampler->SetInterpolationMode(animation::InterpolationMode::Slerp);
                    else if (modeNode.asString() == "hermite")
                        sampler->SetInterpolationMode(animation::InterpolationMode::HermiteSpline);
                    else if (modeNode.asString() == "catmull-rom")
                        sampler->SetInterpolationMode(animation::InterpolationMode::CatmullRomSpline);
                    else
                        caustica::warning("Unknown interpolation mode '%s' specified for animation '%s' channel %d.",
                            modeNode.asCString(), animationName.c_str(), channelIndex);
                }
                else
                {
                    sampler->SetInterpolationMode(animation::InterpolationMode::Step);
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

                    sampler->AddKeyframe(keyframe);
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
                        for (const auto& it : m_EntityWorld->GetMaterials())
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
                            scene::AddAnimationChannel(component, std::move(channelData));
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
                            scene::AddAnimationChannel(component, std::move(channelData));
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

bool Scene::LoadCustomData(Json::Value& rootNode, ThreadPool* threadPool)
{
    // Reserved for derived classes
    return true;
}

void Scene::refreshEntityWorldForFrame(uint32_t frameIndex)
{
    if (!m_EntityWorld)
        return;

    if (!m_EntityWorld->hasPendingStructureChanges() && !m_EntityWorld->hasPendingTransformChanges())
        return;

    scene::SceneRenderPublishState& pending = m_RenderSnapshot.pendingState();
    pending.structureChanged = m_EntityWorld->hasPendingStructureChanges();
    pending.transformsChanged = m_EntityWorld->hasPendingTransformChanges();
    pending.frameIndex = frameIndex;

    m_EntityWorld->refresh(frameIndex);
}

void Scene::RefreshSceneWorld(uint32_t frameIndex)
{
    refreshEntityWorldForFrame(frameIndex);
    if (!m_EntityWorld)
        return;

    scene::ExtractSceneRenderData(*m_EntityWorld, m_RenderSnapshot.writeBufferForFrame(frameIndex));
}

void Scene::PublishRenderSnapshot(uint32_t frameIndex)
{
    m_RenderSnapshot.publish(frameIndex);
    const scene::SceneRenderPublishState& published = m_RenderSnapshot.publishedStateForFrame(frameIndex);
    m_SceneStructureChanged = published.structureChanged;
    m_SceneTransformsChanged = published.transformsChanged;
    m_RenderCommands.drain(*this);
}

void Scene::extractAndPublishRenderSnapshot(uint32_t frameIndex)
{
    if (!m_EntityWorld)
        return;

    scene::SceneRenderPublishState& pending = m_RenderSnapshot.pendingState();
    if (pending.frameIndex != frameIndex)
    {
        if (m_EntityWorld->hasPendingStructureChanges() || m_EntityWorld->hasPendingTransformChanges())
            refreshEntityWorldForFrame(frameIndex);
        else
        {
            pending.structureChanged = false;
            pending.transformsChanged = false;
            pending.frameIndex = frameIndex;
        }
    }

    scene::ExtractSceneRenderData(*m_EntityWorld, m_RenderSnapshot.writeBufferForFrame(frameIndex));
    PublishRenderSnapshot(frameIndex);
}

bool Scene::wasRenderSnapshotExtractedOnLogicThread(uint32_t frameIndex) const
{
    return m_RenderSnapshot.wasExtractedForFrame(frameIndex);
}

void Scene::syncRenderSnapshotGpuIndices(uint32_t frameIndex)
{
    if (!m_EntityWorld)
        return;

    scene::SceneRenderData& data = m_RenderSnapshot.bufferForFrame(frameIndex);
    const ecs::World& world = m_EntityWorld->world();
    for (scene::MeshInstanceRenderProxy& proxy : data.meshInstances)
    {
        if (!world.isAlive(proxy.entity))
            continue;

        if (const auto* meshComp = world.get<scene::MeshInstanceComponent>(proxy.entity))
        {
            proxy.instanceIndex = meshComp->instanceIndex;
            proxy.geometryInstanceIndex = meshComp->geometryInstanceIndex;
        }
    }
}

GeometryData* Scene::GetGeometryData(const MeshGeometry& geometry) const
{
    if (m_GpuResources == nullptr || uint(geometry.globalGeometryIndex) >= m_GpuResources->geometryData.size() )
        return nullptr;

    return &m_GpuResources->geometryData[geometry.globalGeometryIndex];
}


// =============================================================================
// ProcessNodesRecursive - post-load scene traversal (merged from ExtendedScene)
// =============================================================================

void Scene::ProcessNodesRecursive()
{
    if (!m_EntityWorld || !ecs::isValid(m_EntityWorld->root()))
        return;

    auto& world = m_EntityWorld->world();

    world.each<scene::SampleSettingsComponent>([this](ecs::Entity, scene::SampleSettingsComponent& component)
    {
        assert(m_loadedSettings == nullptr || m_loadedSettings == component.settings);
        m_loadedSettings = component.settings;
    });

    world.each<scene::GameSettingsComponent>([this](ecs::Entity, scene::GameSettingsComponent& component)
    {
        assert(m_loadedGameSettings == nullptr || m_loadedGameSettings == component.settings);
        m_loadedGameSettings = component.settings;
    });

    world.each<scene::LightComponent>([this, &world](ecs::Entity lightEntity, scene::LightComponent& component)
    {
        const int lightType = scene::GetLightType(component);
        if ((lightType != LightType_Spot && lightType != LightType_Point) || component.proxies.empty())
            return;

        for (const auto& proxyPath : component.proxies)
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
    });
}
