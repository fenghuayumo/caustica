#include <scene/Scene.h>
#include <assets/loader/GltfImporter.h>
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

#if DONUT_WITH_STATIC_SHADERS
#if DONUT_WITH_DX11
#include "compiled_shaders/skinning_cs.dxbc.h"
#endif
#if DONUT_WITH_DX12
#include "compiled_shaders/skinning_cs.dxil.h"
#endif
#if DONUT_WITH_VULKAN
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
    if (!m_SceneGraph || !m_SceneGraph->GetRootNode())
        return box3::empty();

    const box3& bounds = m_SceneGraph->GetRootNode()->GetGlobalBoundingBox();
    const bool finite = dm::all(dm::isfinite(bounds.m_mins)) && dm::all(dm::isfinite(bounds.m_maxs));
    return finite ? bounds : box3::empty();
}

struct Scene::Resources
{
    std::vector<MaterialConstants> materialData;
    std::vector<GeometryData> geometryData;
    std::vector<InstanceData> instanceData;
};

Scene::Scene(
    nvrhi::IDevice* device,
    ShaderFactory& shaderFactory,
    std::shared_ptr<IFileSystem> fs,
    std::shared_ptr<TextureCache> textureCache,
    std::shared_ptr<DescriptorTableManager> descriptorTable,
    std::shared_ptr<SceneTypeFactory> sceneTypeFactory)
    : m_fs(std::move(fs))
    , m_SceneTypeFactory(std::move(sceneTypeFactory))
    , m_TextureCache(std::move(textureCache))
    , m_DescriptorTable(std::move(descriptorTable))
    , m_Device(device)
{
    m_Resources = std::make_shared<Resources>();

    if (!m_SceneTypeFactory)
        m_SceneTypeFactory = std::make_shared<SceneTypeFactory>();

    m_GltfImporter = std::make_shared<GltfImporter>(m_fs, m_SceneTypeFactory);

    m_EnableBindlessResources = !!m_DescriptorTable;
    m_RayTracingSupported = m_Device->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct);

    m_SkinningShader = shaderFactory.CreateAutoShader("donut/skinning_cs", "main", DONUT_MAKE_PLATFORM_SHADER(g_skinning_cs), nullptr, nvrhi::ShaderType::Compute);

    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SkinningConstants)),
            nvrhi::BindingLayoutItem::RawBuffer_SRV(0),
            nvrhi::BindingLayoutItem::RawBuffer_SRV(1),
            nvrhi::BindingLayoutItem::RawBuffer_UAV(0)
        };

        m_SkinningBindingLayout = m_Device->createBindingLayout(layoutDesc);
    }

    {
        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.bindingLayouts = { m_SkinningBindingLayout };
        pipelineDesc.CS = m_SkinningShader;
        m_SkinningPipeline = m_Device->createComputePipeline(pipelineDesc);
    }
}

bool Scene::Load(const std::filesystem::path& jsonFileName)
{
    ThreadPool threadPool;
    return LoadWithThreadPool(jsonFileName, &threadPool);
}

bool Scene::LoadWithThreadPool(const std::filesystem::path& sceneFileName, ThreadPool* threadPool)
{
    g_LoadingStats.ObjectsLoaded = 0;
    g_LoadingStats.ObjectsTotal = 0;
    
    m_SceneGraph = std::make_shared<SceneGraph>();

    if (sceneFileName.extension() == ".gltf" || sceneFileName.extension() == ".glb")
    {
        m_textureSearchDirectory = sceneFileName.parent_path();
        ++g_LoadingStats.ObjectsTotal;
        m_Models.resize(1);
        LoadModelAsync(0, sceneFileName, threadPool);

        if (threadPool)
            threadPool->WaitForTasks();

        auto modelResult = m_Models[0];
        if (!modelResult.rootNode)
            return false;

        m_SceneGraph->SetRootNode(modelResult.rootNode);
    }
    else
    {
        std::filesystem::path scenePath = sceneFileName.parent_path();

        Json::Value documentRoot;
        if (!caustica::json::LoadFromFile(*m_fs, sceneFileName, documentRoot))
            return false;

        return LoadJsonDocument(documentRoot, scenePath, threadPool);
    }

    return true;
}

bool Scene::LoadFromJsonString(const std::string& sceneJson, const std::filesystem::path& scenePath)
{
    g_LoadingStats.ObjectsLoaded = 0;
    g_LoadingStats.ObjectsTotal = 0;

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
    m_SceneGraph = std::make_shared<SceneGraph>();

    std::shared_ptr<SceneGraphNode> rootNode = std::make_shared<SceneGraphNode>();
    rootNode->SetName("SceneRoot");
    m_SceneGraph->SetRootNode(rootNode);

    if (documentRoot.isObject())
    {
        if (!LoadCustomData(documentRoot, threadPool))
            return false;

        LoadModels(documentRoot["models"], scenePath, threadPool);
        LoadSceneGraph(documentRoot["graph"], rootNode);
        LoadAnimations(documentRoot["animations"]);
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
    return m_GltfImporter->Load(fileName, *m_TextureCache, g_LoadingStats, threadPool, result, m_textureSearchDirectory);
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

    auto rootNode = std::make_shared<SceneGraphNode>();
    rootNode->SetName(mesh->name);

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

    auto meshInstance = m_SceneTypeFactory->CreateMeshInstance(mesh);
    rootNode->SetLeaf(meshInstance);
    result.rootNode = rootNode;
    return result;
}

void Scene::LoadSceneGraph(const Json::Value& nodeList, const std::shared_ptr<SceneGraphNode>& parent)
{
    for (const auto& src : nodeList)
    {
        if (!src.isObject())
        {
            caustica::warning("Non-object node in the scene graph definition.");
            continue;
        }

        std::string nodeName;
        const auto& name = src["name"];
        if (name.isString())
        {
            nodeName = name.asString();
        }

        std::shared_ptr<SceneGraphNode> customParent = parent;
        const auto& parentNode = src["parent"];
        if (parentNode.isString())
        {
            customParent = m_SceneGraph->FindNode(parentNode.asString());
            if (!customParent)
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

        std::shared_ptr<SceneGraphNode> dst;

        const auto& modelNode = src["model"];
        if (!modelNode.isNull())
        {
            if (!modelNode.isIntegral())
            {
                caustica::warning("Model references in the scene graph must be indices into the model array.");
                continue;
            }

            int modelIndex = modelNode.asInt();
            if (modelIndex < 0 || modelIndex >= int(m_Models.size()))
            {
                caustica::warning("Referenced model %d is not defined in the model array.", modelIndex);
                continue;
            }

            const auto& loadedModel = m_Models[modelIndex];
            if (!loadedModel.rootNode)
            {
                continue;
            }

            dst = loadedModel.rootNode;
        }
        else
        {
            dst = std::make_shared<SceneGraphNode>();
        }

        dst = m_SceneGraph->Attach(customParent, dst);

        dst->SetName(nodeName);
        
        const auto& translation = src["translation"];
        if (!translation.isNull())
        {
            double3 value = double3::zero();
            translation >> value;
            dst->SetTranslation(value);
        }

        const auto& rotation = src["rotation"];
        if (!rotation.isNull())
        {
            double4 value = double4(0.0, 0.0, 0.0, 1.0);
            rotation >> value;
            dst->SetRotation(dm::dquat::fromXYZW(value));
        }
        else
        {
            const auto& euler = src["euler"];
            if (!euler.isNull())
            {
                double3 value = double3::zero();
                euler >> value;
                dst->SetRotation(rotationQuat(value));
            }
        }

        const auto& scaling = src["scaling"];
        if (!scaling.isNull())
        {
            double3 value = double3(1.0);
            scaling >> value;
            dst->SetScaling(value);
        }

        const auto& children = src["children"];
        if (!children.isNull())
        {
            LoadSceneGraph(children, dst);
        }

        const auto& leafTypeNode = src["type"];
        if (leafTypeNode.isString())
        {
            auto leaf = m_SceneTypeFactory->CreateLeaf(leafTypeNode.asString());
            if (leaf)
            {
                dst->SetLeaf(leaf);
                leaf->Load(src);
            }
            else
            {
                caustica::warning("Unknown leaf type '%s' for node '%s', skipping.",
                    leafTypeNode.asCString(), dst->GetName().c_str());
            }
        }
        else if (!leafTypeNode.isNull())
        {
            caustica::warning("Leaf type specification for node '%s' is not a string, skipping.",
                dst->GetName().c_str());
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
    std::shared_ptr<SceneGraphNode> animationContainer;

    for (const auto& animationNode : nodeList)
    {
        const auto& animation = std::make_shared<SceneGraphAnimation>();

        const auto& sceneAnimationNode = std::make_shared<SceneGraphNode>();
        sceneAnimationNode->SetLeaf(animation);

        const auto& nameNode = animationNode["name"];
        if (nameNode.isString())
        {
            animation->SetName(nameNode.asString());
        }

        const auto& channelsNode = animationNode["channels"];
        if (channelsNode.isArray())
        {
            int channelIndex = -1;
            for (const auto& channelSrc : channelsNode)
            {
                // Increment the index in the beginning because there are 'continue' statements below
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
                        caustica::warning("Unknown interpolation mode '%s' specified for animation '%s' channel %d. "
                            "Valid interpolation modes are: step, linear, hermite, catmull-rom.",
                            modeNode.asCString(), animation->GetName().c_str(), channelIndex);
                }
                else
                {
                    sampler->SetInterpolationMode(animation::InterpolationMode::Step);
                    caustica::warning("Interpolation mode is not specified for animation '%s' channel %d, using step.",
                        animation->GetName().c_str(), channelIndex);
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
                        animation->GetName().c_str(), channelIndex);
                    continue;
                }

                int keyframeIndex = -1;
                for (const auto& dataPoint : channelSrc["data"])
                {
                    ++keyframeIndex;

                    const auto& timeNode = dataPoint["time"];
                    if (!timeNode.isNumeric())
                    {
                        caustica::warning("Invalid keyframe %d in animation '%s' channel %d: time is not specified or is not numeric.",
                            keyframeIndex, animation->GetName().c_str(), channelIndex);
                        continue;
                    }

                    animation::Keyframe keyframe;
                    keyframe.time = timeNode.asFloat();
                    keyframe.value = ReadUpToFloat4(dataPoint["value"]);
                    keyframe.inTangent = ReadUpToFloat4(dataPoint["inTangent"]);
                    keyframe.outTangent = ReadUpToFloat4(dataPoint["outTangent"]);

                    sampler->AddKeyframe(keyframe);
                }

                auto processTargetNode = [this, &animation, &sampler, attribute, &attributeNode, channelIndex](const Json::Value& targetNode)
                {
                    if (targetNode.isString())
                    {
                        std::string targetName = targetNode.asString();
                        if (caustica::string_utils::starts_with(targetName, "material:"))
                        {
                            targetName = targetName.substr(9);

                            std::shared_ptr<Material> material;
                            for (const auto& it : m_SceneGraph->GetMaterials())
                            {
                                if (it->name == targetName)
                                {
                                    material = it;
                                    break;
                                }
                            }

                            if (material)
                            {
                                const auto& channel = std::make_shared<SceneGraphAnimationChannel>(sampler, material);
                                channel->SetLeafProperyName(attributeNode.asString());
                                animation->AddChannel(channel);
                            }
                            else
                            {
                                caustica::warning("Target material '%s' specified for animation '%s' channel %d not found, ignoring.",
                                    std::string(targetName).c_str(), animation->GetName().c_str(), channelIndex);
                            }
                        }
                        else
                        {
                            const auto& target = m_SceneGraph->FindNode(targetNode.asString());
                            if (target)
                            {
                                const auto& channel = std::make_shared<SceneGraphAnimationChannel>(sampler, target, attribute);
                                if (attribute == AnimationAttribute::LeafProperty)
                                    channel->SetLeafProperyName(attributeNode.asString());
                                animation->AddChannel(channel);
                            }
                            else
                            {
                                caustica::warning("Target node '%s' specified for animation '%s' channel %d not found, ignoring.",
                                    targetNode.asCString(), animation->GetName().c_str(), channelIndex);
                            }
                        }
                    }
                    else if (!targetNode.isNull())
                    {
                        caustica::warning("Target node specification for animation '%s' channel %d is not a string, ignoring.",
                            animation->GetName().c_str(), channelIndex);
                    }
                };

                const auto& targetNode = channelSrc["target"];
                if (!targetNode.isNull())
                {
                    processTargetNode(targetNode);
                }
                else
                {
                    const auto& targetsNode = channelSrc["targets"];
                    if (targetsNode.isArray())
                    {
                        for (const auto& targetArrayItem : targetsNode)
                        {
                            processTargetNode(targetArrayItem);
                        }
                    }
                }
            }
        }

        if (!animation->GetChannels().empty())
        {
            if (!animationContainer)
            {
                animationContainer = std::make_shared<SceneGraphNode>();
                animationContainer->SetName("Animations");
                m_SceneGraph->Attach(m_SceneGraph->GetRootNode(), animationContainer);
            }
            
            m_SceneGraph->Attach(animationContainer, sceneAnimationNode);
        }
        else
        {
            caustica::warning("Animation '%s' processed with no valid channels, ignoring.",
                animation->GetName().c_str());
        }
    }
}

bool Scene::LoadCustomData(Json::Value& rootNode, ThreadPool* threadPool)
{
    // Reserved for derived classes
    return true;
}

void Scene::FinishedLoading(uint32_t frameIndex)
{
    nvrhi::CommandListHandle commandList = m_Device->createCommandList();
    commandList->open();
    
    CreateMeshBuffers(commandList);
    Refresh(commandList, frameIndex);

    commandList->close();
    m_Device->executeCommandList(commandList);
}

void Scene::RefreshSceneGraph(uint32_t frameIndex)
{
    m_SceneStructureChanged = m_SceneGraph->HasPendingStructureChanges();
    m_SceneTransformsChanged = m_SceneGraph->HasPendingTransformChanges();
    m_SceneGraph->Refresh(frameIndex);
}

void Scene::RefreshBuffers(nvrhi::ICommandList* commandList, uint32_t frameIndex)
{
    bool materialsChanged = false;

    if (m_SceneStructureChanged)
        CreateMeshBuffers(commandList);

    const size_t allocationGranularity = 1024;
    bool arraysAllocated = false;

    if (m_EnableBindlessResources && m_SceneGraph->GetGeometryCount() > m_Resources->geometryData.size())
    {
        m_Resources->geometryData.resize(nvrhi::align<size_t>(m_SceneGraph->GetGeometryCount(), allocationGranularity));
        m_GeometryBuffer = CreateGeometryBuffer();
        arraysAllocated = true;
    }

    if (m_SceneGraph->GetMaterials().size() > m_Resources->materialData.size())
    {
        m_Resources->materialData.resize(nvrhi::align<size_t>(m_SceneGraph->GetMaterials().size(), allocationGranularity));
        if (m_EnableBindlessResources)
            m_MaterialBuffer = CreateMaterialBuffer();
        arraysAllocated = true;
    }

    if (m_SceneGraph->GetMeshInstances().size() > m_Resources->instanceData.size())
    {
        m_Resources->instanceData.resize(nvrhi::align<size_t>(m_SceneGraph->GetMeshInstances().size(), allocationGranularity));
        m_InstanceBuffer = CreateInstanceBuffer();
        arraysAllocated = true;
    }

    for (const auto& material : m_SceneGraph->GetMaterials())
    {
        if (material->dirty || m_SceneStructureChanged || arraysAllocated)
            UpdateMaterial(material);

        if (!material->materialConstants)
        {
            material->materialConstants = CreateMaterialConstantBuffer(material->name);
            material->dirty = true;
        }

        if (material->dirty)
        {
            commandList->writeBuffer(material->materialConstants,
                &m_Resources->materialData[material->materialID],
                sizeof(MaterialConstants));

            material->dirty = false;
            materialsChanged = true;
        }
    }

    if (!m_Resources->geometryData.empty())
    {
        uint32_t geometryResourceIndex = 0;
        for (const auto& mesh : m_SceneGraph->GetMeshes())
        {
            if (arraysAllocated)
            {
                break;
            }

            for (const auto& geometry : mesh->geometries)
            {
                if (geometry->numIndices != m_Resources->geometryData[geometryResourceIndex].numIndices)
                {
                    arraysAllocated = true;
                    break;
                }
                ++geometryResourceIndex;
            }
        }
    }

    if (m_SceneStructureChanged || arraysAllocated)
    {
        for (const auto& mesh : m_SceneGraph->GetMeshes())
        {
            mesh->buffers->instanceBuffer = m_InstanceBuffer;

            if (m_EnableBindlessResources)
                UpdateGeometry(mesh);
        }

        if (m_EnableBindlessResources)
            WriteGeometryBuffer(commandList);
    }

    if (m_SceneStructureChanged || m_SceneTransformsChanged || arraysAllocated)
    {
        for (const auto& instance : m_SceneGraph->GetMeshInstances())
        {
            UpdateInstance(instance);
        }

        WriteInstanceBuffer(commandList);
    }

    if (m_EnableBindlessResources && (materialsChanged || m_SceneStructureChanged || arraysAllocated))
    {
        WriteMaterialBuffer(commandList);
    }

    UpdateSkinnedMeshes(commandList, frameIndex);
}

void Scene::UpdateSkinnedMeshes(nvrhi::ICommandList* commandList, uint32_t frameIndex)
{
    bool skinningMarkerPlaced = false;

    std::vector<dm::float4x4> jointMatrices;
    for (const auto& skinnedInstance : m_SceneGraph->GetSkinnedMeshInstances())
    {
        // Only process the groups that were updated on this or previous frame.
        // Previous frame updates should be processed to copy the current positions to the previous buffer.
        if (skinnedInstance->GetLastUpdateFrameIndex() + 1 < frameIndex)
            continue;

        if (!skinningMarkerPlaced)
        {
            commandList->beginMarker("Skinning");
            skinningMarkerPlaced = true;
        }

        const auto& groupName = skinnedInstance->GetName();
        if (!groupName.empty())
            commandList->beginMarker(groupName.c_str());

        jointMatrices.resize(skinnedInstance->joints.size());
        dm::daffine3 worldToRoot = inverse(skinnedInstance->GetNode()->GetLocalToWorldTransform());

        for (size_t i = 0; i < skinnedInstance->joints.size(); i++)
        {
            auto jointNode = skinnedInstance->joints[i].node.lock();

            dm::float4x4 jointMatrix = dm::affineToHomogeneous(dm::affine3(jointNode->GetLocalToWorldTransform() * worldToRoot));
            jointMatrix = skinnedInstance->joints[i].inverseBindMatrix * jointMatrix;
            jointMatrices[i] = jointMatrix;
        }

        commandList->writeBuffer(skinnedInstance->jointBuffer, jointMatrices.data(), jointMatrices.size() * sizeof(float4x4));

        nvrhi::ComputeState state;
        state.pipeline = m_SkinningPipeline;
        state.bindings = { skinnedInstance->skinningBindingSet };
        commandList->setComputeState(state);

        uint32_t vertexOffset = skinnedInstance->GetPrototypeMesh()->vertexOffset;
        const auto& prototypeBuffers = skinnedInstance->GetPrototypeMesh()->buffers;
        const auto& skinnedBuffers = skinnedInstance->GetMesh()->buffers;

        SkinningConstants constants{};
        constants.numVertices = skinnedInstance->GetPrototypeMesh()->totalVertices;

        constants.flags = 0;
        if (prototypeBuffers->hasAttribute(VertexAttribute::Normal)) constants.flags |= SkinningFlag_Normals;
        if (prototypeBuffers->hasAttribute(VertexAttribute::Tangent)) constants.flags |= SkinningFlag_Tangents;
        if (prototypeBuffers->hasAttribute(VertexAttribute::TexCoord1)) constants.flags |= SkinningFlag_TexCoord1;
        if (prototypeBuffers->hasAttribute(VertexAttribute::TexCoord2)) constants.flags |= SkinningFlag_TexCoord2;
        if (!skinnedInstance->skinningInitialized) constants.flags |= SkinningFlag_FirstFrame;
        skinnedInstance->skinningInitialized = true;

        constants.inputPositionOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::Position).byteOffset + vertexOffset * sizeof(float3));
        constants.inputNormalOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset + vertexOffset * sizeof(uint32_t));
        constants.inputTangentOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset + vertexOffset * sizeof(uint32_t));
        constants.inputTexCoord1Offset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset + vertexOffset * sizeof(float2));
        constants.inputTexCoord2Offset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::TexCoord2).byteOffset + vertexOffset * sizeof(float2));
        constants.inputJointIndexOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::JointIndices).byteOffset + vertexOffset * sizeof(uint2));
        constants.inputJointWeightOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::JointWeights).byteOffset + vertexOffset * sizeof(float4));
        constants.outputPositionOffset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::Position).byteOffset);
        constants.outputPrevPositionOffset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset);
        constants.outputNormalOffset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset);
        constants.outputTangentOffset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset);
        constants.outputTexCoord1Offset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset);
        constants.outputTexCoord2Offset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::TexCoord2).byteOffset);
        commandList->setPushConstants(&constants, sizeof(constants));

        commandList->dispatch(dm::div_ceil(constants.numVertices, 256));

        if (!groupName.empty())
            commandList->endMarker();
    }

    if (skinningMarkerPlaced)
    {
        commandList->endMarker();
    }
}

void Scene::Refresh(nvrhi::ICommandList* commandList, uint32_t frameIndex)
{
    RefreshSceneGraph(frameIndex);
    RefreshBuffers(commandList, frameIndex);
}


nvrhi::BufferHandle CreateMaterialConstantBuffer(nvrhi::IDevice* device, const std::string& debugName, bool isVirtual)
{
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(MaterialConstants);
    bufferDesc.debugName = debugName;
    bufferDesc.isConstantBuffer = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    bufferDesc.keepInitialState = true;
    bufferDesc.isVirtual = isVirtual;

    return device->createBuffer(bufferDesc);
}


inline void AppendBufferRange(nvrhi::BufferRange& range, size_t size, uint64_t& currentBufferSize)
{
    range.byteOffset = currentBufferSize;
    range.byteSize = nvrhi::align(size, size_t(16));
    currentBufferSize += range.byteSize;
}

void Scene::CreateMeshBuffers(nvrhi::ICommandList* commandList)
{
    for (const auto& mesh : m_SceneGraph->GetMeshes())
    {
        auto buffers = mesh->buffers;

        if (!buffers)
            continue;

        if (!buffers->indexData.empty() && !buffers->indexBuffer)
        {
            nvrhi::BufferDesc bufferDesc;
            bufferDesc.isIndexBuffer = true;
            bufferDesc.byteSize = buffers->indexData.size() * sizeof(uint32_t);
            bufferDesc.debugName = "IndexBuffer";
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = true;
            bufferDesc.format = nvrhi::Format::R32_UINT;
            bufferDesc.isAccelStructBuildInput = m_RayTracingSupported;

            buffers->indexBuffer = m_Device->createBuffer(bufferDesc);

            if (m_DescriptorTable)
            {
                buffers->indexBufferDescriptor = std::make_shared<DescriptorHandle>(m_DescriptorTable->CreateDescriptorHandle(
                    nvrhi::BindingSetItem::RawBuffer_SRV(0, buffers->indexBuffer)));
            }

            commandList->beginTrackingBufferState(buffers->indexBuffer, nvrhi::ResourceStates::Common);

            commandList->writeBuffer(buffers->indexBuffer, buffers->indexData.data(), buffers->indexData.size() * sizeof(uint32_t));

            nvrhi::ResourceStates state = nvrhi::ResourceStates::IndexBuffer | nvrhi::ResourceStates::ShaderResource;

            if (bufferDesc.isAccelStructBuildInput)
                state = state | nvrhi::ResourceStates::AccelStructBuildInput;

            commandList->setPermanentBufferState(buffers->indexBuffer, state);
            commandList->commitBarriers();
        }

        if (!buffers->vertexBuffer)
        {
            nvrhi::BufferDesc bufferDesc;
            bufferDesc.isVertexBuffer = true;
            bufferDesc.byteSize = 0;
            bufferDesc.debugName = "VertexBuffer";
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = true;
            bufferDesc.isAccelStructBuildInput = m_RayTracingSupported;

            nvrhi::ResourceStates state = nvrhi::ResourceStates::VertexBuffer | nvrhi::ResourceStates::ShaderResource;
            if (bufferDesc.isAccelStructBuildInput)
                state = state | nvrhi::ResourceStates::AccelStructBuildInput;
            bufferDesc.initialState = state;
            bufferDesc.keepInitialState = true;

            if (!buffers->positionData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::Position), 
                    buffers->positionData.size() * sizeof(buffers->positionData[0]), bufferDesc.byteSize);
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::PrevPosition),
                    buffers->positionData.size() * sizeof(buffers->positionData[0]), bufferDesc.byteSize);
            }

            if (!buffers->normalData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::Normal),
                    buffers->normalData.size() * sizeof(buffers->normalData[0]), bufferDesc.byteSize);
            }

            if (!buffers->tangentData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::Tangent),
                    buffers->tangentData.size() * sizeof(buffers->tangentData[0]), bufferDesc.byteSize);
            }

            if (!buffers->texcoord1Data.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::TexCoord1),
                    buffers->texcoord1Data.size() * sizeof(buffers->texcoord1Data[0]), bufferDesc.byteSize);
            }

            if (!buffers->texcoord2Data.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::TexCoord2),
                    buffers->texcoord2Data.size() * sizeof(buffers->texcoord2Data[0]), bufferDesc.byteSize);
            }

            if (!buffers->weightData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::JointWeights),
                    buffers->weightData.size() * sizeof(buffers->weightData[0]), bufferDesc.byteSize);
            }

            if (!buffers->jointData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::JointIndices),
                    buffers->jointData.size() * sizeof(buffers->jointData[0]), bufferDesc.byteSize);
            }

            if (!buffers->radiusData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::CurveRadius),
                    buffers->radiusData.size() * sizeof(buffers->radiusData[0]), bufferDesc.byteSize);
            }

            if (bufferDesc.byteSize == 0)
            {
	            continue;
            }

            buffers->vertexBuffer = m_Device->createBuffer(bufferDesc);
            if (m_DescriptorTable)
            {
                buffers->vertexBufferDescriptor = std::make_shared<DescriptorHandle>(
                    m_DescriptorTable->CreateDescriptorHandle(nvrhi::BindingSetItem::RawBuffer_SRV(0, buffers->vertexBuffer)));
            }

            commandList->beginTrackingBufferState(buffers->vertexBuffer, nvrhi::ResourceStates::Common);

            if (!buffers->positionData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::Position);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->positionData.data(), range.byteSize, range.byteOffset);

                const auto& prevRange = buffers->getVertexBufferRange(VertexAttribute::PrevPosition);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->positionData.data(), prevRange.byteSize, prevRange.byteOffset);
            }

            if (!buffers->normalData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::Normal);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->normalData.data(), range.byteSize, range.byteOffset);
            }

            if (!buffers->tangentData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::Tangent);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->tangentData.data(), range.byteSize, range.byteOffset);
            }

            if (!buffers->texcoord1Data.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::TexCoord1);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->texcoord1Data.data(), range.byteSize, range.byteOffset);
            }

            if (!buffers->texcoord2Data.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::TexCoord2);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->texcoord2Data.data(), range.byteSize, range.byteOffset);
            }

            if (!buffers->weightData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::JointWeights);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->weightData.data(), range.byteSize, range.byteOffset);
            }

            if (!buffers->jointData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::JointIndices);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->jointData.data(), range.byteSize, range.byteOffset);
            }

            if (!buffers->radiusData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::CurveRadius);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->radiusData.data(), range.byteSize, range.byteOffset);
            }

            commandList->setBufferState(buffers->vertexBuffer, state);
            commandList->commitBarriers();
        }
    }

    for (const auto& skinnedInstance : m_SceneGraph->GetSkinnedMeshInstances())
    {
        const auto& skinnedMesh = skinnedInstance->GetMesh();

        if (!skinnedMesh->buffers)
        {
            skinnedMesh->buffers = std::make_shared<BufferGroup>();

            uint32_t totalVertices = skinnedMesh->totalVertices;

            skinnedMesh->buffers->indexBuffer = skinnedInstance->GetPrototypeMesh()->buffers->indexBuffer;
            skinnedMesh->buffers->indexBufferDescriptor = skinnedInstance->GetPrototypeMesh()->buffers->indexBufferDescriptor;

            const auto& prototypeBuffers = skinnedInstance->GetPrototypeMesh()->buffers;
            const auto& skinnedBuffers = skinnedMesh->buffers;

            size_t skinnedVertexBufferSize = 0;
            assert(prototypeBuffers->hasAttribute(VertexAttribute::Position));

            AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::Position),
                totalVertices * sizeof(float3), skinnedVertexBufferSize);
    
            AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::PrevPosition),
                totalVertices * sizeof(float3), skinnedVertexBufferSize);
            
            if(prototypeBuffers->hasAttribute(VertexAttribute::Normal))
            {
                AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::Normal),
                    totalVertices * sizeof(uint32_t), skinnedVertexBufferSize);
            }

            if (prototypeBuffers->hasAttribute(VertexAttribute::Tangent))
            {
                AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::Tangent),
                    totalVertices * sizeof(uint32_t), skinnedVertexBufferSize);
            }

            if (prototypeBuffers->hasAttribute(VertexAttribute::TexCoord1))
            {
                AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::TexCoord1),
                    totalVertices * sizeof(float2), skinnedVertexBufferSize);
            }

            if (prototypeBuffers->hasAttribute(VertexAttribute::TexCoord2))
            {
                AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::TexCoord2),
                    totalVertices * sizeof(float2), skinnedVertexBufferSize);
            }

            nvrhi::BufferDesc bufferDesc;
            bufferDesc.isVertexBuffer = true;
            bufferDesc.byteSize = skinnedVertexBufferSize;
            bufferDesc.debugName = "SkinnedVertexBuffer";
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = true;
            bufferDesc.canHaveUAVs = true;
            bufferDesc.isAccelStructBuildInput = m_RayTracingSupported;
            bufferDesc.keepInitialState = true;
            bufferDesc.initialState = nvrhi::ResourceStates::VertexBuffer;

            skinnedBuffers->vertexBuffer = m_Device->createBuffer(bufferDesc);

            if (m_DescriptorTable)
            {
                skinnedBuffers->vertexBufferDescriptor = std::make_shared<DescriptorHandle>(
                    m_DescriptorTable->CreateDescriptorHandle(nvrhi::BindingSetItem::RawBuffer_SRV(0, skinnedBuffers->vertexBuffer)));
            }
        }

        if (!skinnedInstance->jointBuffer)
        {
            nvrhi::BufferDesc jointBufferDesc;
            jointBufferDesc.debugName = "JointBuffer";
            jointBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            jointBufferDesc.keepInitialState = true;
            jointBufferDesc.canHaveRawViews = true;
            jointBufferDesc.byteSize = sizeof(dm::float4x4) * skinnedInstance->joints.size();
            skinnedInstance->jointBuffer = m_Device->createBuffer(jointBufferDesc);
        }

        if (!skinnedInstance->skinningBindingSet)
        {
            const auto& prototypeBuffers = skinnedInstance->GetPrototypeMesh()->buffers;
            const auto& skinnedBuffers = skinnedInstance->GetMesh()->buffers;
            
            nvrhi::BindingSetDesc setDesc;
            setDesc.bindings = {
                nvrhi::BindingSetItem::PushConstants(0, sizeof(SkinningConstants)),
                nvrhi::BindingSetItem::RawBuffer_SRV(0, prototypeBuffers->vertexBuffer),
                nvrhi::BindingSetItem::RawBuffer_SRV(1, skinnedInstance->jointBuffer),
                nvrhi::BindingSetItem::RawBuffer_UAV(0, skinnedBuffers->vertexBuffer)
            };

            skinnedInstance->skinningBindingSet = m_Device->createBindingSet(setDesc, m_SkinningBindingLayout);
        }
    }
}

nvrhi::BufferHandle Scene::CreateMaterialBuffer()
{
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(MaterialConstants) * m_Resources->materialData.size();
    bufferDesc.debugName = "BindlessMaterials";
    bufferDesc.structStride = sizeof(MaterialConstants);
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    return m_Device->createBuffer(bufferDesc);
}

nvrhi::BufferHandle Scene::CreateGeometryBuffer()
{
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(GeometryData) * m_Resources->geometryData.size();
    bufferDesc.debugName = "BindlessGeometry";
    bufferDesc.structStride = sizeof(GeometryData);
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    return m_Device->createBuffer(bufferDesc);
}

nvrhi::BufferHandle Scene::CreateInstanceBuffer()
{
    // On DX11, a buffer cannot be both structured and vertex.
    // On other APIs, a structured instance buffer can be used for rasterization.
    bool const needStructuredBuffer = m_Device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11;

    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(InstanceData) * m_Resources->instanceData.size();
    bufferDesc.debugName = "Instances";
    bufferDesc.structStride = needStructuredBuffer ? sizeof(InstanceData) : 0;
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.isVertexBuffer = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    return m_Device->createBuffer(bufferDesc);
}

nvrhi::BufferHandle Scene::CreateMaterialConstantBuffer(const std::string& debugName)
{
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(MaterialConstants);
    bufferDesc.debugName = debugName;
    bufferDesc.isConstantBuffer = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    bufferDesc.keepInitialState = true;

    return m_Device->createBuffer(bufferDesc);
}

void Scene::WriteMaterialBuffer(nvrhi::ICommandList* commandList) const
{
    commandList->writeBuffer(m_MaterialBuffer, m_Resources->materialData.data(),
        m_Resources->materialData.size() * sizeof(MaterialConstants));
}

void Scene::WriteGeometryBuffer(nvrhi::ICommandList* commandList) const
{
    commandList->writeBuffer(m_GeometryBuffer, m_Resources->geometryData.data(),
        m_Resources->geometryData.size() * sizeof(GeometryData));
}

void Scene::WriteInstanceBuffer(nvrhi::ICommandList* commandList) const
{
    commandList->writeBuffer(m_InstanceBuffer, m_Resources->instanceData.data(), 
        m_Resources->instanceData.size() * sizeof(InstanceData));
}

void Scene::UpdateMaterial(const std::shared_ptr<Material>& material)
{
    material->FillConstantBuffer(m_Resources->materialData[material->materialID], m_UseResourceDescriptorHeapBindless);
}

void Scene::UpdateGeometry(const std::shared_ptr<MeshInfo>& mesh)
{
    // TODO: support 64-bit buffer offsets in the CB.
    for (const auto& geometry : mesh->geometries)
    {
        uint32_t indexOffset = mesh->indexOffset + geometry->indexOffsetInMesh;
        uint32_t vertexOffset = mesh->vertexOffset + geometry->vertexOffsetInMesh;

        GeometryData& gdata = m_Resources->geometryData[geometry->globalGeometryIndex];
        gdata.numIndices = geometry->numIndices;
        gdata.numVertices = geometry->numVertices;
        gdata.indexBufferIndex = mesh->buffers->indexBufferDescriptor ? mesh->buffers->indexBufferDescriptor->Get() : -1;
        gdata.indexOffset = indexOffset * sizeof(uint32_t);
        gdata.vertexBufferIndex = mesh->buffers->vertexBufferDescriptor ? mesh->buffers->vertexBufferDescriptor->Get() : -1;
        gdata.positionOffset = mesh->buffers->hasAttribute(VertexAttribute::Position)
            ? uint32_t(vertexOffset * sizeof(float3) + mesh->buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset) : ~0u;
        gdata.prevPositionOffset = mesh->buffers->hasAttribute(VertexAttribute::PrevPosition)
            ? uint32_t(vertexOffset * sizeof(float3) + mesh->buffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset) : ~0u;
        gdata.texCoord1Offset = mesh->buffers->hasAttribute(VertexAttribute::TexCoord1)
            ? uint32_t(vertexOffset * sizeof(float2) + mesh->buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset) : ~0u;
        gdata.texCoord2Offset = mesh->buffers->hasAttribute(VertexAttribute::TexCoord2)
            ? uint32_t(vertexOffset * sizeof(float2) + mesh->buffers->getVertexBufferRange(VertexAttribute::TexCoord2).byteOffset) : ~0u;
        gdata.normalOffset = mesh->buffers->hasAttribute(VertexAttribute::Normal)
            ? uint32_t(vertexOffset * sizeof(uint32_t) + mesh->buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset) : ~0u;
        gdata.tangentOffset = mesh->buffers->hasAttribute(VertexAttribute::Tangent)
            ? uint32_t(vertexOffset * sizeof(uint32_t) + mesh->buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset) : ~0u;
        gdata.curveRadiusOffset = mesh->buffers->hasAttribute(VertexAttribute::CurveRadius)
            ? uint32_t(vertexOffset * sizeof(float) + mesh->buffers->getVertexBufferRange(VertexAttribute::CurveRadius).byteOffset) : ~0u;
        gdata.materialIndex = geometry->material ? geometry->material->materialID : ~0u;
    }
}

GeometryData* Scene::GetGeometryData(const MeshGeometry& geometry) const
{
    if (m_Resources == nullptr || uint(geometry.globalGeometryIndex) >= m_Resources->geometryData.size() )
        return nullptr;

    return &m_Resources->geometryData[geometry.globalGeometryIndex];
}


void Scene::UpdateInstance(const std::shared_ptr<MeshInstance>& instance)
{
    SceneGraphNode* node = instance->GetNode();
    if (!node)
        return;

    InstanceData& idata = m_Resources->instanceData[instance->GetInstanceIndex()];
    affineToColumnMajor(node->GetLocalToWorldTransformFloat(), idata.transform);
    affineToColumnMajor(node->GetPrevLocalToWorldTransformFloat(), idata.prevTransform);

    const auto& mesh = instance->GetMesh();
    idata.firstGeometryInstanceIndex = instance->GetGeometryInstanceIndex();
    idata.numGeometries = uint32_t(mesh->geometries.size());
    idata.firstGeometryIndex = idata.numGeometries > 0 ? mesh->geometries[0]->globalGeometryIndex : -1;
    idata.flags = 0u;

    if (mesh->type == MeshType::CurveDisjointOrthogonalTriangleStrips)
    {
        idata.flags |= InstanceFlags_CurveDisjointOrthogonalTriangleStrips;
    }
    else if (mesh->type == MeshType::CurveLinearSweptSpheres)
    {
        // NVAPI does not support Vulkan, so NvRtIsLssHit() cannot be used to detect LSS hits.
        // Instead, we explicitly mark each LSS instance with a flag and check this flag during hit processing.
        // For consistency and completeness, this flag is also set for DX12.
        idata.flags |= InstanceFlags_CurveLinearSweptSpheres;
    }
}
