#include <scene/SceneResources.h>  // provides complete SceneTypeFactory; transitively includes SceneObjects.h
#include <core/json.h>

using namespace caustica;

// =============================================================================
// GaussianSplat
// =============================================================================

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
// SceneSettings
// =============================================================================

void SceneSettings::load(const Json::Value& node)
{
    node["realtimeMode"]          >> realtimeMode;
    node["enableAnimations"]      >> enableAnimations;
    node["enableKeyframes"]       >> enableKeyframes;
    node["startingCamera"]        >> startingCamera;
    node["realtimeFireflyFilter"] >> realtimeFireflyFilter;
    node["maxBounces"]            >> maxBounces;
    node["maxDiffuseBounces"]     >> maxDiffuseBounces;
    node["textureMIPBias"]        >> textureMIPBias;
}

// =============================================================================
// GameSettings
// =============================================================================

void GameSettings::load(const Json::Value& node)
{
    Json::StreamWriterBuilder writer;
    jsonData = Json::writeString(writer, node);
}

// =============================================================================
// SceneTypeFactory
// =============================================================================

std::shared_ptr<void> SceneTypeFactory::createLeaf(const std::string& type)
{
    if (type == "GaussianSplat" || type == "GaussianSplats" || type == "3DGaussianSplat")
        return std::make_shared<GaussianSplat>();
    // Accept the legacy name so scenes authored before the engine rename still load.
    if (type == "SceneSettings" || type == "SampleSettings")
        return std::make_shared<SceneSettings>();
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
