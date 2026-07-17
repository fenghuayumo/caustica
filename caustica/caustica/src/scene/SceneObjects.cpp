#include <scene/SceneResources.h>  // provides complete SceneTypeFactory; transitively includes SceneObjects.h
#include <core/json.h>

using namespace caustica;

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
    if (type == "GaussianSplat" || type == "GaussianSplats" || type == "3DGaussianSplat")
        return std::make_shared<GaussianSplat>();
    if (type == "SampleSettings")
        return std::make_shared<SampleSettings>();
    if (type == "GameSettings")
        return std::make_shared<GameSettings>();
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
