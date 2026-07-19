#if CAUSTICA_WITH_PYTHON

#include "PythonBindingsCore.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/optional.h>
#include <nanobind/operators.h>

#include "SceneEditor.h"
#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/SceneQuery.h>
#include <engine/CameraApi.h>
#include <engine/SceneLifecycle.h>
#include <engine/RenderSessionApi.h>
#include <render/RenderAppState.h>
#include <EditorUI.h>
#include <scene/Scene.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/lighting/LightSamplingCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>

#include <scene/Scene.h>
#include <scene/SceneTypes.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
#include <ecs/Entity.h>
#include <core/log.h>
#include <math/math.h>
#include <shaders/light_types.h>

#include <stdexcept>
#include <cmath>
#include <string>
#include <vector>
#include <optional>
#include <array>
#include <cstring>
#include <unordered_set>
#include <algorithm>
#include <cctype>

namespace nb = nanobind;
using namespace caustica;
using caustica::App;
using caustica::math::float2;
using caustica::math::float3;
using caustica::math::float4;
using caustica::math::double3;
using caustica::math::double4;
using caustica::editor::SceneEditor;
using caustica::editor::EditorUIData;
using caustica::render::RenderAppState;

// Singleton consumed by embed mode (set by PythonScripting before Py_Initialize).
// In extension mode this stays nullptr - Renderer manages its own Sample.
SceneEditor* g_pythonSceneEditorSingleton = nullptr;

// Distinct C++ enum types so nanobind can register them as separate Python
// enums (nb::enum_<T> requires T to be unique across the module).  All map
// 1:1 to ints already used by the underlying Sample / OIDN / Streamline UI.
namespace py_enums
{
    enum class PathTracerMode : int { Realtime = 0, Reference = 1 };
    enum class RealtimeAA     : int { Off = 0, TAA = 1, DLSS = 2, DLSS_RR = 3 };
    enum class DLSSMode       : int { Off = 0, MaxPerformance = 1, Balanced = 2, MaxQuality = 3, UltraPerformance = 4, UltraQuality = 5, DLAA = 6 };
    enum class DLSSFGMode     : int { Off = 0, On = 1, Auto = 2 };
    enum class DLSSRRPreset   : int { Default = 0, PresetA = 1, PresetB = 2, PresetC = 3, PresetD = 4, PresetE = 5, PresetF = 6, PresetG = 7, PresetH = 8 };
    enum class ReflexMode     : int { Off = 0, LowLatency = 1, LowLatencyWithBoost = 2 };
    enum class OidnPasses     : int { ColorOnly = 0, Albedo = 1, AlbedoNormal = 2 };
    enum class OidnPrefilter  : int { None_ = 0, Fast = 1, Accurate = 2 };
    enum class OidnQuality    : int { Fast = 0, Balanced = 1, High = 2 };
    enum class GaussianSplatSortMode : int { GpuSort = 0, StochasticSplats = 1 };
    enum class GaussianSplatStorageFormat : int { Float32 = 0, Float16 = 1, Uint8 = 2 };
    enum class GaussianSplatFrustumCulling : int { Disabled = 0, AtDistanceStage = 1, AtRasterStage = 2 };
    enum class GaussianSplatShadowMode : int { Disabled = 0, Hard = 1, Soft = 2 };
    enum class GaussianSplatFTBSyncMode : int { Disabled = 0, Interlock = 1 };
}

namespace
{
    float3 ToFloat3(const nb::object& src)
    {
        if (nb::isinstance<float3>(src))
            return nb::cast<float3>(src);
        nb::sequence seq = nb::cast<nb::sequence>(src);
        std::vector<float> v;
        for (auto h : seq) v.push_back(nb::cast<float>(nb::handle(h)));
        if (v.size() != 3)
            throw std::runtime_error("Expected an iterable of 3 floats");
        return float3(v[0], v[1], v[2]);
    }

    nb::tuple Float3ToTuple(const float3& v) { return nb::make_tuple(v.x, v.y, v.z); }
    nb::tuple Double3ToTuple(const double3& v) { return nb::make_tuple(v.x, v.y, v.z); }

    std::string LowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return char(std::tolower(ch));
        });
        return value;
    }

    bool IsOpenPBRMaterialModelName(const std::string& materialModel)
    {
        std::string normalized = LowerCopy(materialModel);
        // Accept former spelling variants when loading older scripts/files.
        return normalized == "openpbr" || normalized == "openpbr-lite" || normalized == "openpbr_lite";
    }

    void SetMaterialModelFromPython(PTMaterial& self, const std::string& value)
    {
        self.materialModel = IsOpenPBRMaterialModelName(value) ? "OpenPBR" : value;
        if (IsOpenPBRMaterialModelName(value))
        {
            self.useSpecularGlossModel = false;
            if (self.specularColor.x == 0.f && self.specularColor.y == 0.f && self.specularColor.z == 0.f)
                self.specularColor = float3(1.f);
        }
        self.gpuDataDirty = true;
    }

    void SetOpenPBRTransmissionWeightFromPython(PTMaterial& self, float value)
    {
        self.transmissionFactor = value;
        self.enableTransmission = self.transmissionFactor > 0.f || self.diffuseTransmissionFactor > 0.f;
        self.gpuDataDirty = true;
    }

    void SetOpenPBRDiffuseTransmissionWeightFromPython(PTMaterial& self, float value)
    {
        self.diffuseTransmissionFactor = value;
        self.enableTransmission = self.transmissionFactor > 0.f || self.diffuseTransmissionFactor > 0.f;
        self.gpuDataDirty = true;
    }

    double3 ToDouble3(const nb::object& src)
    {
        nb::sequence seq = nb::cast<nb::sequence>(src);
        std::vector<double> v;
        for (auto h : seq) v.push_back(nb::cast<double>(nb::handle(h)));
        if (v.size() != 3)
            throw std::runtime_error("Expected an iterable of 3 floats");
        return double3(v[0], v[1], v[2]);
    }

    double4 ToDouble4(const nb::object& src)
    {
        nb::sequence seq = nb::cast<nb::sequence>(src);
        std::vector<double> v;
        for (auto h : seq) v.push_back(nb::cast<double>(nb::handle(h)));
        if (v.size() != 4)
            throw std::runtime_error("Expected an iterable of 4 floats");
        return double4(v[0], v[1], v[2], v[3]);
    }

    nb::tuple DQuatToXYZWTuple(const caustica::math::dquat& q)
    {
        return nb::make_tuple(q.x, q.y, q.z, q.w);
    }

    caustica::math::dquat ToDQuatXYZW(const nb::object& src)
    {
        return caustica::math::dquat::fromXYZW(ToDouble4(src));
    }

    double3 DQuatToEulerRadiansXYZ(const caustica::math::dquat& rotation)
    {
        const caustica::math::double3x3 m = rotation.toMatrix();

        const double y = std::asin(caustica::math::clamp(-m.m_data[2], -1.0, 1.0));
        const double cy = std::cos(y);

        double x = 0.0;
        double z = 0.0;
        if (std::abs(cy) > 1e-8)
        {
            x = std::atan2(m.m_data[5], m.m_data[8]);
            z = std::atan2(m.m_data[1], m.m_data[0]);
        }
        else
        {
            x = std::atan2(-m.m_data[7], m.m_data[4]);
        }

        return double3(x, y, z);
    }

    struct PySceneEntity
    {
        Scene* scene = nullptr;
        ecs::Entity entity = ecs::NullEntity;

        [[nodiscard]] scene::SceneEntityWorld* entityWorld() const
        {
            return scene ? scene->getEntityWorld() : nullptr;
        }
    };

    [[nodiscard]] int EntityLightType(const PySceneEntity& self)
    {
        scene::SceneEntityWorld* entityWorld = self.entityWorld();
        if (!entityWorld)
            return LightType_None;
        auto& world = entityWorld->world();
        if (world.has<scene::DirectionalLightComponent>(self.entity))
            return LightType_Directional;
        if (world.has<scene::SpotLightComponent>(self.entity))
            return LightType_Spot;
        if (world.has<scene::PointLightComponent>(self.entity))
            return LightType_Point;
        if (world.has<scene::EnvironmentLightComponent>(self.entity))
            return LightType_Environment;
        return LightType_None;
    }

    [[nodiscard]] dm::float3* TryMutableLightColor(PySceneEntity& self)
    {
        scene::SceneEntityWorld* entityWorld = self.entityWorld();
        if (!entityWorld)
            return nullptr;
        auto& world = entityWorld->world();
        if (auto* directional = scene::tryGetDirectionalLight(world, self.entity))
            return &directional->color;
        if (auto* spot = scene::tryGetSpotLight(world, self.entity))
            return &spot->color;
        if (auto* point = scene::tryGetPointLight(world, self.entity))
            return &point->color;
        if (auto* environment = scene::tryGetEnvironmentLight(world, self.entity))
            return &environment->color;
        return nullptr;
    }

    [[nodiscard]] float* TryMutableLightIntensity(PySceneEntity& self)
    {
        scene::SceneEntityWorld* entityWorld = self.entityWorld();
        if (!entityWorld)
            return nullptr;
        auto& world = entityWorld->world();
        if (auto* spot = scene::tryGetSpotLight(world, self.entity))
            return &spot->intensity;
        if (auto* point = scene::tryGetPointLight(world, self.entity))
            return &point->intensity;
        return nullptr;
    }


    std::vector<std::shared_ptr<PTMaterial>> GetSceneMaterials(const Scene* scene)
    {
        std::vector<std::shared_ptr<PTMaterial>> result;
        if (!scene)
            return result;

        for (const auto& mat : scene->getMaterials())
        {
            if (auto pt = PTMaterial::safeCast(mat))
                result.push_back(pt);
        }
        return result;
    }

    std::shared_ptr<PTMaterial> FindSceneMaterial(const Scene* scene, const std::string& name)
    {
        if (!scene)
            return nullptr;

        for (const auto& mat : scene->getMaterials())
        {
            auto pt = PTMaterial::safeCast(mat);
            if (pt && (pt->name == name || pt->uniqueName == name))
                return pt;
        }
        return nullptr;
    }

    std::shared_ptr<PTMaterial> FindSceneMaterialById(const Scene* scene, int materialId)
    {
        if (!scene || materialId < 0)
            return nullptr;

        // Prefer gpuDataIndex (path-tracer / Material Editor id). materialID is a dense
        // scene-list index from unordered_map iteration and can diverge after imports.
        for (const auto& mat : scene->getMaterials())
        {
            const auto pt = PTMaterial::safeCast(mat);
            if (pt && int(pt->gpuDataIndex) == materialId)
                return pt;
        }
        for (const auto& mat : scene->getMaterials())
        {
            if (mat && mat->materialID == materialId)
                return PTMaterial::safeCast(mat);
        }
        return nullptr;
    }

    std::vector<std::shared_ptr<PySceneEntity>> GetSceneLights(Scene* scene)
    {
        std::vector<std::shared_ptr<PySceneEntity>> result;
        if (!scene)
            return result;

        for (ecs::Entity entity : scene->getLightEntities())
            result.push_back(std::make_shared<PySceneEntity>(PySceneEntity{ scene, entity }));
        return result;
    }

    std::shared_ptr<PySceneEntity> FindSceneLight(Scene* scene, const std::string& name)
    {
        if (!scene)
            return nullptr;

        const scene::SceneEntityWorld* entityWorld = scene->getEntityWorld();
        if (!entityWorld)
            return nullptr;

        for (ecs::Entity entity : scene->getLightEntities())
        {
            if (entityWorld->getEntityName(entity) == name)
                return std::make_shared<PySceneEntity>(PySceneEntity{ scene, entity });
        }
        return nullptr;
    }

    void WalkEntitiesByName(const scene::SceneEntityWorld& entityWorld, ecs::Entity root, const std::string& name, ecs::Entity& outEntity)
    {
        if (!ecs::isValid(root) || ecs::isValid(outEntity))
            return;

        if (entityWorld.getEntityName(root) == name)
        {
            outEntity = root;
            return;
        }

        for (ecs::Entity child : entityWorld.getEntityChildren(root))
            WalkEntitiesByName(entityWorld, child, name, outEntity);
    }

    std::shared_ptr<PySceneEntity> FindSceneEntity(Scene* scene, const std::string& path)
    {
        if (!scene || path.empty())
            return nullptr;

        scene::SceneEntityWorld* entityWorld = scene->getEntityWorld();
        if (!entityWorld)
            return nullptr;

        ecs::Entity entity = ecs::NullEntity;
        const std::filesystem::path query(path);
        if (query.is_absolute())
            entity = entityWorld->findEntity(query);
        else if (ecs::Entity found = entityWorld->findEntity(std::filesystem::path("/") / query); ecs::isValid(found))
            entity = found;
        else if (!query.has_parent_path())
            WalkEntitiesByName(*entityWorld, entityWorld->root(), path, entity);

        if (!ecs::isValid(entity))
            return nullptr;

        return std::make_shared<PySceneEntity>(PySceneEntity{ scene, entity });
    }

    std::vector<std::shared_ptr<MeshInfo>> GetSceneMeshes(const Scene* scene)
    {
        std::vector<std::shared_ptr<MeshInfo>> result;
        if (!scene)
            return result;

        for (const auto& mesh : scene->getMeshes())
            result.push_back(mesh);
        return result;
    }

    std::shared_ptr<MeshInfo> FindSceneMesh(const Scene* scene, const std::string& name)
    {
        if (!scene)
            return nullptr;

        for (const auto& mesh : scene->getMeshes())
        {
            if (mesh && mesh->name == name)
                return mesh;
        }
        return nullptr;
    }

    std::shared_ptr<MeshInfo> MeshFromEntity(const PySceneEntity& pyEntity)
    {
        scene::SceneEntityWorld* entityWorld = pyEntity.entityWorld();
        if (!entityWorld || !ecs::isValid(pyEntity.entity))
            return nullptr;

        const auto* meshComponent = entityWorld->world().get<scene::MeshInstanceComponent>(pyEntity.entity);
        return meshComponent && meshComponent->mesh ? meshComponent->mesh : nullptr;
    }

    ecs::Entity EntityHandleFromPyNode(const std::shared_ptr<PySceneEntity>& node)
    {
        if (!node || !ecs::isValid(node->entity))
            throw std::runtime_error("SceneNode is null or invalid");
        return node->entity;
    }

    std::array<uint32_t, 3> MeshPositionKey(const float3& p)
    {
        std::array<uint32_t, 3> key{};
        std::memcpy(&key[0], &p.x, sizeof(uint32_t));
        std::memcpy(&key[1], &p.y, sizeof(uint32_t));
        std::memcpy(&key[2], &p.z, sizeof(uint32_t));
        return key;
    }

    struct MeshPositionKeyHash
    {
        size_t operator()(const std::array<uint32_t, 3>& key) const noexcept
        {
            size_t h = std::hash<uint32_t>{}(key[0]);
            h ^= std::hash<uint32_t>{}(key[1]) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(key[2]) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    size_t UniqueMeshPositionCount(const MeshInfo& mesh)
    {
        if (!mesh.buffers || mesh.totalVertices == 0)
            return 0;

        const auto& positions = mesh.buffers->positionData;
        const size_t begin = size_t(mesh.vertexOffset);
        const size_t end = begin + size_t(mesh.totalVertices);
        if (positions.size() < end)
            return size_t(mesh.totalVertices);

        const auto* meshEx = dynamic_cast<const MeshInfoEx*>(&mesh);
        if (meshEx && meshEx->DeformationSourcePositionIndices.size() == size_t(mesh.totalVertices))
        {
            std::unordered_set<uint32_t> uniqueSourcePositions;
            uniqueSourcePositions.reserve(mesh.totalVertices);
            for (uint32_t sourceIndex : meshEx->DeformationSourcePositionIndices)
                uniqueSourcePositions.insert(sourceIndex);
            return uniqueSourcePositions.size();
        }

        std::unordered_set<std::array<uint32_t, 3>, MeshPositionKeyHash> uniquePositions;
        uniquePositions.reserve(mesh.totalVertices);
        for (size_t i = begin; i < end; ++i)
            uniquePositions.insert(MeshPositionKey(positions[i]));

        return uniquePositions.size();
    }

    std::vector<float3> ToFloat3Vector(const nb::object& src)
    {
        nb::sequence seq = nb::cast<nb::sequence>(src);
        std::vector<float3> result;
        for (auto h : seq)
            result.push_back(ToFloat3(nb::borrow<nb::object>(h)));
        return result;
    }

    nb::list Float3VectorToList(const std::vector<float3>& vertices)
    {
        nb::list result;
        for (const float3& v : vertices)
            result.append(Float3ToTuple(v));
        return result;
    }

    bool IsFiniteBox(const caustica::math::box3& bounds)
    {
        return caustica::math::all(caustica::math::isfinite(bounds.m_mins))
            && caustica::math::all(caustica::math::isfinite(bounds.m_maxs));
    }

    // Returns the C++ Scene bounds when they are populated and finite.
    std::optional<caustica::math::box3> ValidSceneBounds(const caustica::math::box3& bounds)
    {
        if (bounds.isempty() || !IsFiniteBox(bounds))
            return std::nullopt;
        return bounds;
    }

    std::optional<caustica::math::box3> SceneBoundsFromScene(const Scene* scene)
    {
        if (!scene)
            return std::nullopt;
        return ValidSceneBounds(scene->getSceneBounds());
    }

    std::optional<caustica::math::box3> SceneBoundsFromScene(const std::shared_ptr<Scene>& scene)
    {
        return SceneBoundsFromScene(scene.get());
    }

    // Converts the Scene AABB to a ((min.xyz), (max.xyz)) Python tuple,
    // or `None` if the scene is empty / not loaded yet.
    nb::object SceneBoundsTuple(const std::optional<caustica::math::box3>& bbox)
    {
        if (!bbox)
            return nb::none();
        return nb::make_tuple(Float3ToTuple(bbox->m_mins), Float3ToTuple(bbox->m_maxs));
    }

    nb::object SceneBoundsCenter(const std::optional<caustica::math::box3>& bbox)
    {
        if (!bbox)
            return nb::none();
        return Float3ToTuple(bbox->center());
    }

    nb::object SceneBoundsSize(const std::optional<caustica::math::box3>& bbox)
    {
        if (!bbox)
            return nb::none();
        return Float3ToTuple(bbox->diagonal());
    }

    nb::object MaterialTexturePath(const PTMaterial& material, PTMaterialTextureSlot slot)
    {
        const PTTexture& texture = material.getTexture(slot);
        if (texture.loaded == nullptr || texture.localPath.empty())
            return nb::none();
        return nb::str(texture.localPath.generic_string().c_str());
    }

    bool SetMaterialTextureFromPython(
        PTMaterial& material,
        PTMaterialTextureSlot slot,
        const std::string& path,
        std::optional<bool> sRGB = std::nullopt,
        std::optional<bool> normalMap = std::nullopt)
    {
        if (material.runtimeMaterialGpuCache == nullptr)
            throw std::runtime_error("Material is not attached to a live MaterialGpuCache. Reload the scene and look up the material again.");

        return material.runtimeMaterialGpuCache->setMaterialTexture(
            material,
            slot,
            std::filesystem::path(path),
            sRGB,
            normalMap);
    }

    void ClearMaterialTextureFromPython(PTMaterial& material, PTMaterialTextureSlot slot)
    {
        if (material.runtimeMaterialGpuCache == nullptr)
            throw std::runtime_error("Material is not attached to a live MaterialGpuCache. Reload the scene and look up the material again.");

        material.runtimeMaterialGpuCache->clearMaterialTexture(material, slot);
    }
}

namespace caustica_py
{

void RegisterCoreBindings(nb::module_& m)
{
    // --- helpers ----------------------------------------------------------
    m.def("log_info",    [](const std::string& s) { caustica::info("[py] %s", s.c_str()); },
          nb::arg("message"), "Forward a message to the host log at INFO level.");
    m.def("log_warning", [](const std::string& s) { caustica::warning("[py] %s", s.c_str()); },
          nb::arg("message"), "Forward a message to the host log at WARNING level.");
    m.def("log_error",   [](const std::string& s) { caustica::error("[py] %s", s.c_str()); },
          nb::arg("message"), "Forward a message to the host log at ERROR level.");

    using namespace py_enums;

    // All enums use `is_arithmetic()` so users can write `int(value)` /
    // `value | other` and Python -> C++ implicit conversion to the underlying
    // int field works seamlessly.

    // --- Path tracer mode (Reference vs Realtime) --------------------------
    nb::enum_<PathTracerMode>(m, "PathTracerMode",
        "Selects between accumulating reference rendering and a realtime path tracer.",
        nb::is_arithmetic())
        .value("Realtime",  PathTracerMode::Realtime,  "Realtime mode - 1 SPP per frame, denoiser, DLSS, RTXDI.")
        .value("Reference", PathTracerMode::Reference, "Reference / accumulation mode - converges to ground truth.")
        .export_values();

    // --- AA / super-resolution / denoising preset --------------------------
    nb::enum_<RealtimeAA>(m, "RealtimeAA",
        "Realtime-mode AA / SR / denoising preset (mirrors the UI 'AA/SR/Denoising' combo).",
        nb::is_arithmetic())
        .value("Off",     RealtimeAA::Off,     "No AA / no upscaling.")
        .value("TAA",     RealtimeAA::TAA,     "Temporal anti-aliasing (no DLSS).")
        .value("DLSS",    RealtimeAA::DLSS,    "DLSS Super Resolution.")
        .value("DLSS_RR", RealtimeAA::DLSS_RR, "DLSS Ray Reconstruction (DLSS + denoising).")
        .export_values();

    // --- DLSS quality enums (mirrors SI::DLSSMode) -------------------------
    nb::enum_<DLSSMode>(m, "DLSSMode",
        "Quality preset for DLSS (used by both DLSS and DLSS-RR).",
        nb::is_arithmetic())
        .value("Off",              DLSSMode::Off)
        .value("MaxPerformance",   DLSSMode::MaxPerformance)
        .value("Balanced",         DLSSMode::Balanced)
        .value("MaxQuality",       DLSSMode::MaxQuality)
        .value("UltraPerformance", DLSSMode::UltraPerformance)
        .value("UltraQuality",     DLSSMode::UltraQuality)
        .value("DLAA",             DLSSMode::DLAA)
        .export_values();

    nb::enum_<DLSSFGMode>(m, "DLSSFGMode", "Frame generation (DLSS-G) mode.",
        nb::is_arithmetic())
        .value("Off",  DLSSFGMode::Off)
        .value("On",   DLSSFGMode::On)
        .value("Auto", DLSSFGMode::Auto)
        .export_values();

    nb::enum_<DLSSRRPreset>(m, "DLSSRRPreset",
        "DLSS-RR neural network preset (DLSSRRPreset).",
        nb::is_arithmetic())
        .value("Default", DLSSRRPreset::Default)
        .value("PresetA", DLSSRRPreset::PresetA)
        .value("PresetB", DLSSRRPreset::PresetB)
        .value("PresetC", DLSSRRPreset::PresetC)
        .value("PresetD", DLSSRRPreset::PresetD)
        .value("PresetE", DLSSRRPreset::PresetE)
        .value("PresetF", DLSSRRPreset::PresetF)
        .value("PresetG", DLSSRRPreset::PresetG)
        .value("PresetH", DLSSRRPreset::PresetH)
        .export_values();

    nb::enum_<ReflexMode>(m, "ReflexMode", "NVIDIA Reflex low-latency mode.",
        nb::is_arithmetic())
        .value("Off",                 ReflexMode::Off)
        .value("LowLatency",          ReflexMode::LowLatency)
        .value("LowLatencyWithBoost", ReflexMode::LowLatencyWithBoost)
        .export_values();

    // --- OIDN denoiser enums (mirror OidnDenoiser::Passes/Prefilter/Quality)
    nb::enum_<OidnPasses>(m, "OidnPasses",
        "Auxiliary guide passes used by OIDN (Color Only / Albedo / Albedo+Normal).",
        nb::is_arithmetic())
        .value("ColorOnly",    OidnPasses::ColorOnly)
        .value("Albedo",       OidnPasses::Albedo)
        .value("AlbedoNormal", OidnPasses::AlbedoNormal)
        .export_values();

    nb::enum_<OidnPrefilter>(m, "OidnPrefilter", "OIDN auxiliary prefilter quality.",
        nb::is_arithmetic())
        .value("None_",    OidnPrefilter::None_)
        .value("Fast",     OidnPrefilter::Fast)
        .value("Accurate", OidnPrefilter::Accurate)
        .export_values();

    nb::enum_<OidnQuality>(m, "OidnQuality", "OIDN beauty filter quality / performance trade-off.",
        nb::is_arithmetic())
        .value("Fast",     OidnQuality::Fast)
        .value("Balanced", OidnQuality::Balanced)
        .value("High",     OidnQuality::High)
        .export_values();

    nb::enum_<py_enums::GaussianSplatSortMode>(m, "GaussianSplatSortMode",
        "3D Gaussian Splat rasterization ordering mode.",
        nb::is_arithmetic())
        .value("GpuSort",           py_enums::GaussianSplatSortMode::GpuSort)
        .value("StochasticSplats",  py_enums::GaussianSplatSortMode::StochasticSplats)
        .export_values();

    nb::enum_<py_enums::GaussianSplatStorageFormat>(m, "GaussianSplatStorageFormat",
        "GPU storage format for 3DGS color/SH payloads.",
        nb::is_arithmetic())
        .value("Float32", py_enums::GaussianSplatStorageFormat::Float32)
        .value("Float16", py_enums::GaussianSplatStorageFormat::Float16)
        .value("Uint8",   py_enums::GaussianSplatStorageFormat::Uint8)
        .export_values();

    nb::enum_<py_enums::GaussianSplatFrustumCulling>(m, "GaussianSplatFrustumCulling",
        "3DGS frustum culling mode.",
        nb::is_arithmetic())
        .value("Disabled",        py_enums::GaussianSplatFrustumCulling::Disabled)
        .value("AtDistanceStage",  py_enums::GaussianSplatFrustumCulling::AtDistanceStage)
        .value("AtRasterStage",    py_enums::GaussianSplatFrustumCulling::AtRasterStage)
        .export_values();

    nb::enum_<GaussianSplatShadowMode>(m, "GaussianSplatShadowMode",
        "Hybrid 3DGS shadow mode.",
        nb::is_arithmetic())
        .value("Disabled", GaussianSplatShadowMode::Disabled)
        .value("Hard",     GaussianSplatShadowMode::Hard)
        .value("Soft",     GaussianSplatShadowMode::Soft)
        .export_values();

    nb::enum_<GaussianSplatFTBSyncMode>(m, "GaussianSplatFTBSyncMode",
        "3DGS front-to-back synchronization mode.",
        nb::is_arithmetic())
        .value("Disabled",  GaussianSplatFTBSyncMode::Disabled)
        .value("Interlock", GaussianSplatFTBSyncMode::Interlock)
        .export_values();

    nb::enum_<PTMaterialTextureSlot>(m, "TextureSlot",
        "Material texture slot for runtime texture replacement.",
        nb::is_arithmetic())
        .value("Base", PTMaterialTextureSlot::Base)
        .value("ORM", PTMaterialTextureSlot::OcclusionRoughnessMetallic)
        .value("OcclusionRoughnessMetallic", PTMaterialTextureSlot::OcclusionRoughnessMetallic)
        .value("Normal", PTMaterialTextureSlot::Normal)
        .value("Emissive", PTMaterialTextureSlot::Emissive)
        .value("Transmission", PTMaterialTextureSlot::Transmission)
        .export_values();

    // --- PTMaterial -------------------------------------------------------
    nb::class_<PTMaterial>(m, "Material",
        "caustica material wrapper (PTMaterial). All edits flag the material as\n"
        "dirty so the GPU buffer is re-uploaded the following frame.")
        .def_ro("name",         &PTMaterial::name)
        .def_ro("model_name",   &PTMaterial::modelName)
        .def_ro("unique_name",  &PTMaterial::uniqueName)

        .def_prop_rw("base_color",
            [](PTMaterial& self) { return Float3ToTuple(self.baseOrDiffuseColor); },
            [](PTMaterial& self, nb::object v) { self.baseOrDiffuseColor = ToFloat3(v); self.gpuDataDirty = true; },
            "Metal-rough base color or spec-gloss diffuse color (linear RGB).")
        .def_prop_rw("specular_color",
            [](PTMaterial& self) { return Float3ToTuple(self.specularColor); },
            [](PTMaterial& self, nb::object v) { self.specularColor = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("emissive_color",
            [](PTMaterial& self) { return Float3ToTuple(self.emissiveColor); },
            [](PTMaterial& self, nb::object v) { self.emissiveColor = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("emission_color",
            [](PTMaterial& self) { return Float3ToTuple(self.emissiveColor); },
            [](PTMaterial& self, nb::object v) { self.emissiveColor = ToFloat3(v); self.gpuDataDirty = true; },
            "OpenPBR alias for emissive_color.")

        .def_prop_rw("emissive_intensity",
            [](PTMaterial& self) { return self.emissiveIntensity; },
            [](PTMaterial& self, float v) { self.emissiveIntensity = v; self.gpuDataDirty = true; })
        .def_prop_rw("emission_luminance",
            [](PTMaterial& self) { return self.emissiveIntensity; },
            [](PTMaterial& self, float v) { self.emissiveIntensity = v; self.gpuDataDirty = true; },
            "OpenPBR alias for emissive_intensity.")
        .def_prop_rw("metalness",
            [](PTMaterial& self) { return self.metalness; },
            [](PTMaterial& self, float v) { self.metalness = v; self.gpuDataDirty = true; })
        .def_prop_rw("base_metalness",
            [](PTMaterial& self) { return self.metalness; },
            [](PTMaterial& self, float v) { self.metalness = v; self.gpuDataDirty = true; },
            "OpenPBR alias for metalness.")
        .def_prop_rw("roughness",
            [](PTMaterial& self) { return self.roughness; },
            [](PTMaterial& self, float v) { self.roughness = v; self.gpuDataDirty = true; })
        .def_prop_rw("specular_roughness",
            [](PTMaterial& self) { return self.roughness; },
            [](PTMaterial& self, float v) { self.roughness = v; self.gpuDataDirty = true; },
            "OpenPBR alias for roughness.")
        .def_prop_rw("material_model",
            [](PTMaterial& self) { return self.materialModel; },
            [](PTMaterial& self, const std::string& v) { SetMaterialModelFromPython(self, v); })
        .def_prop_rw("base_weight",
            [](PTMaterial& self) { return self.baseWeight; },
            [](PTMaterial& self, float v) { self.baseWeight = v; self.gpuDataDirty = true; })
        .def_prop_rw("specular_weight",
            [](PTMaterial& self) { return self.specularWeight; },
            [](PTMaterial& self, float v) { self.specularWeight = v; self.gpuDataDirty = true; })
        .def_prop_rw("anisotropy",
            [](PTMaterial& self) { return self.anisotropy; },
            [](PTMaterial& self, float v) { self.anisotropy = v; self.gpuDataDirty = true; })
        .def_prop_rw("specular_roughness_anisotropy",
            [](PTMaterial& self) { return self.anisotropy; },
            [](PTMaterial& self, float v) { self.anisotropy = v; self.gpuDataDirty = true; },
            "OpenPBR alias for anisotropy.")
        .def_prop_rw("fuzz_weight",
            [](PTMaterial& self) { return self.fuzzWeight; },
            [](PTMaterial& self, float v) { self.fuzzWeight = v; self.gpuDataDirty = true; })
        .def_prop_rw("fuzz_color",
            [](PTMaterial& self) { return Float3ToTuple(self.fuzzColor); },
            [](PTMaterial& self, nb::object v) { self.fuzzColor = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("fuzz_roughness",
            [](PTMaterial& self) { return self.fuzzRoughness; },
            [](PTMaterial& self, float v) { self.fuzzRoughness = v; self.gpuDataDirty = true; })
        .def_prop_rw("opacity",
            [](PTMaterial& self) { return self.opacity; },
            [](PTMaterial& self, float v) { self.opacity = v; self.gpuDataDirty = true; })
        .def_prop_rw("geometry_opacity",
            [](PTMaterial& self) { return self.opacity; },
            [](PTMaterial& self, float v) { self.opacity = v; self.gpuDataDirty = true; },
            "OpenPBR alias for opacity.")
        .def_prop_rw("transmission_factor",
            [](PTMaterial& self) { return self.transmissionFactor; },
            [](PTMaterial& self, float v) { self.transmissionFactor = v; self.gpuDataDirty = true; })
        .def_prop_rw("transmission_weight",
            [](PTMaterial& self) { return self.transmissionFactor; },
            [](PTMaterial& self, float v) { SetOpenPBRTransmissionWeightFromPython(self, v); },
            "OpenPBR alias for transmission_factor; toggles enable_transmission from the weight.")
        .def_prop_rw("diffuse_transmission_factor",
            [](PTMaterial& self) { return self.diffuseTransmissionFactor; },
            [](PTMaterial& self, float v) { self.diffuseTransmissionFactor = v; self.gpuDataDirty = true; })
        .def_prop_rw("transmission_diffuse_weight",
            [](PTMaterial& self) { return self.diffuseTransmissionFactor; },
            [](PTMaterial& self, float v) { SetOpenPBRDiffuseTransmissionWeightFromPython(self, v); },
            "OpenPBR alias for diffuse_transmission_factor; toggles enable_transmission from the weight.")
        .def_prop_rw("normal_texture_scale",
            [](PTMaterial& self) { return self.normalTextureScale; },
            [](PTMaterial& self, float v) { self.normalTextureScale = v; self.gpuDataDirty = true; })
        .def_prop_rw("geometry_normal_scale",
            [](PTMaterial& self) { return self.normalTextureScale; },
            [](PTMaterial& self, float v) { self.normalTextureScale = v; self.gpuDataDirty = true; },
            "OpenPBR alias for normal_texture_scale.")
        .def_prop_rw("ior",
            [](PTMaterial& self) { return self.IoR; },
            [](PTMaterial& self, float v) { self.IoR = v; self.gpuDataDirty = true; })
        .def_prop_rw("specular_ior",
            [](PTMaterial& self) { return self.IoR; },
            [](PTMaterial& self, float v) { self.IoR = v; self.gpuDataDirty = true; },
            "OpenPBR alias for ior.")
        .def_prop_rw("alpha_cutoff",
            [](PTMaterial& self) { return self.alphaCutoff; },
            [](PTMaterial& self, float v) { self.alphaCutoff = v; self.gpuDataDirty = true; })
        .def_prop_rw("geometry_alpha_cutoff",
            [](PTMaterial& self) { return self.alphaCutoff; },
            [](PTMaterial& self, float v) { self.alphaCutoff = v; self.gpuDataDirty = true; },
            "OpenPBR alias for alpha_cutoff.")

        .def_prop_rw("volume_attenuation_distance",
            [](PTMaterial& self) { return self.volumeAttenuationDistance; },
            [](PTMaterial& self, float v) { self.volumeAttenuationDistance = v; self.gpuDataDirty = true; })
        .def_prop_rw("volume_attenuation_color",
            [](PTMaterial& self) { return Float3ToTuple(self.volumeAttenuationColor); },
            [](PTMaterial& self, nb::object v) { self.volumeAttenuationColor = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("nested_priority",
            [](PTMaterial& self) { return self.nestedPriority; },
            [](PTMaterial& self, int v) { self.nestedPriority = v; self.gpuDataDirty = true; })

        .def_prop_rw("use_specular_gloss",
            [](PTMaterial& self) { return self.useSpecularGlossModel; },
            [](PTMaterial& self, bool v) { self.useSpecularGlossModel = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_alpha_testing",
            [](PTMaterial& self) { return self.enableAlphaTesting; },
            [](PTMaterial& self, bool v) { self.enableAlphaTesting = v; self.gpuDataDirty = true; })
        .def_prop_rw("geometry_enable_alpha_test",
            [](PTMaterial& self) { return self.enableAlphaTesting; },
            [](PTMaterial& self, bool v) { self.enableAlphaTesting = v; self.gpuDataDirty = true; },
            "OpenPBR UI alias for enable_alpha_testing.")
        .def_prop_rw("enable_transmission",
            [](PTMaterial& self) { return self.enableTransmission; },
            [](PTMaterial& self, bool v) { self.enableTransmission = v; self.gpuDataDirty = true; })
        .def_prop_rw("thin_surface",
            [](PTMaterial& self) { return self.thinSurface; },
            [](PTMaterial& self, bool v) { self.thinSurface = v; self.gpuDataDirty = true; })
        .def_prop_rw("geometry_thin_walled",
            [](PTMaterial& self) { return self.thinSurface; },
            [](PTMaterial& self, bool v) { self.thinSurface = v; self.gpuDataDirty = true; },
            "OpenPBR alias for thin_surface.")
        .def_prop_rw("exclude_from_nee",
            [](PTMaterial& self) { return self.excludeFromNEE; },
            [](PTMaterial& self, bool v) { self.excludeFromNEE = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_as_analytic_light_proxy",
            [](PTMaterial& self) { return self.enableAsAnalyticLightProxy; },
            [](PTMaterial& self, bool v) { self.enableAsAnalyticLightProxy = v; self.gpuDataDirty = true; })
        .def_prop_rw("skip_render",
            [](PTMaterial& self) { return self.skipRender; },
            [](PTMaterial& self, bool v) { self.skipRender = v; self.gpuDataDirty = true; })
        .def_prop_rw("metalness_in_red_channel",
            [](PTMaterial& self) { return self.metalnessInRedChannel; },
            [](PTMaterial& self, bool v) { self.metalnessInRedChannel = v; self.gpuDataDirty = true; })

        .def_prop_rw("enable_base_texture",
            [](PTMaterial& self) { return self.enableBaseTexture; },
            [](PTMaterial& self, bool v) { self.enableBaseTexture = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_base_color_texture",
            [](PTMaterial& self) { return self.enableBaseTexture; },
            [](PTMaterial& self, bool v) { self.enableBaseTexture = v; self.gpuDataDirty = true; },
            "OpenPBR alias for enable_base_texture.")
        .def_prop_rw("enable_orm_texture",
            [](PTMaterial& self) { return self.enableOcclusionRoughnessMetallicTexture; },
            [](PTMaterial& self, bool v) { self.enableOcclusionRoughnessMetallicTexture = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_base_metalness_specular_roughness_texture",
            [](PTMaterial& self) { return self.enableOcclusionRoughnessMetallicTexture; },
            [](PTMaterial& self, bool v) { self.enableOcclusionRoughnessMetallicTexture = v; self.gpuDataDirty = true; },
            "OpenPBR alias for enable_orm_texture.")
        .def_prop_rw("enable_normal_texture",
            [](PTMaterial& self) { return self.enableNormalTexture; },
            [](PTMaterial& self, bool v) { self.enableNormalTexture = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_geometry_normal_texture",
            [](PTMaterial& self) { return self.enableNormalTexture; },
            [](PTMaterial& self, bool v) { self.enableNormalTexture = v; self.gpuDataDirty = true; },
            "OpenPBR alias for enable_normal_texture.")
        .def_prop_rw("enable_emissive_texture",
            [](PTMaterial& self) { return self.enableEmissiveTexture; },
            [](PTMaterial& self, bool v) { self.enableEmissiveTexture = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_emission_color_texture",
            [](PTMaterial& self) { return self.enableEmissiveTexture; },
            [](PTMaterial& self, bool v) { self.enableEmissiveTexture = v; self.gpuDataDirty = true; },
            "OpenPBR alias for enable_emissive_texture.")
        .def_prop_rw("enable_transmission_texture",
            [](PTMaterial& self) { return self.enableTransmissionTexture; },
            [](PTMaterial& self, bool v) { self.enableTransmissionTexture = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_transmission_weight_texture",
            [](PTMaterial& self) { return self.enableTransmissionTexture; },
            [](PTMaterial& self, bool v) { self.enableTransmissionTexture = v; self.gpuDataDirty = true; },
            "OpenPBR alias for enable_transmission_texture.")

        .def_prop_ro("base_texture_path",
            [](PTMaterial& self) { return MaterialTexturePath(self, PTMaterialTextureSlot::Base); })
        .def_prop_ro("orm_texture_path",
            [](PTMaterial& self) { return MaterialTexturePath(self, PTMaterialTextureSlot::OcclusionRoughnessMetallic); })
        .def_prop_ro("normal_texture_path",
            [](PTMaterial& self) { return MaterialTexturePath(self, PTMaterialTextureSlot::Normal); })
        .def_prop_ro("emissive_texture_path",
            [](PTMaterial& self) { return MaterialTexturePath(self, PTMaterialTextureSlot::Emissive); })
        .def_prop_ro("transmission_texture_path",
            [](PTMaterial& self) { return MaterialTexturePath(self, PTMaterialTextureSlot::Transmission); })

        .def("set_texture",
            [](PTMaterial& self, PTMaterialTextureSlot slot, const std::string& path, std::optional<bool> sRGB, std::optional<bool> normalMap) {
                return SetMaterialTextureFromPython(self, slot, path, sRGB, normalMap);
            },
            nb::arg("slot"), nb::arg("path"), nb::arg("srgb") = nb::none(), nb::arg("normal_map") = nb::none(),
            "Replace one material texture slot from a file path. Returns False if the file cannot be resolved.")
        .def("set_base_texture",
            [](PTMaterial& self, const std::string& path, std::optional<bool> sRGB) {
                return SetMaterialTextureFromPython(self, PTMaterialTextureSlot::Base, path, sRGB, false);
            },
            nb::arg("path"), nb::arg("srgb") = nb::none())
        .def("set_orm_texture",
            [](PTMaterial& self, const std::string& path, std::optional<bool> sRGB) {
                return SetMaterialTextureFromPython(self, PTMaterialTextureSlot::OcclusionRoughnessMetallic, path, sRGB, false);
            },
            nb::arg("path"), nb::arg("srgb") = nb::none())
        .def("set_normal_texture",
            [](PTMaterial& self, const std::string& path) {
                return SetMaterialTextureFromPython(self, PTMaterialTextureSlot::Normal, path, false, true);
            },
            nb::arg("path"))
        .def("set_emissive_texture",
            [](PTMaterial& self, const std::string& path, std::optional<bool> sRGB) {
                return SetMaterialTextureFromPython(self, PTMaterialTextureSlot::Emissive, path, sRGB, false);
            },
            nb::arg("path"), nb::arg("srgb") = nb::none())
        .def("set_transmission_texture",
            [](PTMaterial& self, const std::string& path, std::optional<bool> sRGB) {
                return SetMaterialTextureFromPython(self, PTMaterialTextureSlot::Transmission, path, sRGB, false);
            },
            nb::arg("path"), nb::arg("srgb") = nb::none())
        .def("clear_texture",
            [](PTMaterial& self, PTMaterialTextureSlot slot) { ClearMaterialTextureFromPython(self, slot); },
            nb::arg("slot"),
            "Disconnect and disable one material texture slot.")
        .def("clear_base_texture",
            [](PTMaterial& self) { ClearMaterialTextureFromPython(self, PTMaterialTextureSlot::Base); })
        .def("clear_orm_texture",
            [](PTMaterial& self) { ClearMaterialTextureFromPython(self, PTMaterialTextureSlot::OcclusionRoughnessMetallic); })
        .def("clear_normal_texture",
            [](PTMaterial& self) { ClearMaterialTextureFromPython(self, PTMaterialTextureSlot::Normal); })
        .def("clear_emissive_texture",
            [](PTMaterial& self) { ClearMaterialTextureFromPython(self, PTMaterialTextureSlot::Emissive); })
        .def("clear_transmission_texture",
            [](PTMaterial& self) { ClearMaterialTextureFromPython(self, PTMaterialTextureSlot::Transmission); })

        .def("mark_dirty", [](PTMaterial& self) { self.gpuDataDirty = true; },
             "Force this material's GPU buffer slot to be refreshed next frame.")
        .def("__repr__", [](const PTMaterial& self) {
                return std::string("<caustica.Material '") + self.name + "'>";
            });

    // Lights are ECS typed components on SceneNode (no OO Light hierarchy).

    nb::class_<MeshInfo>(m, "Mesh",
        "CPU/GPU mesh wrapper. Use Sample.get_mesh_vertices(mesh) and\n"
        "Sample.set_mesh_vertices(mesh, vertices) for deformation.")
        .def_ro("name", &MeshInfo::name)
        .def_ro("global_mesh_index", &MeshInfo::globalMeshIndex)
        .def_prop_ro("vertex_count", [](MeshInfo& self) { return UniqueMeshPositionCount(self); },
            "Number of unique position vertices returned by Sample.get_mesh_vertices(mesh).")
        .def_ro("index_count", &MeshInfo::totalIndices)
        .def_prop_ro("geometry_count", [](MeshInfo& self) { return self.geometries.size(); })
        .def_prop_ro("bounds", [](MeshInfo& self) -> nb::object {
                return SceneBoundsTuple(ValidSceneBounds(self.objectSpaceBounds));
            },
            "Object-space `((min.xyz), (max.xyz))` AABB for this mesh.")
        .def("__repr__", [](MeshInfo& self) {
                return std::string("<caustica.Mesh '") + self.name
                    + "' vertices=" + std::to_string(self.totalVertices) + ">";
            });

    nb::class_<PySceneEntity>(m, "SceneNode",
        "ECS scene entity wrapper for runtime mesh/light/camera transforms.")
        .def_prop_ro("name", [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                return entityWorld ? entityWorld->getEntityName(self.entity) : std::string{};
            })
        .def_prop_ro("path", [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                return entityWorld ? entityWorld->getEntityPath(self.entity).generic_string() : std::string{};
            })
        .def_prop_ro("mesh", [](PySceneEntity& self) {
                return MeshFromEntity(self);
            }, "Mesh attached to this entity, or None when it is not a mesh instance.")
        .def_prop_ro("is_mesh", [](PySceneEntity& self) {
                return MeshFromEntity(self) != nullptr;
            })
        .def_prop_ro("is_light", [](PySceneEntity& self) {
                return EntityLightType(self) != LightType_None;
            })
        .def_prop_ro("light_type", [](PySceneEntity& self) {
                return EntityLightType(self);
            }, "LightType_* constant, or 0 when this entity is not a light.")
        .def_prop_rw("color",
            [](PySceneEntity& self) {
                if (const dm::float3* color = TryMutableLightColor(self))
                    return Float3ToTuple(*color);
                return Float3ToTuple(dm::float3(1.f));
            },
            [](PySceneEntity& self, nb::object v) {
                if (dm::float3* color = TryMutableLightColor(self))
                    *color = ToFloat3(v);
            },
            "Light color when this node has a light component.")
        .def_prop_rw("intensity",
            [](PySceneEntity& self) {
                const float* intensity = TryMutableLightIntensity(self);
                return intensity ? *intensity : 0.f;
            },
            [](PySceneEntity& self, nb::object v) {
                if (float* intensity = TryMutableLightIntensity(self))
                    *intensity = nb::cast<float>(v);
            },
            "Point/spot luminous intensity.")
        .def_prop_rw("irradiance",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* directional = entityWorld
                    ? scene::tryGetDirectionalLight(entityWorld->world(), self.entity) : nullptr;
                return directional ? directional->irradiance : 0.f;
            },
            [](PySceneEntity& self, nb::object v) {
                if (scene::SceneEntityWorld* entityWorld = self.entityWorld())
                {
                    if (auto* directional = scene::tryGetDirectionalLight(entityWorld->world(), self.entity))
                        directional->irradiance = nb::cast<float>(v);
                }
            })
        .def_prop_rw("angular_size",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* directional = entityWorld
                    ? scene::tryGetDirectionalLight(entityWorld->world(), self.entity) : nullptr;
                return directional ? directional->angularSize : 0.f;
            },
            [](PySceneEntity& self, nb::object v) {
                if (scene::SceneEntityWorld* entityWorld = self.entityWorld())
                {
                    if (auto* directional = scene::tryGetDirectionalLight(entityWorld->world(), self.entity))
                        directional->angularSize = nb::cast<float>(v);
                }
            })
        .def_prop_rw("radius",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld)
                    return 0.f;
                if (const auto* spot = scene::tryGetSpotLight(entityWorld->world(), self.entity))
                    return spot->radius;
                if (const auto* point = scene::tryGetPointLight(entityWorld->world(), self.entity))
                    return point->radius;
                return 0.f;
            },
            [](PySceneEntity& self, nb::object v) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld)
                    return;
                const float radius = nb::cast<float>(v);
                if (auto* spot = scene::tryGetSpotLight(entityWorld->world(), self.entity))
                    spot->radius = radius;
                else if (auto* point = scene::tryGetPointLight(entityWorld->world(), self.entity))
                    point->radius = radius;
            })
        .def_prop_rw("range",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld)
                    return 0.f;
                if (const auto* spot = scene::tryGetSpotLight(entityWorld->world(), self.entity))
                    return spot->range;
                if (const auto* point = scene::tryGetPointLight(entityWorld->world(), self.entity))
                    return point->range;
                return 0.f;
            },
            [](PySceneEntity& self, nb::object v) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld)
                    return;
                const float range = nb::cast<float>(v);
                if (auto* spot = scene::tryGetSpotLight(entityWorld->world(), self.entity))
                    spot->range = range;
                else if (auto* point = scene::tryGetPointLight(entityWorld->world(), self.entity))
                    point->range = range;
            })
        .def_prop_rw("inner_angle",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* spot = entityWorld ? scene::tryGetSpotLight(entityWorld->world(), self.entity) : nullptr;
                return spot ? spot->innerAngle : 0.f;
            },
            [](PySceneEntity& self, nb::object v) {
                if (scene::SceneEntityWorld* entityWorld = self.entityWorld())
                {
                    if (auto* spot = scene::tryGetSpotLight(entityWorld->world(), self.entity))
                        spot->innerAngle = nb::cast<float>(v);
                }
            })
        .def_prop_rw("outer_angle",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* spot = entityWorld ? scene::tryGetSpotLight(entityWorld->world(), self.entity) : nullptr;
                return spot ? spot->outerAngle : 0.f;
            },
            [](PySceneEntity& self, nb::object v) {
                if (scene::SceneEntityWorld* entityWorld = self.entityWorld())
                {
                    if (auto* spot = scene::tryGetSpotLight(entityWorld->world(), self.entity))
                        spot->outerAngle = nb::cast<float>(v);
                }
            })
        .def_prop_rw("environment_path",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* environment = entityWorld
                    ? scene::tryGetEnvironmentLight(entityWorld->world(), self.entity) : nullptr;
                return environment ? environment->path : std::string{};
            },
            [](PySceneEntity& self, nb::object v) {
                if (scene::SceneEntityWorld* entityWorld = self.entityWorld())
                {
                    if (auto* environment = scene::tryGetEnvironmentLight(entityWorld->world(), self.entity))
                        environment->path = nb::cast<std::string>(v);
                }
            },
            "Environment light HDRI path.")
        .def_prop_rw("position",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* global = entityWorld
                    ? entityWorld->world().tryGet<scene::GlobalTransformComponent>(self.entity) : nullptr;
                return Double3ToTuple(global ? scene::getLightPosition(global->transform) : double3(0.0));
            },
            [](PySceneEntity& self, nb::object v) {
                if (scene::SceneEntityWorld* entityWorld = self.entityWorld())
                    scene::setLightWorldPosition(*entityWorld, self.entity, ToDouble3(v));
            },
            "World-space light position (updates local translation).")
        .def_prop_rw("direction",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* global = entityWorld
                    ? entityWorld->world().tryGet<scene::GlobalTransformComponent>(self.entity) : nullptr;
                return Double3ToTuple(global ? scene::getLightDirection(global->transform) : double3(0.0, -1.0, 0.0));
            },
            [](PySceneEntity& self, nb::object v) {
                if (scene::SceneEntityWorld* entityWorld = self.entityWorld())
                    scene::setLightWorldDirection(*entityWorld, self.entity, ToDouble3(v));
            },
            "World-space light direction (updates local rotation).")
        .def_prop_rw("translation",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld)
                    return Double3ToTuple(double3(0.0));
                const auto* local = entityWorld->world().get<scene::LocalTransformComponent>(self.entity);
                return Double3ToTuple(local ? local->translation : double3(0.0));
            },
            [](PySceneEntity& self, nb::object v) {
                if (scene::SceneEntityWorld* entityWorld = self.entityWorld())
                    entityWorld->setTranslation(self.entity, ToDouble3(v));
            },
            "Local translation in scene space.")
        .def_prop_rw("rotation",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld)
                    return DQuatToXYZWTuple(caustica::math::dquat::identity());
                const auto* local = entityWorld->world().get<scene::LocalTransformComponent>(self.entity);
                return DQuatToXYZWTuple(local ? local->rotation : caustica::math::dquat::identity());
            },
            [](PySceneEntity& self, nb::object v) {
                if (scene::SceneEntityWorld* entityWorld = self.entityWorld())
                    entityWorld->setRotation(self.entity, ToDQuatXYZW(v));
            },
            "Local rotation quaternion as `(x, y, z, w)`.")
        .def_prop_rw("euler",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* local = entityWorld ? entityWorld->world().get<scene::LocalTransformComponent>(self.entity) : nullptr;
                const caustica::math::dquat rotation = local ? local->rotation : caustica::math::dquat::identity();
                return Double3ToTuple(DQuatToEulerRadiansXYZ(rotation));
            },
            [](PySceneEntity& self, nb::object v) {
                if (scene::SceneEntityWorld* entityWorld = self.entityWorld())
                    entityWorld->setRotation(self.entity, caustica::math::rotationQuat(ToDouble3(v)));
            },
            "Local XYZ Euler rotation in radians. Assigning this updates the entity rotation quaternion.")
        .def_prop_rw("scaling",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld)
                    return Double3ToTuple(double3(1.0));
                const auto* local = entityWorld->world().get<scene::LocalTransformComponent>(self.entity);
                return Double3ToTuple(local ? local->scaling : double3(1.0));
            },
            [](PySceneEntity& self, nb::object v) {
                if (scene::SceneEntityWorld* entityWorld = self.entityWorld())
                    entityWorld->setScaling(self.entity, ToDouble3(v));
            },
            "Local non-uniform scaling.")
        .def_prop_ro("bounds",
            [](PySceneEntity& self) -> nb::object {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld)
                    return nb::none();
                const auto* bounds = entityWorld->world().get<scene::BoundsComponent>(self.entity);
                return SceneBoundsTuple(ValidSceneBounds(bounds ? bounds->globalBounds : caustica::math::box3::empty()));
            },
            "World-space `((min.xyz), (max.xyz))` AABB for this entity's subgraph.")
        .def("__repr__", [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const std::string name = entityWorld ? entityWorld->getEntityName(self.entity) : std::string{};
                return std::string("<caustica.SceneNode '") + name + "'>";
            });

    // --- Scene ------------------------------------------------------------
    nb::class_<Scene>(m, "Scene",
        "Loaded caustica scene. Material and light access lives here so Python\n"
        "scripts can follow the same shape as the C++ Sample::scene() path.")
        .def("get_materials", [](Scene& self) {
                return GetSceneMaterials(&self);
            }, "Return every PTMaterial in this scene.")

        .def("find_material", [](Scene& self, const std::string& name) {
                return FindSceneMaterial(&self, name);
            }, nb::arg("name"), "Look up a material by Name or uniqueName.")

        .def("find_material_by_id", [](Scene& self, int materialId) {
                return FindSceneMaterialById(&self, materialId);
            }, nb::arg("material_id"),
            "Look up a material by path-tracer GPU index (Material Editor id), with materialID fallback.")

        .def("get_lights", [](Scene& self) {
                return GetSceneLights(&self);
            }, "Return every light entity as SceneNode.")

        .def("find_light", [](Scene& self, const std::string& name) {
                return FindSceneLight(&self, name);
            }, nb::arg("name"), "Look up a light entity by name; returns SceneNode or None.")
        .def("find_node", [](Scene& self, const std::string& path) {
                return FindSceneEntity(&self, path);
            }, nb::arg("path"), "Look up a scene entity by name or path.")
        .def("get_meshes", [](Scene& self) {
                return GetSceneMeshes(&self);
            }, "Return every Mesh in this scene.")
        .def("find_mesh", [](Scene& self, const std::string& name) {
                return FindSceneMesh(&self, name);
            }, nb::arg("name"), "Look up a mesh by mesh name.")

        .def_prop_ro("material_count", [](Scene& self) {
                return GetSceneMaterials(&self).size();
            }, "Number of PTMaterial instances in this scene.")
        .def_prop_ro("mesh_count", [](Scene& self) {
                return self.getMeshes().size();
            }, "Number of meshes in this scene.")
        .def_prop_ro("light_count", [](Scene& self) {
                return self.getLightEntities().size();
            }, "Number of lights in this scene.")

        .def("get_bounds", [](Scene& self) {
                return SceneBoundsTuple(SceneBoundsFromScene(&self));
            },
            "Return this scene's world-space ((min.xyz), (max.xyz)) AABB, or None.")
        .def("get_scene_bounds", [](Scene& self) {
                return SceneBoundsTuple(SceneBoundsFromScene(&self));
            },
            "Alias for get_bounds().")

        .def_prop_ro("bounds", [](Scene& self) {
                return SceneBoundsTuple(SceneBoundsFromScene(&self));
            },
            "World-space axis-aligned bounding box that covers every renderable\n"
            "leaf in the scene (mesh instances, lights, splats, ...).\n"
            "Returns ``((min_x, min_y, min_z), (max_x, max_y, max_z))`` or\n"
            "``None`` when the scene is empty / not refreshed yet.\n"
            "The AABB is recomputed by the engine after every scene load and\n"
            "after each ``Renderer.load_mesh_file`` call.")
        .def_prop_ro("bounds_center", [](Scene& self) {
                return SceneBoundsCenter(SceneBoundsFromScene(&self));
            },
            "Center point of `Scene.bounds`, or ``None`` for an empty scene.")
        .def_prop_ro("bounds_size", [](Scene& self) {
                return SceneBoundsSize(SceneBoundsFromScene(&self));
            },
            "Diagonal extent (max - min) of `Scene.bounds`, or ``None`` for an empty scene.")

        .def("__repr__", [](Scene& self) {
                const auto materialCount = GetSceneMaterials(&self).size();
                const auto lightCount = self.getLightEntities().size();
                return std::string("<caustica.Scene materials=") + std::to_string(materialCount)
                    + " lights=" + std::to_string(lightCount) + ">";
            });

    // --- Runtime UI / sampling parameters --------------------------------
    nb::class_<EnvironmentMapRuntimeParameters>(m, "EnvironmentMapParams",
        "Runtime tweakables applied on top of the EnvironmentLight in the\n"
        "current scene. Mirror of the UI controls in 'Environment'.")
        .def_prop_rw("tint_color",
            [](EnvironmentMapRuntimeParameters& s) { return Float3ToTuple(s.TintColor); },
            [](EnvironmentMapRuntimeParameters& s, nb::object v) { s.TintColor = ToFloat3(v); })
        .def_rw("intensity", &EnvironmentMapRuntimeParameters::Intensity)
        .def_prop_rw("rotation_xyz",
            [](EnvironmentMapRuntimeParameters& s) { return Float3ToTuple(s.RotationXYZ); },
            [](EnvironmentMapRuntimeParameters& s, nb::object v) { s.RotationXYZ = ToFloat3(v); })
        .def_rw("enabled", &EnvironmentMapRuntimeParameters::enabled)
        .def_rw("visible_to_camera", &EnvironmentMapRuntimeParameters::VisibleToCamera)
        .def_prop_rw("hide_source",
            [](EnvironmentMapRuntimeParameters& s) { return !s.VisibleToCamera; },
            [](EnvironmentMapRuntimeParameters& s, bool hide) { s.VisibleToCamera = !hide; });

    nb::class_<PathTracerSettings>(m, "settings",
        "Live renderer session state (path tracer settings and runtime flags).\n"
        "Mutating attributes is equivalent to moving the corresponding ImGui widget.")
        .def_rw("enable_animations",             &PathTracerSettings::EnableAnimations)
        .def_rw("enable_vsync",                  &PathTracerSettings::EnableVsync)
        .def_rw("fps_limiter",                   &PathTracerSettings::FPSLimiter)

        // --- Path tracer top-level mode ----------------------------------
        .def_prop_rw("realtime_mode",
            [](PathTracerSettings& s) { return s.RealtimeMode; },
            [](PathTracerSettings& s, bool realtime) {
                const bool wasRealtime = s.RealtimeMode;
                s.RealtimeMode = realtime;
                if (wasRealtime != s.RealtimeMode)
                {
                    s.ResetAccumulation = true;
                    if (s.RealtimeMode)
                        s.ResetRealtimeCaches = true;
                }
            },
                "True for realtime mode, False for reference / accumulation mode.\n"
                "See `settings.path_tracer_mode` for an enum-flavored version.")
        .def_prop_rw("path_tracer_mode",
            [](PathTracerSettings& s) -> int { return s.RealtimeMode ? 0 /*Realtime*/ : 1 /*Reference*/; },
            [](PathTracerSettings& s, int mode) {
                bool wasRealtime = s.RealtimeMode;
                s.RealtimeMode = (mode == 0);
                if (wasRealtime != s.RealtimeMode)
                {
                    s.ResetAccumulation = true;
                    if (s.RealtimeMode)
                        s.ResetRealtimeCaches = true;
                }
            },
            "Convenience wrapper around `realtime_mode`.\n"
            "Set to caustica.PathTracerMode.Reference or .Realtime.")

        .def_rw("realtime_samples_per_pixel",    &PathTracerSettings::RealtimeSamplesPerPixel)
        .def_rw("accumulation_target",           &PathTracerSettings::AccumulationTarget)
        .def_rw("reset_accumulation",            &PathTracerSettings::ResetAccumulation)
        .def_rw("reset_realtime_caches",         &PathTracerSettings::ResetRealtimeCaches)
        .def_rw("accumulation_aa",               &PathTracerSettings::AccumulationAA)
        .def_rw("accumulation_prewarm_realtime_caches", &PathTracerSettings::AccumulationPreWarmRealtimeCaches)

        .def_rw("bounce_count",                  &PathTracerSettings::BounceCount)
        .def_rw("diffuse_bounce_count",          &PathTracerSettings::DiffuseBounceCount)
        .def_rw("enable_russian_roulette",       &PathTracerSettings::EnableRussianRoulette)
        .def_rw("texture_lod_bias",              &PathTracerSettings::TexLODBias)

        .def_rw("use_nee",                       &PathTracerSettings::UseNEE)
        .def_rw("nee_type",                      &PathTracerSettings::NEEType,
                "0 = uniform, 1 = power-based, 2 = NEE-AT")
        .def_rw("nee_candidate_samples",         &PathTracerSettings::NEECandidateSamples)
        .def_rw("nee_full_samples",              &PathTracerSettings::NEEFullSamples)
        .def_rw("nee_mis_type",                  &PathTracerSettings::NEEMISType)

        .def_rw("use_restir_di",                 &PathTracerSettings::UseReSTIRDI)
        .def_rw("use_restir_gi",                 &PathTracerSettings::UseReSTIRGI)
        .def_rw("use_restir_pt",                 &PathTracerSettings::UseReSTIRPT)

        .def_rw("camera_aperture",               &PathTracerSettings::CameraAperture)
        .def_rw("camera_focal_distance",         &PathTracerSettings::CameraFocalDistance)
        .def_rw("camera_move_speed",             &PathTracerSettings::CameraMoveSpeed)

        .def_rw("realtime_firefly_filter_enabled", &PathTracerSettings::RealtimeFireflyFilterEnabled)
        .def_rw("realtime_firefly_filter_threshold", &PathTracerSettings::RealtimeFireflyFilterThreshold)
        .def_rw("reference_firefly_filter_enabled",  &PathTracerSettings::ReferenceFireflyFilterEnabled)
        .def_rw("reference_firefly_filter_threshold",&PathTracerSettings::ReferenceFireflyFilterThreshold)

        .def_rw("enable_tone_mapping",           &PathTracerSettings::EnableToneMapping)
        .def_rw("enable_bloom",                  &PathTracerSettings::EnableBloom)
        .def_rw("bloom_intensity",               &PathTracerSettings::BloomIntensity)
        .def_rw("bloom_radius",                  &PathTracerSettings::BloomRadius)

        .def_rw("enable_gaussian_splats",        &PathTracerSettings::EnableGaussianSplats)
        .def_rw("gaussian_splat_depth_test",     &PathTracerSettings::GaussianSplatDepthTest)
        .def_rw("gaussian_splat_shadows",        &PathTracerSettings::GaussianSplatShadows)
        .def_rw("gaussian_splat_hybrid_shadows", &PathTracerSettings::GaussianSplatShadows)
        .def_rw("gaussian_splat_shadows_mode",   &PathTracerSettings::GaussianSplatShadowsMode,
                "3DGS shadow mode (caustica.GaussianSplatShadowMode).")
        .def_rw("gaussian_splat_sorting_mode",   &PathTracerSettings::GaussianSplatSortingMode,
                "3DGS sort mode (caustica.GaussianSplatSortMode).")
        .def_rw("gaussian_splat_sh_format",      &PathTracerSettings::GaussianSplatSHFormat,
                "3DGS SH storage format (caustica.GaussianSplatStorageFormat).")
        .def_rw("gaussian_splat_rgba_format",    &PathTracerSettings::GaussianSplatRGBAFormat,
                "3DGS RGBA storage format (caustica.GaussianSplatStorageFormat).")
        .def_rw("gaussian_splat_use_aabbs",      &PathTracerSettings::GaussianSplatUseAABBs)
        .def_rw("gaussian_splat_use_tlas_instances", &PathTracerSettings::GaussianSplatUseTLASInstances)
        .def_rw("gaussian_splat_blas_compaction", &PathTracerSettings::GaussianSplatBlasCompaction)
        .def_rw("gaussian_splat_rtx_kernel_degree", &PathTracerSettings::GaussianSplatRtxKernelDegree)
        .def_rw("gaussian_splat_rtx_adaptive_clamp", &PathTracerSettings::GaussianSplatRtxAdaptiveClamp)
        .def_rw("gaussian_splat_rtx_alpha_clamp", &PathTracerSettings::GaussianSplatRtxAlphaClamp)
        .def_rw("gaussian_splat_rtx_minimum_transmittance", &PathTracerSettings::GaussianSplatRtxMinimumTransmittance)
        .def_rw("gaussian_splat_rtx_trace_strategy", &PathTracerSettings::GaussianSplatRtxTraceStrategy)
        .def_rw("gaussian_splat_rtx_particle_samples_per_pass", &PathTracerSettings::GaussianSplatRtxParticleSamplesPerPass)
        .def_rw("gaussian_splat_rtx_maximum_pass_count", &PathTracerSettings::GaussianSplatRtxMaximumPassCount)
        .def_rw("gaussian_splat_rtx_particle_shadow_offset", &PathTracerSettings::GaussianSplatRtxParticleShadowOffset)
        .def_rw("gaussian_splat_rtx_particle_shadow_threshold", &PathTracerSettings::GaussianSplatRtxParticleShadowThreshold)
        .def_rw("gaussian_splat_rtx_colored_shadow_strength", &PathTracerSettings::GaussianSplatRtxColoredShadowStrength)
        .def_rw("gaussian_splat_rtx_mesh_composite_threshold", &PathTracerSettings::GaussianSplatRtxMeshCompositeThreshold)
        .def_rw("gaussian_splat_rtx_depth_iso_threshold", &PathTracerSettings::GaussianSplatRtxDepthIsoThreshold)
        .def_rw("gaussian_splat_mip_antialiasing", &PathTracerSettings::GaussianSplatMipAntialiasing)
        .def_rw("gaussian_splat_quantize_normals", &PathTracerSettings::GaussianSplatQuantizeNormals)
        .def_rw("gaussian_splat_ftb_sync_mode", &PathTracerSettings::GaussianSplatFTBSyncMode,
                "3DGS front-to-back synchronization mode (caustica.GaussianSplatFTBSyncMode).")
        .def_rw("gaussian_splat_depth_iso_threshold", &PathTracerSettings::GaussianSplatDepthIsoThreshold)
        .def_rw("gaussian_splat_fragment_shader_barycentric", &PathTracerSettings::GaussianSplatFragmentShaderBarycentric)
        .def_rw("gaussian_splat_frustum_culling", &PathTracerSettings::GaussianSplatFrustumCulling,
                "3DGS frustum culling mode (caustica.GaussianSplatFrustumCulling).")
        .def_rw("gaussian_splat_frustum_dilation", &PathTracerSettings::GaussianSplatFrustumDilation)
        .def_rw("gaussian_splat_screen_size_culling", &PathTracerSettings::GaussianSplatScreenSizeCulling)
        .def_rw("gaussian_splat_min_pixel_coverage", &PathTracerSettings::GaussianSplatMinPixelCoverage)
        .def_rw("gaussian_splat_scale",          &PathTracerSettings::GaussianSplatScale)
        .def_rw("gaussian_splat_alpha_scale",    &PathTracerSettings::GaussianSplatAlphaScale)
        .def_rw("gaussian_splat_brightness",     &PathTracerSettings::GaussianSplatBrightness)
        .def_prop_rw("gaussian_splat_tint_color",
            [](PathTracerSettings& s) { return Float3ToTuple(s.GaussianSplatTintColor); },
            [](PathTracerSettings& s, nb::object v) { s.GaussianSplatTintColor = ToFloat3(v); })
        .def_rw("gaussian_splat_as_emitter",     &PathTracerSettings::GaussianSplatAsEmitter)
        .def_rw("gaussian_splat_emission_intensity", &PathTracerSettings::GaussianSplatEmissionIntensity)
        .def_rw("gaussian_splat_emission_max_proxy_count", &PathTracerSettings::GaussianSplatEmissionMaxProxyCount)
        .def_rw("gaussian_splat_alpha_cull_threshold", &PathTracerSettings::GaussianSplatAlphaCullThreshold)
        .def_rw("gaussian_splat_shadow_strength", &PathTracerSettings::GaussianSplatShadowStrength)
        .def_rw("gaussian_splat_shadow_soft_radius", &PathTracerSettings::GaussianSplatShadowSoftRadius)
        .def_rw("gaussian_splat_shadow_soft_sample_count", &PathTracerSettings::GaussianSplatShadowSoftSampleCount)
        .def_prop_rw("gaussian_splat_translation",
            [](PathTracerSettings& s) { return Float3ToTuple(s.GaussianSplatTranslation); },
            [](PathTracerSettings& s, nb::object v) {
                s.GaussianSplatTranslation = ToFloat3(v);
                s.ResetAccumulation = true;
            })
        .def_prop_rw("gaussian_splat_rotation_euler_deg",
            [](PathTracerSettings& s) { return Float3ToTuple(s.GaussianSplatRotationEulerDeg); },
            [](PathTracerSettings& s, nb::object v) {
                s.GaussianSplatRotationEulerDeg = ToFloat3(v);
                s.ResetAccumulation = true;
            })
        .def_prop_rw("gaussian_splat_object_scale",
            [](PathTracerSettings& s) { return Float3ToTuple(s.GaussianSplatObjectScale); },
            [](PathTracerSettings& s, nb::object v) {
                s.GaussianSplatObjectScale = ToFloat3(v);
                s.ResetAccumulation = true;
            })

        // --- AA / DLSS / DLSS-RR / DLSS-G / Reflex (realtime only) -------
        .def_rw("realtime_aa",                   &PathTracerSettings::RealtimeAA,
                "Realtime AA mode (caustica.RealtimeAA enum):\n"
                "  0 = Off, 1 = TAA, 2 = DLSS, 3 = DLSS-RR")

        // DLSS quality (caustica.DLSSMode enum -> SI::DLSSMode underlying uint32)
        .def_prop_rw("dlss_mode",
            [](PathTracerSettings& s) { return int(s.DLSSMode); },
            [](PathTracerSettings& s, int v) { s.DLSSMode = SI::DLSSMode(v); },
            "DLSS quality preset (caustica.DLSSMode).\n"
            "Off, MaxPerformance, Balanced, MaxQuality, UltraPerformance, UltraQuality, DLAA.")
        .def_rw("dlss_lod_bias_use_override", &PathTracerSettings::DLSSLodBiasUseOverride)
        .def_rw("dlss_lod_bias_override",     &PathTracerSettings::DLSSLodBiasOverride)
        .def_rw("dlss_always_use_extents",    &PathTracerSettings::DLSSAlwaysUseExtents)

        // DLSS-G (frame generation)
        .def_prop_rw("dlss_fg_mode",
            [](PathTracerSettings& s) { return int(s.DLSSFGMode); },
            [](PathTracerSettings& s, int v) { s.DLSSFGMode = SI::DLSSGMode(v); },
            "DLSS frame generation mode (caustica.DLSSFGMode).")
        .def_rw("dlss_fg_multiplier",            &PathTracerSettings::DLSSFGMultiplier)
        .def_rw("dlss_fg_num_frames_to_generate",&PathTracerSettings::DLSSFGNumFramesToGenerate)
        .def_rw("dlss_fg_max_num_frames_to_generate",&PathTracerSettings::DLSSFGMaxNumFramesToGenerate)

        // DLSS-RR (ray reconstruction)
        .def_prop_rw("dlss_rr_preset",
            [](PathTracerSettings& s) { return int(s.DLSRRPreset); },
            [](PathTracerSettings& s, int v) { s.DLSRRPreset = SI::DLSSRRPreset(v); },
            "DLSS-RR preset (caustica.DLSSRRPreset).")
        .def_rw("dlss_rr_micro_jitter",          &PathTracerSettings::DLSSRRMicroJitter)
        .def_rw("dlss_rr_brightness_clamp_k",    &PathTracerSettings::DLSSRRBrightnessClampK)
        .def_rw("disable_restirs_with_dlss_rr",  &PathTracerSettings::DisableReSTIRsWithDLSSRR)

        // Reflex (low latency)
        .def_rw("reflex_mode",                   &PathTracerSettings::ReflexMode,
                "NVIDIA Reflex mode (caustica.ReflexMode).")
        .def_rw("reflex_capped_fps",             &PathTracerSettings::ReflexCappedFps)

        // --- read-only support flags -------------------------------------
        .def_ro("is_dlss_supported",     &PathTracerSettings::IsDLSSSuported)
        .def_ro("is_dlss_fg_supported",  &PathTracerSettings::IsDLSSFGSupported)
        .def_ro("is_dlss_rr_supported",  &PathTracerSettings::IsDLSSRRSupported)
        .def_ro("is_reflex_supported",   &PathTracerSettings::IsReflexSupported)

        // --- Standalone NRD denoiser (realtime, RealtimeAA != DLSS-RR) ---
        .def_rw("standalone_denoiser",           &PathTracerSettings::StandaloneDenoiser,
                "Enable NRD denoiser in realtime mode (no effect with DLSS-RR).")
        .def_rw("denoiser_radiance_clamp_k",     &PathTracerSettings::DenoiserRadianceClampK)

        // --- OIDN reference-mode denoiser --------------------------------
        .def_rw("oidn_enabled",            &PathTracerSettings::ReferenceOIDNDenoiser,
                "(Reference mode) Run Intel Open Image denoise after accumulation reaches the SPP target.")
        .def_rw("oidn_use_gpu",            &PathTracerSettings::ReferenceOIDNUseGPU,
                "Use OIDN GPU device (CUDA/HIP/SYCL) when available, else CPU.")
        .def_rw("oidn_passes",             &PathTracerSettings::ReferenceOIDNPasses,
                "Auxiliary guide passes (caustica.OidnPasses).")
        .def_rw("oidn_prefilter",          &PathTracerSettings::ReferenceOIDNPrefilter,
                "Prefilter quality for guide passes (caustica.OidnPrefilter).")
        .def_rw("oidn_quality",            &PathTracerSettings::ReferenceOIDNQuality,
                "Beauty filter quality (caustica.OidnQuality).")
        .def_rw("oidn_changed",            &PathTracerSettings::ReferenceOIDNDenoiserChanged,
                "Set to True after editing any OIDN parameter to force a redenoise.\n"
                "Cleared automatically by the renderer.")
        .def("oidn_apply", [](PathTracerSettings& s) { s.ReferenceOIDNDenoiserChanged = true; },
             "Mark OIDN parameters dirty so the next accumulation completion runs the filter again.")

        .def_rw("environment_map",               &PathTracerSettings::EnvironmentMapParams,
                nb::rv_policy::reference_internal,
                "EnvironmentMapParams structure (intensity, tint, rotation, enabled, visible_to_camera).")
        ;

    nb::class_<EditorUIData>(m, "EditorSettings",
        "Desktop-editor settings that extend `settings` with ImGui view state.")
        .def_prop_rw("show_ui",
            [](EditorUIData& ui) { return ui.editor.ShowUI; },
            [](EditorUIData& ui, bool value) { ui.editor.ShowUI = value; });

    // --- Sample (top-level renderer access) -------------------------------
    nb::class_<App>(m, "Sample",
        "caustica renderer instance. In embed mode use caustica.app(); in extension\n"
        "mode use Renderer.app to retrieve the underlying instance.")
        .def_prop_ro("settings", [](App& self) -> PathTracerSettings* {
                return caustica::settings(self);
            }, nb::rv_policy::reference,
            "Live `settings` mirror of the current UI state.")
        .def_prop_ro("scene", [](App& self) {
                return caustica::activeScene(self);
            }, "Current loaded `Scene`, or None before a scene is available.")

        .def_prop_ro("scene_name",  [](App& self) { return caustica::currentSceneName(self); })
        .def_prop_ro("available_scenes", [](App& self) { return caustica::availableScenes(self); })

        .def("get_scene", [](App& self) {
                return caustica::activeScene(self);
            }, "Return the current loaded Scene, matching the C++ scene() entry point.")

        .def("set_scene", [](App& self, const std::string& name, bool forceReload)
            {
                caustica::setCurrentScene(self, name, forceReload);
            },
            nb::arg("scene_name"), nb::arg("force_reload") = false,
            "Switch to a different scene file from caustica.Sample.available_scenes.")

        .def("load_gaussian_splats", [](App& self, const std::string& fileName, bool convertRdfToRub)
            {
                return caustica::loadGaussianSplatFile(self, fileName, convertRdfToRub);
            },
            nb::arg("file_name"), nb::arg("convert_rdf_to_rub") = true,
            "load a 3DGS .ply file and rasterize it over the current scene.")

        .def_prop_ro("gaussian_splat_count", [](App& self) { return caustica::gaussianSplatCount(self); })
        .def_prop_ro("gaussian_splat_object_count", [](App& self) { return caustica::gaussianSplatObjectCount(self); })
        .def_prop_ro("gaussian_splat_file_name", [](App& self) { return caustica::gaussianSplatFileName(self); })

        .def("get_materials", [](App& self) {
                return GetSceneMaterials(caustica::activeScene(self).get());
            }, "Compatibility alias for `sample.scene.get_materials()`.")

        .def("find_material", [](App& self, const std::string& name) -> std::shared_ptr<PTMaterial> {
                return FindSceneMaterial(caustica::activeScene(self).get(), name);
            }, nb::arg("name"), "Compatibility alias for `sample.scene.find_material(name)`.")

        .def("find_material_by_id", [](App& self, int materialId) -> std::shared_ptr<PTMaterial> {
                return FindSceneMaterialById(caustica::activeScene(self).get(), materialId);
            }, nb::arg("material_id"), "Compatibility alias for `sample.scene.find_material_by_id(material_id)`.")

        .def("get_lights", [](App& self) {
                return GetSceneLights(caustica::activeScene(self).get());
            }, "Compatibility alias for `sample.scene.get_lights()`.")

        .def("get_scene_bounds", [](App& self) {
                return SceneBoundsTuple(SceneBoundsFromScene(caustica::activeScene(self)));
            },
            "Compatibility alias for `sample.scene.get_scene_bounds()`.")

        .def_prop_ro("scene_bounds", [](App& self) {
                return SceneBoundsTuple(SceneBoundsFromScene(caustica::activeScene(self)));
            },
            "Shortcut for `sample.scene.bounds`. Returns the world-space\n"
            "((min.xyz), (max.xyz)) AABB or `None` if no scene is loaded.")
        .def_prop_ro("scene_bounds_center", [](App& self) {
                return SceneBoundsCenter(SceneBoundsFromScene(caustica::activeScene(self)));
            }, "Shortcut for `sample.scene.bounds_center` (or `None`).")
        .def_prop_ro("scene_bounds_size", [](App& self) {
                return SceneBoundsSize(SceneBoundsFromScene(caustica::activeScene(self)));
            }, "Shortcut for `sample.scene.bounds_size` (or `None`).")

        .def("find_light", [](App& self, const std::string& name) -> std::shared_ptr<PySceneEntity> {
                return FindSceneLight(caustica::activeScene(self).get(), name);
            }, nb::arg("name"), "Compatibility alias for `sample.scene.find_light(name)`.")
        .def("find_node", [](App& self, const std::string& path) -> std::shared_ptr<PySceneEntity> {
                return FindSceneEntity(caustica::activeScene(self).get(), path);
            }, nb::arg("path"), "Compatibility alias for `sample.scene.find_node(path)`.")

        .def("get_meshes", [](App& self) {
                return GetSceneMeshes(caustica::activeScene(self).get());
            }, "Compatibility alias for `sample.scene.get_meshes()`.")
        .def("find_mesh", [](App& self, const std::string& name) -> std::shared_ptr<MeshInfo> {
                return FindSceneMesh(caustica::activeScene(self).get(), name);
            }, nb::arg("name"), "Compatibility alias for `sample.scene.find_mesh(name)`.")

        .def("set_environment_map", [](App& self, const std::string& path) {
                caustica::setEnvMapOverrideSource(self, path);
            }, nb::arg("path"))

        .def("get_camera_pos_dir_up", [](App& self) {
                return caustica::currentCameraPosDirUp(self);
            }, "Returns a comma-separated string of pos.xyz, dir.xyz, up.xyz.")

        .def("set_camera_pos_dir_up", [](App& self, const std::string& v) {
                return caustica::setCurrentCameraPosDirUp(self, v);
            }, nb::arg("pos_dir_up"))

        .def("set_camera_fov", [](App& self, float fov) {
                caustica::setCameraVerticalFOV(self, caustica::math::radians(fov));
            },
            nb::arg("vertical_fov_degrees"))

        .def("set_camera_intrinsics",
            [](App& self, float fx, float fy, float cx, float cy, float width, float height) {
                caustica::setCameraIntrinsics(self, fx, fy, cx, cy, width, height);
            },
            nb::arg("fx"), nb::arg("fy"), nb::arg("cx"), nb::arg("cy"), nb::arg("width"), nb::arg("height"))

        .def("get_camera_fov", [](App& self) { return caustica::cameraVerticalFOV(self); })

        .def("save_current_camera",  [](App& self) { caustica::saveCurrentCamera(self); })
        .def("load_current_camera",  [](App& self) { caustica::loadCurrentCamera(self); })

        .def("request_shader_reload",  [](App& self) {
                self.resource<RenderAppState>().runtime.Invalidation.ShaderReloadRequested = true;
            })
        .def("request_accel_rebuild",  [](App& self) {
                self.resource<RenderAppState>().runtime.Invalidation.AccelerationStructRebuildRequested = true;
            })
        .def("request_mesh_accel_rebuild",
            [](App& self, const std::shared_ptr<MeshInfo>& mesh) {
                caustica::requestMeshAccelRebuild(self, mesh);
            },
            nb::arg("mesh"),
            "Request a BLAS rebuild for one dirty mesh without forcing a full scene AS rebuild.")
        .def("request_mesh_accel_rebuild",
            [](App& self, const std::shared_ptr<PySceneEntity>& node) {
                if (!node)
                    throw std::runtime_error("request_mesh_accel_rebuild: node is null");
                std::shared_ptr<MeshInfo> mesh = MeshFromEntity(*node);
                if (!mesh)
                    throw std::runtime_error("request_mesh_accel_rebuild: entity has no mesh");
                caustica::requestMeshAccelRebuild(self, mesh);
            },
            nb::arg("node"),
            "Request a BLAS rebuild for the mesh attached to one scene entity.")
        .def("reset_accumulation",     [](App& self) {
                if (auto* s = caustica::settings(self))
                    s->ResetAccumulation = true;
            })
        .def("reset_realtime_caches",  [](App& self) {
                if (auto* s = caustica::settings(self))
                    s->ResetRealtimeCaches = true;
            })

        .def("set_realtime_mode", [](App& self, bool standaloneDenoiser, int realtimeAA)
            {
                PathTracerSettings& settings = *caustica::settings(self);
                if (!settings.RealtimeMode)
                {
                    settings.ResetAccumulation = true;
                    settings.ResetRealtimeCaches = true;
                }
                settings.RealtimeMode      = true;
                settings.StandaloneDenoiser = standaloneDenoiser;
                settings.RealtimeAA         = realtimeAA;
            },
            nb::arg("standalone_denoiser") = true,
            nb::arg("realtime_aa") = 2 /*DLSS*/,
            "Switch to realtime path tracing.\n"
            "Args:\n"
            "    standalone_denoiser: enable NRD (no effect with DLSS-RR)\n"
            "    realtime_aa        : 0=Off, 1=TAA, 2=DLSS, 3=DLSS-RR")

        .def("set_reference_mode", [](App& self, int spp, bool oidn, int oidnQuality, int oidnPasses, int oidnPrefilter)
            {
                PathTracerSettings& settings = *caustica::settings(self);
                if (settings.RealtimeMode)
                    settings.ResetAccumulation = true;
                settings.RealtimeMode             = false;
                if (spp > 0)
                    settings.AccumulationTarget   = spp;
                settings.ReferenceOIDNDenoiser    = oidn;
                settings.ReferenceOIDNQuality     = oidnQuality;
                settings.ReferenceOIDNPasses      = oidnPasses;
                settings.ReferenceOIDNPrefilter   = oidnPrefilter;
                settings.ReferenceOIDNDenoiserChanged = true;
            },
            nb::arg("spp") = 0,
            nb::arg("oidn") = false,
            nb::arg("oidn_quality")   = 1 /*Balanced*/,
            nb::arg("oidn_passes")    = 1 /*Albedo*/,
            nb::arg("oidn_prefilter") = 1 /*Fast*/,
            "Switch to reference / accumulation rendering.\n"
            "Args:\n"
            "    spp           : reference SPP target (0 to keep current).\n"
            "    oidn          : run OIDN once accumulation hits the SPP target.\n"
            "    oidn_quality  : caustica.OidnQuality (0=Fast, 1=Balanced, 2=High)\n"
            "    oidn_passes   : caustica.OidnPasses (0=ColorOnly, 1=Albedo, 2=AlbedoNormal)\n"
            "    oidn_prefilter: caustica.OidnPrefilter (0=None, 1=Fast, 2=Accurate)")

        .def_prop_ro("accumulation_completed",
            [](App& self) { return caustica::accumulationCompleted(self); })
        .def_prop_ro("accumulation_sample_index",
            [](App& self) { return caustica::accumulationSampleIndex(self); })
        ;

    nb::class_<SceneEditor>(m, "EditorSample",
        "Editor-only Sample extensions (mesh import and vertex deformation).")
        .def("load_mesh_file", [](SceneEditor& self, const std::string& fileName)
            {
                return self.loadMeshFile(fileName);
            },
            nb::arg("file_name"),
            "Append a mesh file (.gltf, .glb, or .obj) to the current scene.")
        .def("get_mesh_vertices", [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh) {
                return Float3VectorToList(self.getMeshVertices(mesh));
            }, nb::arg("mesh"),
            "Return unique mesh positions as a list of (x, y, z) tuples in object space.")
        .def("set_mesh_vertices",
            [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh, nb::object vertices,
               bool recomputeNormals, bool rebuildAccelerationStructure) {
                self.setMeshVertices(mesh, ToFloat3Vector(vertices), recomputeNormals, rebuildAccelerationStructure);
            },
            nb::arg("mesh"), nb::arg("vertices"), nb::arg("recompute_normals") = true,
            nb::arg("rebuild_acceleration_structure") = true,
            "Replace all unique object-space positions for a mesh and refresh GPU buffers.")
        .def("deform_mesh",
            [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh, nb::object callback,
               bool recomputeNormals, bool rebuildAccelerationStructure) {
                std::vector<float3> vertices = self.getMeshVertices(mesh);
                for (size_t i = 0; i < vertices.size(); ++i)
                {
                    nb::object updated = callback(i, Float3ToTuple(vertices[i]));
                    if (!updated.is_none())
                        vertices[i] = ToFloat3(updated);
                }
                self.setMeshVertices(mesh, vertices, recomputeNormals, rebuildAccelerationStructure);
                return vertices.size();
            },
            nb::arg("mesh"), nb::arg("callback"), nb::arg("recompute_normals") = true,
            nb::arg("rebuild_acceleration_structure") = true,
            "apply a Python callback to each unique vertex.")
        .def("get_mesh_vertices_world", [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh) {
                return Float3VectorToList(self.getMeshVerticesWorld(mesh));
            }, nb::arg("mesh"))
        .def("get_mesh_vertices_world", [](SceneEditor& self, const std::shared_ptr<PySceneEntity>& node) {
                return Float3VectorToList(self.getMeshVerticesWorld(EntityHandleFromPyNode(node)));
            }, nb::arg("node"))
        .def("set_mesh_vertices_world",
            [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh, nb::object vertices,
               bool recomputeNormals, bool rebuildAccelerationStructure) {
                self.setMeshVerticesWorld(mesh, ToFloat3Vector(vertices), recomputeNormals, rebuildAccelerationStructure);
            },
            nb::arg("mesh"), nb::arg("vertices"), nb::arg("recompute_normals") = true,
            nb::arg("rebuild_acceleration_structure") = true)
        .def("set_mesh_vertices_world",
            [](SceneEditor& self, const std::shared_ptr<PySceneEntity>& node, nb::object vertices,
               bool recomputeNormals, bool rebuildAccelerationStructure) {
                self.setMeshVerticesWorld(EntityHandleFromPyNode(node), ToFloat3Vector(vertices), recomputeNormals, rebuildAccelerationStructure);
            },
            nb::arg("node"), nb::arg("vertices"), nb::arg("recompute_normals") = true,
            nb::arg("rebuild_acceleration_structure") = true)
        .def("deform_mesh_world",
            [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh, nb::object callback,
               bool recomputeNormals, bool rebuildAccelerationStructure) {
                std::vector<float3> vertices = self.getMeshVerticesWorld(mesh);
                for (size_t i = 0; i < vertices.size(); ++i)
                {
                    nb::object updated = callback(i, Float3ToTuple(vertices[i]));
                    if (!updated.is_none())
                        vertices[i] = ToFloat3(updated);
                }
                self.setMeshVerticesWorld(mesh, vertices, recomputeNormals, rebuildAccelerationStructure);
                return vertices.size();
            },
            nb::arg("mesh"), nb::arg("callback"), nb::arg("recompute_normals") = true,
            nb::arg("rebuild_acceleration_structure") = true)
        .def("deform_mesh_world",
            [](SceneEditor& self, const std::shared_ptr<PySceneEntity>& node, nb::object callback,
               bool recomputeNormals, bool rebuildAccelerationStructure) {
                const ecs::Entity entity = EntityHandleFromPyNode(node);
                std::vector<float3> vertices = self.getMeshVerticesWorld(entity);
                for (size_t i = 0; i < vertices.size(); ++i)
                {
                    nb::object updated = callback(i, Float3ToTuple(vertices[i]));
                    if (!updated.is_none())
                        vertices[i] = ToFloat3(updated);
                }
                self.setMeshVerticesWorld(entity, vertices, recomputeNormals, rebuildAccelerationStructure);
                return vertices.size();
            },
            nb::arg("node"), nb::arg("callback"), nb::arg("recompute_normals") = true,
            nb::arg("rebuild_acceleration_structure") = true)
        ;
}

} // namespace caustica_py

#endif // CAUSTICA_WITH_PYTHON
