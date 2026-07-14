#include <scene/SceneResources.h>  // provides complete SceneTypeFactory; transitively includes SceneObjects.h
#include <core/json.h>

using namespace caustica;

// =============================================================================
// MeshInstance
// =============================================================================

std::shared_ptr<MeshInstance> MeshInstance::clone() const
{
    auto copy = std::make_shared<MeshInstance>(m_Mesh);
    copy->name = name;
    copy->ownerEntity = ownerEntity;
    copy->PerGeometryLightSamplerLinks = PerGeometryLightSamplerLinks;
    copy->ProxiedAnalyticLight = ProxiedAnalyticLight;
    return copy;
}

SceneContentFlags MeshInstance::getContentFlags() const
{
    if (!m_Mesh)
        return SceneContentFlags::None;

    SceneContentFlags flags = SceneContentFlags::None;

    for (const auto& geometry : m_Mesh->geometries)
    {
        if (!geometry->material)
            continue;

        switch (geometry->material->domain)  // NOLINT(clang-diagnostic-switch-enum)
        {
        case MaterialDomain::Opaque:
            flags |= SceneContentFlags::OpaqueMeshes;
            break;
        case MaterialDomain::AlphaTested:
            flags |= SceneContentFlags::AlphaTestedMeshes;
            break;
        default:
            flags |= SceneContentFlags::BlendedMeshes;
            break;
        }
    }

    return flags;
}

bool MeshInstance::setProperty(const std::string& propName, const dm::float4& value)
{
    if (m_Mesh && m_Mesh->geometries.size() == 1 && m_Mesh->geometries[0]->material) // TODO: support targeting a specific geometry
        return m_Mesh->geometries[0]->material->setProperty(propName, value);

    return false;
}

// =============================================================================
// SkinnedMeshInstance
// =============================================================================

SkinnedMeshInstance::SkinnedMeshInstance(std::shared_ptr<SceneTypeFactory> sceneTypeFactory,
                                         std::shared_ptr<MeshInfo> prototypeMesh)
    : MeshInstance(nullptr)
    , m_SceneTypeFactory(std::move(sceneTypeFactory))
{
    m_PrototypeMesh = std::move(prototypeMesh);

    auto skinnedMesh              = m_SceneTypeFactory->createMesh();
    skinnedMesh->skinPrototype    = m_PrototypeMesh;
    skinnedMesh->name             = m_PrototypeMesh->name;
    skinnedMesh->objectSpaceBounds = m_PrototypeMesh->objectSpaceBounds;
    skinnedMesh->indexOffset      = m_PrototypeMesh->indexOffset;
    skinnedMesh->totalVertices    = m_PrototypeMesh->totalVertices;
    skinnedMesh->totalIndices     = m_PrototypeMesh->totalIndices;
    skinnedMesh->geometries.reserve(m_PrototypeMesh->geometries.size());

    for (const auto& geometry : m_PrototypeMesh->geometries)
    {
        auto newGeometry = m_SceneTypeFactory->createMeshGeometry();
        *newGeometry = *geometry;
        skinnedMesh->geometries.push_back(std::move(newGeometry));
    }

    m_Mesh = skinnedMesh;
    PerGeometryLightSamplerLinks.resize(skinnedMesh->geometries.size(), { -1, -1 });
}

std::shared_ptr<MeshInstance> SkinnedMeshInstance::clone() const
{
    auto copy = std::make_shared<SkinnedMeshInstance>(m_SceneTypeFactory, m_PrototypeMesh);
    copy->name = name;
    copy->ownerEntity = ownerEntity;
    for (const auto& joint : joints)
        copy->joints.push_back(joint);
    return copy;
}

// =============================================================================
// SkinnedMeshReference
// =============================================================================

std::shared_ptr<SkinnedMeshReference> SkinnedMeshReference::clone() const
{
    auto copy = std::make_shared<SkinnedMeshReference>(m_Instance.lock());
    copy->name = name;
    return copy;
}

// =============================================================================
// PerspectiveCamera
// =============================================================================

std::shared_ptr<PerspectiveCamera> PerspectiveCamera::clone() const
{
    auto copy = std::make_shared<PerspectiveCamera>();
    copy->name                 = name;
    copy->zNear                = zNear;
    copy->zFar                 = zFar;
    copy->verticalFov          = verticalFov;
    copy->aspectRatio          = aspectRatio;
    copy->enableAutoExposure   = enableAutoExposure;
    copy->exposureCompensation = exposureCompensation;
    copy->exposureValue        = exposureValue;
    copy->exposureValueMin     = exposureValueMin;
    copy->exposureValueMax     = exposureValueMax;
    return copy;
}

void PerspectiveCamera::load(const Json::Value& node)
{
    node["verticalFov"]          >> verticalFov;
    node["aspectRatio"]          >> aspectRatio;
    node["zNear"]                >> zNear;
    node["zFar"]                 >> zFar;
    node["enableAutoExposure"]   >> enableAutoExposure;
    node["exposureCompensation"] >> exposureCompensation;
    node["exposureValue"]        >> exposureValue;
    node["exposureValueMin"]     >> exposureValueMin;
    node["exposureValueMax"]     >> exposureValueMax;
}

bool PerspectiveCamera::setProperty(const std::string& propName, const dm::float4& value)
{
    if (propName == "zNear")       { zNear       = value.x; return true; }
    if (propName == "zFar")        { zFar        = value.x; return true; }
    if (propName == "verticalFov") { verticalFov = value.x; return true; }
    if (propName == "aspectRatio") { aspectRatio = value.x; return true; }
    return false;
}

// =============================================================================
// OrthographicCamera
// =============================================================================

std::shared_ptr<OrthographicCamera> OrthographicCamera::clone() const
{
    auto copy = std::make_shared<OrthographicCamera>();
    copy->name  = name;
    copy->zNear = zNear;
    copy->zFar  = zFar;
    copy->xMag  = xMag;
    copy->yMag  = yMag;
    return copy;
}

void OrthographicCamera::load(const Json::Value& node)
{
    node["xMag"]  >> xMag;
    node["yMag"]  >> yMag;
    node["zNear"] >> zNear;
    node["zFar"]  >> zFar;
}

bool OrthographicCamera::setProperty(const std::string& propName, const dm::float4& value)
{
    if (propName == "zNear") { zNear = value.x; return true; }
    if (propName == "zFar")  { zFar  = value.x; return true; }
    if (propName == "xMag")  { xMag  = value.x; return true; }
    if (propName == "yMag")  { yMag  = value.x; return true; }
    return false;
}

// =============================================================================
// EnvironmentLight
// =============================================================================

std::shared_ptr<EnvironmentLight> EnvironmentLight::clone() const
{
    auto copy = std::make_shared<EnvironmentLight>();
    copy->name          = name;
    copy->color         = color;
    copy->radianceScale = radianceScale;
    copy->textureIndex  = textureIndex;
    copy->rotation      = rotation;
    copy->path          = path;
    copy->LightLink     = LightLink;
    copy->Proxies       = Proxies;
    return copy;
}

void EnvironmentLight::load(const Json::Value& node)
{
    node["radianceScale"] >> radianceScale;
    node["textureIndex"]  >> textureIndex;
    node["rotation"]      >> rotation;
    node["path"]          >> path;
}

// =============================================================================
// GaussianSplat
// =============================================================================

std::shared_ptr<GaussianSplat> GaussianSplat::clone() const
{
    auto copy = std::make_shared<GaussianSplat>();
    copy->name             = name;
    copy->path             = path;
    copy->resolvedPath     = resolvedPath;
    copy->convertRdfToRub  = convertRdfToRub;
    copy->enabled          = enabled;
    copy->loadedSplatCount = loadedSplatCount;
    return copy;
}

void GaussianSplat::load(const Json::Value& node)
{
    node["path"] >> path;
    if (path.empty()) node["file"]     >> path;
    if (path.empty()) node["fileName"] >> path;
    node["convertRdfToRub"] >> convertRdfToRub;
    if (!node.isMember("convertRdfToRub") && node.isMember("convertRdfToDonut"))
        node["convertRdfToDonut"] >> convertRdfToRub;
    node["enabled"] >> enabled;
}

// =============================================================================
// SampleSettings
// =============================================================================

std::shared_ptr<SampleSettings> SampleSettings::clone() const
{
    auto copy = std::make_shared<SampleSettings>();
    copy->name                  = name;
    copy->realtimeMode          = realtimeMode;
    copy->enableAnimations      = enableAnimations;
    copy->startingCamera        = startingCamera;
    copy->realtimeFireflyFilter = realtimeFireflyFilter;
    copy->maxBounces            = maxBounces;
    copy->maxDiffuseBounces     = maxDiffuseBounces;
    copy->textureMIPBias        = textureMIPBias;
    return copy;
}

void SampleSettings::load(const Json::Value& node)
{
    node["realtimeMode"]          >> realtimeMode;
    node["enableAnimations"]      >> enableAnimations;
    node["startingCamera"]        >> startingCamera;
    node["realtimeFireflyFilter"] >> realtimeFireflyFilter;
    node["maxBounces"]            >> maxBounces;
    node["maxDiffuseBounces"]     >> maxDiffuseBounces;
    node["textureMIPBias"]        >> textureMIPBias;
}

// =============================================================================
// GameSettings
// =============================================================================

std::shared_ptr<GameSettings> GameSettings::clone() const
{
    auto copy = std::make_shared<GameSettings>();
    copy->name      = name;
    copy->m_JsonData = m_JsonData;
    return copy;
}

void GameSettings::load(const Json::Value& node)
{
    Json::StreamWriterBuilder writer;
    m_JsonData = Json::writeString(writer, node);
}

// =============================================================================
// SceneTypeFactory
// =============================================================================

std::shared_ptr<void> SceneTypeFactory::createLeaf(const std::string& type)
{
    if (type == "DirectionalLight")
        return std::make_shared<DirectionalLight>();
    if (type == "PointLight")
        return std::make_shared<PointLight>();
    if (type == "SpotLight")
        return std::make_shared<SpotLight>();
    if (type == "EnvironmentLight")
        return std::make_shared<EnvironmentLight>();
    if (type == "PerspectiveCamera" || type == "PerspectiveCameraEx")
        return std::make_shared<PerspectiveCamera>();
    if (type == "OrthographicCamera")
        return std::make_shared<OrthographicCamera>();
    if (type == "GaussianSplat" || type == "GaussianSplats" || type == "3DGaussianSplat")
        return std::make_shared<GaussianSplat>();
    if (type == "SampleSettings")
        return std::make_shared<SampleSettings>();
    if (type == "GameSettings")
        return std::make_shared<GameSettings>();
    // Legacy type no longer supported.
    return nullptr;
}

std::shared_ptr<Material> SceneTypeFactory::createMaterial()
{
    return std::make_shared<Material>();
}

std::shared_ptr<MeshInfo> SceneTypeFactory::createMesh()
{
    return std::make_shared<MeshInfo>();
}

std::shared_ptr<MeshGeometry> SceneTypeFactory::createMeshGeometry()
{
    return std::make_shared<MeshGeometry>();
}

std::shared_ptr<MeshInstance> SceneTypeFactory::createMeshInstance(const std::shared_ptr<MeshInfo>& mesh)
{
    return std::make_shared<MeshInstance>(mesh);
}

std::shared_ptr<SkinnedMeshInstance> SceneTypeFactory::createSkinnedMeshInstance(
    const std::shared_ptr<SceneTypeFactory>& sceneTypeFactory,
    const std::shared_ptr<MeshInfo>& prototypeMesh)
{
    return std::make_shared<SkinnedMeshInstance>(sceneTypeFactory, prototypeMesh);
}
