#pragma once

#include <scene/Scene.h>
#include <string.h>

constexpr int LightType_Environment = 1000;

struct LightSamplerLink
{
    int         IndexOrBase = -1;       // index of a corresponding PolymorphicLight in the light sampler
    int         LastUpdateTag = -1;     // identifier of when IndexOrBase was last updated (either frame index or similar)
};

struct LightExtension// : virtual public caustica::Light
{
    LightSamplerLink            LightLink;
    std::vector<std::string>    Proxies;

    void Load(const Json::Value& node);
    void Store(Json::Value& node) const;
    void Copy(LightExtension & src)         { LightLink = src.LightLink; Proxies = src.Proxies; }
};

class SpotLightEx : public caustica::SpotLight, public LightExtension
{
public:
    [[nodiscard]] std::shared_ptr<SceneGraphLeaf> Clone() override;
    void Load(const Json::Value& node) override;
    void Store(Json::Value& node) const override;
};

class PointLightEx : public caustica::PointLight, public LightExtension
{
public:
    [[nodiscard]] std::shared_ptr<SceneGraphLeaf> Clone() override;
    void Load(const Json::Value& node) override;
    void Store(Json::Value& node) const override;
};


struct MeshInstanceExtension
{
    std::vector<LightSamplerLink>       PerGeometryLightSamplerLinks;
    std::weak_ptr<LightExtension>       ProxiedAnalyticLight;

    void Copy(MeshInstanceExtension& src) { PerGeometryLightSamplerLinks = src.PerGeometryLightSamplerLinks; }
};

class MeshInstanceEx : public caustica::MeshInstance, public MeshInstanceExtension
{
public:
    explicit MeshInstanceEx(const std::shared_ptr<caustica::MeshInfo> & mesh);
    [[nodiscard]] std::shared_ptr<SceneGraphLeaf> Clone() override;
};

class SkinnedMeshInstanceEx : public caustica::SkinnedMeshInstance, public MeshInstanceExtension
{
public:
    explicit SkinnedMeshInstanceEx(std::shared_ptr<caustica::SceneTypeFactory> sceneTypeFactory, std::shared_ptr<caustica::MeshInfo> prototypeMesh);
    [[nodiscard]] std::shared_ptr<SceneGraphLeaf> Clone() override;
};

class EnvironmentLight : public caustica::Light
{
public:
    dm::float3 radianceScale = 1.f;
    int textureIndex = -1;
    float rotation = 0.f;
    std::string path;

    void Load(const Json::Value& node) override;
    [[nodiscard]] int GetLightType() const override { return LightType_Environment; }
    [[nodiscard]] std::shared_ptr<SceneGraphLeaf> Clone() override;
    void FillLightConstants(LightConstants& lightConstants) const override;
    bool SetProperty(const std::string& name, const dm::float4& value) override { assert( false ); return false; }    // not yet implemented, never needed
};

class GaussianSplat : public caustica::SceneGraphLeaf
{
public:
    std::string path;
    std::string resolvedPath;
    bool convertRdfToRub = true;
    bool enabled = true;
    uint32_t loadedSplatCount = 0;

    [[nodiscard]] std::shared_ptr<SceneGraphLeaf> Clone() override;
    void Load(const Json::Value& node) override;
};

class PerspectiveCameraEx : public caustica::PerspectiveCamera
{
public:
    std::optional<bool>     enableAutoExposure;
    std::optional<float>    exposureCompensation;
    std::optional<float>    exposureValue;
    std::optional<float>    exposureValueMin;
    std::optional<float>    exposureValueMax;

    [[nodiscard]] std::shared_ptr<SceneGraphLeaf> Clone() override;
    void Load(const Json::Value& node) override;
    bool SetProperty(const std::string& name, const dm::float4& value) override;
};

// used to setup initial sample scene settings
class SampleSettings : public caustica::SceneGraphLeaf
{
public:
    std::optional<bool>         realtimeMode;
    std::optional<bool>         enableAnimations;
    std::optional<int>          startingCamera;
    std::optional<float>        realtimeFireflyFilter;
    std::optional<int>          maxBounces;
    std::optional<int>          maxDiffuseBounces;
    std::optional<float>        textureMIPBias;

    [[nodiscard]] std::shared_ptr<SceneGraphLeaf> Clone() override;
    virtual void Load(const Json::Value& node) override;
};

// used to setup initial sample game settings (if any)
class GameSettings : public caustica::SceneGraphLeaf
{
    std::string                 jsonData;

    [[nodiscard]] virtual std::shared_ptr<SceneGraphLeaf> Clone() override;
    virtual void                Load(const Json::Value& node) override;

public:
    const std::string           GetJsonData() const { return jsonData; }
};

class ExtendedSceneTypeFactory : public caustica::SceneTypeFactory
{
public:
    std::shared_ptr<caustica::SceneGraphLeaf>  CreateLeaf(const std::string& type) override;
    std::shared_ptr<caustica::Material>        CreateMaterial() override;
    std::shared_ptr<caustica::MeshInfo>        CreateMesh() override;
    std::shared_ptr<caustica::MeshGeometry>    CreateMeshGeometry() override;
    std::shared_ptr<caustica::MeshInstance>    CreateMeshInstance(const std::shared_ptr<caustica::MeshInfo>& mesh);
    std::shared_ptr<caustica::SkinnedMeshInstance> CreateSkinnedMeshInstance(const std::shared_ptr<caustica::SceneTypeFactory> & sceneTypeFactory, const std::shared_ptr<caustica::MeshInfo> & prototypeMesh) override;
};

class ExtendedScene : public caustica::Scene
{
private:
    std::shared_ptr<SampleSettings> m_loadedSettings = nullptr;
    std::shared_ptr<GameSettings>   m_loadedGameSettings = nullptr;

public:
    using Scene::Scene;

    bool LoadWithThreadPool(const std::filesystem::path& jsonFileName, caustica::ThreadPool* threadPool) override;
    bool LoadFromJsonString(const std::string& sceneJson, const std::filesystem::path& scenePath = {}) override;
    std::shared_ptr<SampleSettings> GetSampleSettingsNode() const   { return m_loadedSettings; }
    std::shared_ptr<GameSettings>   GetGameSettingsNode() const     { return m_loadedGameSettings; }

    const std::vector<caustica::SceneImportResult> & GetModels() const               { return m_Models; }

protected:
    bool LoadModelFile(
        const std::filesystem::path& fileName,
        caustica::ThreadPool* threadPool,
        caustica::SceneImportResult& result) override;

private:
    // maybe switch to SceneGraphWalker?
    void ProcessNodesRecursive(std::shared_ptr<caustica::SceneGraphNode> node);
};

std::shared_ptr<EnvironmentLight> FindEnvironmentLight(std::vector <std::shared_ptr<caustica::Light>> lights);
