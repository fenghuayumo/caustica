#if CAUSTICA_WITH_PYTHON

#include "PythonBindingsCore.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/optional.h>
#include <nanobind/operators.h>

#include "SceneEditor.h"
#include <render/RenderSessionState.h>
#include <EditorUI.h>
#include <scene/Scene.h>
#include <render/Passes/Lighting/MaterialsBaker.h>
#include <render/Passes/Lighting/LightsBaker.h>
#include <render/Passes/OMM/OmmBaker.h>

#include <scene/Scene.h>
#include <scene/SceneTypes.h>
#include <scene/SceneGraph.h>
#include <core/log.h>
#include <math/math.h>

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
using caustica::math::float2;
using caustica::math::float3;
using caustica::math::float4;
using caustica::math::double3;
using caustica::math::double4;
using caustica::editor::SceneEditor;
using caustica::editor::SampleUIData;
using caustica::render::RenderSessionState;

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
        return normalized == "openpbr" || normalized == "openpbr-lite" || normalized == "openpbr_lite";
    }

    void SetMaterialModelFromPython(PTMaterial& self, const std::string& value)
    {
        self.MaterialModel = value;
        if (IsOpenPBRMaterialModelName(value))
        {
            self.UseSpecularGlossModel = false;
            if (self.SpecularColor.x == 0.f && self.SpecularColor.y == 0.f && self.SpecularColor.z == 0.f)
                self.SpecularColor = float3(1.f);
        }
        self.GPUDataDirty = true;
    }

    void SetOpenPBRTransmissionWeightFromPython(PTMaterial& self, float value)
    {
        self.TransmissionFactor = value;
        self.EnableTransmission = self.TransmissionFactor > 0.f || self.DiffuseTransmissionFactor > 0.f;
        self.GPUDataDirty = true;
    }

    void SetOpenPBRDiffuseTransmissionWeightFromPython(PTMaterial& self, float value)
    {
        self.DiffuseTransmissionFactor = value;
        self.EnableTransmission = self.TransmissionFactor > 0.f || self.DiffuseTransmissionFactor > 0.f;
        self.GPUDataDirty = true;
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

    std::shared_ptr<SceneGraph> SceneGraphFromScene(const std::shared_ptr<Scene>& scene)
    {
        return scene ? scene->GetSceneGraph() : nullptr;
    }

    std::vector<std::shared_ptr<PTMaterial>> GetSceneMaterials(const std::shared_ptr<SceneGraph>& sceneGraph)
    {
        std::vector<std::shared_ptr<PTMaterial>> result;
        if (!sceneGraph)
            return result;

        for (const auto& mat : sceneGraph->GetMaterials())
        {
            if (auto pt = PTMaterial::SafeCast(mat))
                result.push_back(pt);
        }
        return result;
    }

    std::shared_ptr<PTMaterial> FindSceneMaterial(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& name)
    {
        if (!sceneGraph)
            return nullptr;

        for (const auto& mat : sceneGraph->GetMaterials())
        {
            auto pt = PTMaterial::SafeCast(mat);
            if (pt && (pt->Name == name || pt->UniqueName == name))
                return pt;
        }
        return nullptr;
    }

    std::shared_ptr<PTMaterial> FindSceneMaterialById(const std::shared_ptr<SceneGraph>& sceneGraph, int materialId)
    {
        if (!sceneGraph)
            return nullptr;

        for (const auto& mat : sceneGraph->GetMaterials())
        {
            if (mat && mat->materialID == materialId)
                return PTMaterial::SafeCast(mat);
        }
        return nullptr;
    }

    std::vector<std::shared_ptr<Light>> GetSceneLights(const std::shared_ptr<SceneGraph>& sceneGraph)
    {
        std::vector<std::shared_ptr<Light>> result;
        if (!sceneGraph)
            return result;

        for (const auto& light : sceneGraph->GetLights())
            result.push_back(light);
        return result;
    }

    std::shared_ptr<Light> FindSceneLight(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& name)
    {
        if (!sceneGraph)
            return nullptr;

        for (const auto& light : sceneGraph->GetLights())
        {
            if (light && light->GetNode() && light->GetNode()->GetName() == name)
                return light;
        }
        return nullptr;
    }

    std::shared_ptr<SceneGraphNode> FindSceneNode(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& path)
    {
        if (!sceneGraph || path.empty())
            return nullptr;

        std::filesystem::path query(path);
        if (query.is_absolute())
            return sceneGraph->FindNode(query);

        if (auto node = sceneGraph->FindNode(std::filesystem::path("/") / query))
            return node;

        // Simple name lookup is convenient from Python, especially when the
        // caller doesn't know the full scene graph path ahead of time.
        if (query.has_parent_path())
            return nullptr;

        auto root = sceneGraph->GetRootNode();
        if (!root)
            return nullptr;

        SceneGraphWalker walker(root.get());
        while (walker)
        {
            if (walker->GetName() == path)
                return walker->shared_from_this();
            walker.Next(true);
        }

        return nullptr;
    }

    std::vector<std::shared_ptr<MeshInfo>> GetSceneMeshes(const std::shared_ptr<SceneGraph>& sceneGraph)
    {
        std::vector<std::shared_ptr<MeshInfo>> result;
        if (!sceneGraph)
            return result;

        for (const auto& mesh : sceneGraph->GetMeshes())
            result.push_back(mesh);
        return result;
    }

    std::shared_ptr<MeshInfo> FindSceneMesh(const std::shared_ptr<SceneGraph>& sceneGraph, const std::string& name)
    {
        if (!sceneGraph)
            return nullptr;

        for (const auto& mesh : sceneGraph->GetMeshes())
        {
            if (mesh && mesh->name == name)
                return mesh;
        }
        return nullptr;
    }

    std::shared_ptr<MeshInfo> MeshFromNode(const SceneGraphNode& node)
    {
        auto meshInstance = std::dynamic_pointer_cast<MeshInstance>(node.GetLeaf());
        return meshInstance ? meshInstance->GetMesh() : nullptr;
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
        return ValidSceneBounds(scene->GetSceneBounds());
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
        const PTTexture& texture = material.GetTexture(slot);
        if (texture.Loaded == nullptr || texture.LocalPath.empty())
            return nb::none();
        return nb::str(texture.LocalPath.generic_string().c_str());
    }

    bool SetMaterialTextureFromPython(
        PTMaterial& material,
        PTMaterialTextureSlot slot,
        const std::string& path,
        std::optional<bool> sRGB = std::nullopt,
        std::optional<bool> normalMap = std::nullopt)
    {
        if (material.RuntimeBaker == nullptr)
            throw std::runtime_error("Material is not attached to a live MaterialsBaker. Reload the scene and look up the material again.");

        return material.RuntimeBaker->SetMaterialTexture(
            material,
            slot,
            std::filesystem::path(path),
            sRGB,
            normalMap);
    }

    void ClearMaterialTextureFromPython(PTMaterial& material, PTMaterialTextureSlot slot)
    {
        if (material.RuntimeBaker == nullptr)
            throw std::runtime_error("Material is not attached to a live MaterialsBaker. Reload the scene and look up the material again.");

        material.RuntimeBaker->ClearMaterialTexture(material, slot);
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

    nb::enum_<GaussianSplatSortMode>(m, "GaussianSplatSortMode",
        "3D Gaussian Splat rasterization ordering mode.",
        nb::is_arithmetic())
        .value("GpuSort",           GaussianSplatSortMode::GpuSort)
        .value("StochasticSplats",  GaussianSplatSortMode::StochasticSplats)
        .export_values();

    nb::enum_<GaussianSplatStorageFormat>(m, "GaussianSplatStorageFormat",
        "GPU storage format for 3DGS color/SH payloads.",
        nb::is_arithmetic())
        .value("Float32", GaussianSplatStorageFormat::Float32)
        .value("Float16", GaussianSplatStorageFormat::Float16)
        .value("Uint8",   GaussianSplatStorageFormat::Uint8)
        .export_values();

    nb::enum_<GaussianSplatFrustumCulling>(m, "GaussianSplatFrustumCulling",
        "3DGS frustum culling mode.",
        nb::is_arithmetic())
        .value("Disabled",        GaussianSplatFrustumCulling::Disabled)
        .value("AtDistanceStage",  GaussianSplatFrustumCulling::AtDistanceStage)
        .value("AtRasterStage",    GaussianSplatFrustumCulling::AtRasterStage)
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
        .def_ro("name",         &PTMaterial::Name)
        .def_ro("model_name",   &PTMaterial::ModelName)
        .def_ro("unique_name",  &PTMaterial::UniqueName)

        .def_prop_rw("base_color",
            [](PTMaterial& self) { return Float3ToTuple(self.BaseOrDiffuseColor); },
            [](PTMaterial& self, nb::object v) { self.BaseOrDiffuseColor = ToFloat3(v); self.GPUDataDirty = true; },
            "Metal-rough base color or spec-gloss diffuse color (linear RGB).")
        .def_prop_rw("specular_color",
            [](PTMaterial& self) { return Float3ToTuple(self.SpecularColor); },
            [](PTMaterial& self, nb::object v) { self.SpecularColor = ToFloat3(v); self.GPUDataDirty = true; })
        .def_prop_rw("emissive_color",
            [](PTMaterial& self) { return Float3ToTuple(self.EmissiveColor); },
            [](PTMaterial& self, nb::object v) { self.EmissiveColor = ToFloat3(v); self.GPUDataDirty = true; })
        .def_prop_rw("emission_color",
            [](PTMaterial& self) { return Float3ToTuple(self.EmissiveColor); },
            [](PTMaterial& self, nb::object v) { self.EmissiveColor = ToFloat3(v); self.GPUDataDirty = true; },
            "OpenPBR-lite alias for emissive_color.")

        .def_prop_rw("emissive_intensity",
            [](PTMaterial& self) { return self.EmissiveIntensity; },
            [](PTMaterial& self, float v) { self.EmissiveIntensity = v; self.GPUDataDirty = true; })
        .def_prop_rw("emission_luminance",
            [](PTMaterial& self) { return self.EmissiveIntensity; },
            [](PTMaterial& self, float v) { self.EmissiveIntensity = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for emissive_intensity.")
        .def_prop_rw("metalness",
            [](PTMaterial& self) { return self.Metalness; },
            [](PTMaterial& self, float v) { self.Metalness = v; self.GPUDataDirty = true; })
        .def_prop_rw("base_metalness",
            [](PTMaterial& self) { return self.Metalness; },
            [](PTMaterial& self, float v) { self.Metalness = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for metalness.")
        .def_prop_rw("roughness",
            [](PTMaterial& self) { return self.Roughness; },
            [](PTMaterial& self, float v) { self.Roughness = v; self.GPUDataDirty = true; })
        .def_prop_rw("specular_roughness",
            [](PTMaterial& self) { return self.Roughness; },
            [](PTMaterial& self, float v) { self.Roughness = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for roughness.")
        .def_prop_rw("material_model",
            [](PTMaterial& self) { return self.MaterialModel; },
            [](PTMaterial& self, const std::string& v) { SetMaterialModelFromPython(self, v); })
        .def_prop_rw("base_weight",
            [](PTMaterial& self) { return self.BaseWeight; },
            [](PTMaterial& self, float v) { self.BaseWeight = v; self.GPUDataDirty = true; })
        .def_prop_rw("specular_weight",
            [](PTMaterial& self) { return self.SpecularWeight; },
            [](PTMaterial& self, float v) { self.SpecularWeight = v; self.GPUDataDirty = true; })
        .def_prop_rw("anisotropy",
            [](PTMaterial& self) { return self.Anisotropy; },
            [](PTMaterial& self, float v) { self.Anisotropy = v; self.GPUDataDirty = true; })
        .def_prop_rw("specular_roughness_anisotropy",
            [](PTMaterial& self) { return self.Anisotropy; },
            [](PTMaterial& self, float v) { self.Anisotropy = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for anisotropy.")
        .def_prop_rw("fuzz_weight",
            [](PTMaterial& self) { return self.FuzzWeight; },
            [](PTMaterial& self, float v) { self.FuzzWeight = v; self.GPUDataDirty = true; })
        .def_prop_rw("fuzz_color",
            [](PTMaterial& self) { return Float3ToTuple(self.FuzzColor); },
            [](PTMaterial& self, nb::object v) { self.FuzzColor = ToFloat3(v); self.GPUDataDirty = true; })
        .def_prop_rw("fuzz_roughness",
            [](PTMaterial& self) { return self.FuzzRoughness; },
            [](PTMaterial& self, float v) { self.FuzzRoughness = v; self.GPUDataDirty = true; })
        .def_prop_rw("opacity",
            [](PTMaterial& self) { return self.Opacity; },
            [](PTMaterial& self, float v) { self.Opacity = v; self.GPUDataDirty = true; })
        .def_prop_rw("geometry_opacity",
            [](PTMaterial& self) { return self.Opacity; },
            [](PTMaterial& self, float v) { self.Opacity = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for opacity.")
        .def_prop_rw("transmission_factor",
            [](PTMaterial& self) { return self.TransmissionFactor; },
            [](PTMaterial& self, float v) { self.TransmissionFactor = v; self.GPUDataDirty = true; })
        .def_prop_rw("transmission_weight",
            [](PTMaterial& self) { return self.TransmissionFactor; },
            [](PTMaterial& self, float v) { SetOpenPBRTransmissionWeightFromPython(self, v); },
            "OpenPBR-lite alias for transmission_factor; toggles enable_transmission from the weight.")
        .def_prop_rw("diffuse_transmission_factor",
            [](PTMaterial& self) { return self.DiffuseTransmissionFactor; },
            [](PTMaterial& self, float v) { self.DiffuseTransmissionFactor = v; self.GPUDataDirty = true; })
        .def_prop_rw("transmission_diffuse_weight",
            [](PTMaterial& self) { return self.DiffuseTransmissionFactor; },
            [](PTMaterial& self, float v) { SetOpenPBRDiffuseTransmissionWeightFromPython(self, v); },
            "OpenPBR-lite alias for diffuse_transmission_factor; toggles enable_transmission from the weight.")
        .def_prop_rw("normal_texture_scale",
            [](PTMaterial& self) { return self.NormalTextureScale; },
            [](PTMaterial& self, float v) { self.NormalTextureScale = v; self.GPUDataDirty = true; })
        .def_prop_rw("geometry_normal_scale",
            [](PTMaterial& self) { return self.NormalTextureScale; },
            [](PTMaterial& self, float v) { self.NormalTextureScale = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for normal_texture_scale.")
        .def_prop_rw("ior",
            [](PTMaterial& self) { return self.IoR; },
            [](PTMaterial& self, float v) { self.IoR = v; self.GPUDataDirty = true; })
        .def_prop_rw("specular_ior",
            [](PTMaterial& self) { return self.IoR; },
            [](PTMaterial& self, float v) { self.IoR = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for ior.")
        .def_prop_rw("alpha_cutoff",
            [](PTMaterial& self) { return self.AlphaCutoff; },
            [](PTMaterial& self, float v) { self.AlphaCutoff = v; self.GPUDataDirty = true; })
        .def_prop_rw("geometry_alpha_cutoff",
            [](PTMaterial& self) { return self.AlphaCutoff; },
            [](PTMaterial& self, float v) { self.AlphaCutoff = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for alpha_cutoff.")

        .def_prop_rw("volume_attenuation_distance",
            [](PTMaterial& self) { return self.VolumeAttenuationDistance; },
            [](PTMaterial& self, float v) { self.VolumeAttenuationDistance = v; self.GPUDataDirty = true; })
        .def_prop_rw("volume_attenuation_color",
            [](PTMaterial& self) { return Float3ToTuple(self.VolumeAttenuationColor); },
            [](PTMaterial& self, nb::object v) { self.VolumeAttenuationColor = ToFloat3(v); self.GPUDataDirty = true; })
        .def_prop_rw("nested_priority",
            [](PTMaterial& self) { return self.NestedPriority; },
            [](PTMaterial& self, int v) { self.NestedPriority = v; self.GPUDataDirty = true; })

        .def_prop_rw("use_specular_gloss",
            [](PTMaterial& self) { return self.UseSpecularGlossModel; },
            [](PTMaterial& self, bool v) { self.UseSpecularGlossModel = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_alpha_testing",
            [](PTMaterial& self) { return self.EnableAlphaTesting; },
            [](PTMaterial& self, bool v) { self.EnableAlphaTesting = v; self.GPUDataDirty = true; })
        .def_prop_rw("geometry_enable_alpha_test",
            [](PTMaterial& self) { return self.EnableAlphaTesting; },
            [](PTMaterial& self, bool v) { self.EnableAlphaTesting = v; self.GPUDataDirty = true; },
            "OpenPBR-lite UI alias for enable_alpha_testing.")
        .def_prop_rw("enable_transmission",
            [](PTMaterial& self) { return self.EnableTransmission; },
            [](PTMaterial& self, bool v) { self.EnableTransmission = v; self.GPUDataDirty = true; })
        .def_prop_rw("thin_surface",
            [](PTMaterial& self) { return self.ThinSurface; },
            [](PTMaterial& self, bool v) { self.ThinSurface = v; self.GPUDataDirty = true; })
        .def_prop_rw("geometry_thin_walled",
            [](PTMaterial& self) { return self.ThinSurface; },
            [](PTMaterial& self, bool v) { self.ThinSurface = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for thin_surface.")
        .def_prop_rw("exclude_from_nee",
            [](PTMaterial& self) { return self.ExcludeFromNEE; },
            [](PTMaterial& self, bool v) { self.ExcludeFromNEE = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_as_analytic_light_proxy",
            [](PTMaterial& self) { return self.EnableAsAnalyticLightProxy; },
            [](PTMaterial& self, bool v) { self.EnableAsAnalyticLightProxy = v; self.GPUDataDirty = true; })
        .def_prop_rw("skip_render",
            [](PTMaterial& self) { return self.SkipRender; },
            [](PTMaterial& self, bool v) { self.SkipRender = v; self.GPUDataDirty = true; })
        .def_prop_rw("metalness_in_red_channel",
            [](PTMaterial& self) { return self.MetalnessInRedChannel; },
            [](PTMaterial& self, bool v) { self.MetalnessInRedChannel = v; self.GPUDataDirty = true; })

        .def_prop_rw("enable_base_texture",
            [](PTMaterial& self) { return self.EnableBaseTexture; },
            [](PTMaterial& self, bool v) { self.EnableBaseTexture = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_base_color_texture",
            [](PTMaterial& self) { return self.EnableBaseTexture; },
            [](PTMaterial& self, bool v) { self.EnableBaseTexture = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for enable_base_texture.")
        .def_prop_rw("enable_orm_texture",
            [](PTMaterial& self) { return self.EnableOcclusionRoughnessMetallicTexture; },
            [](PTMaterial& self, bool v) { self.EnableOcclusionRoughnessMetallicTexture = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_base_metalness_specular_roughness_texture",
            [](PTMaterial& self) { return self.EnableOcclusionRoughnessMetallicTexture; },
            [](PTMaterial& self, bool v) { self.EnableOcclusionRoughnessMetallicTexture = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for enable_orm_texture.")
        .def_prop_rw("enable_normal_texture",
            [](PTMaterial& self) { return self.EnableNormalTexture; },
            [](PTMaterial& self, bool v) { self.EnableNormalTexture = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_geometry_normal_texture",
            [](PTMaterial& self) { return self.EnableNormalTexture; },
            [](PTMaterial& self, bool v) { self.EnableNormalTexture = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for enable_normal_texture.")
        .def_prop_rw("enable_emissive_texture",
            [](PTMaterial& self) { return self.EnableEmissiveTexture; },
            [](PTMaterial& self, bool v) { self.EnableEmissiveTexture = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_emission_color_texture",
            [](PTMaterial& self) { return self.EnableEmissiveTexture; },
            [](PTMaterial& self, bool v) { self.EnableEmissiveTexture = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for enable_emissive_texture.")
        .def_prop_rw("enable_transmission_texture",
            [](PTMaterial& self) { return self.EnableTransmissionTexture; },
            [](PTMaterial& self, bool v) { self.EnableTransmissionTexture = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_transmission_weight_texture",
            [](PTMaterial& self) { return self.EnableTransmissionTexture; },
            [](PTMaterial& self, bool v) { self.EnableTransmissionTexture = v; self.GPUDataDirty = true; },
            "OpenPBR-lite alias for enable_transmission_texture.")

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

        .def("mark_dirty", [](PTMaterial& self) { self.GPUDataDirty = true; },
             "Force this material's GPU buffer slot to be refreshed next frame.")
        .def("__repr__", [](const PTMaterial& self) {
                return std::string("<caustica.Material '") + self.Name + "'>";
            });

    // --- Lights -----------------------------------------------------------
    nb::class_<Light>(m, "Light", "Base class for all scene lights.")
        .def_prop_ro("light_type", [](Light& self) { return self.GetLightType(); })
        .def_prop_rw("color",
            [](Light& self) { return Float3ToTuple(self.color); },
            [](Light& self, nb::object v) { self.color = ToFloat3(v); })
        .def_prop_ro("name", [](Light& self) -> std::string {
                return self.GetNode() ? self.GetNode()->GetName() : std::string{};
            })
        .def_prop_rw("position",
            [](Light& self) { return Double3ToTuple(self.GetPosition()); },
            [](Light& self, nb::object v) { self.SetPosition(ToDouble3(v)); })
        .def_prop_rw("direction",
            [](Light& self) { return Double3ToTuple(self.GetDirection()); },
            [](Light& self, nb::object v) { self.SetDirection(ToDouble3(v)); })
        .def("__repr__", [](Light& self) {
                std::string n = self.GetNode() ? self.GetNode()->GetName() : "<unnamed>";
                return std::string("<caustica.Light '") + n + "'>";
            });

    nb::class_<DirectionalLight, Light>(m, "DirectionalLight",
        "Distant directional light source (sun-style).")
        .def_rw("irradiance", &DirectionalLight::irradiance,
                "Target illuminance (lm/m^2) - multiplied by `color`.")
        .def_rw("angular_size", &DirectionalLight::angularSize,
                "Apparent angular size of the light source, in degrees.");

    nb::class_<SpotLight, Light>(m, "SpotLight", "Spot light with inner / outer cones.")
        .def_rw("intensity", &SpotLight::intensity)
        .def_rw("radius", &SpotLight::radius)
        .def_rw("range", &SpotLight::range)
        .def_rw("inner_angle", &SpotLight::innerAngle)
        .def_rw("outer_angle", &SpotLight::outerAngle);

    nb::class_<PointLight, Light>(m, "PointLight", "Omnidirectional point light.")
        .def_rw("intensity", &PointLight::intensity)
        .def_rw("radius", &PointLight::radius)
        .def_rw("range", &PointLight::range);

    nb::class_<EnvironmentLight, Light>(m, "EnvironmentLight",
        "RTXPT environment map / IBL light.")
        .def_prop_rw("radiance_scale",
            [](EnvironmentLight& self) { return Float3ToTuple(self.radianceScale); },
            [](EnvironmentLight& self, nb::object v) { self.radianceScale = ToFloat3(v); })
        .def_rw("rotation", &EnvironmentLight::rotation)
        .def_rw("path", &EnvironmentLight::path);

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

    nb::class_<SceneGraphNode>(m, "SceneNode",
        "Scene graph node wrapper for runtime mesh/light/camera transforms.")
        .def_prop_ro("name", [](SceneGraphNode& self) { return self.GetName(); })
        .def_prop_ro("path", [](SceneGraphNode& self) { return self.GetPath().generic_string(); })
        .def_prop_ro("mesh", [](SceneGraphNode& self) {
                return MeshFromNode(self);
            }, "Mesh attached to this node, or None when the node is not a mesh instance.")
        .def_prop_ro("is_mesh", [](SceneGraphNode& self) {
                return MeshFromNode(self) != nullptr;
            })
        .def_prop_rw("translation",
            [](SceneGraphNode& self) { return Double3ToTuple(self.GetTranslation()); },
            [](SceneGraphNode& self, nb::object v) { self.SetTranslation(ToDouble3(v)); },
            "Local translation in scene space.")
        .def_prop_rw("rotation",
            [](SceneGraphNode& self) { return DQuatToXYZWTuple(self.GetRotation()); },
            [](SceneGraphNode& self, nb::object v) { self.SetRotation(ToDQuatXYZW(v)); },
            "Local rotation quaternion as `(x, y, z, w)`.")
        .def_prop_rw("euler",
            [](SceneGraphNode& self) { return Double3ToTuple(DQuatToEulerRadiansXYZ(self.GetRotation())); },
            [](SceneGraphNode& self, nb::object v) { self.SetRotation(caustica::math::rotationQuat(ToDouble3(v))); },
            "Local XYZ Euler rotation in radians. Assigning this updates the node rotation quaternion.")
        .def_prop_rw("scaling",
            [](SceneGraphNode& self) { return Double3ToTuple(self.GetScaling()); },
            [](SceneGraphNode& self, nb::object v) { self.SetScaling(ToDouble3(v)); },
            "Local non-uniform scaling.")
        .def_prop_ro("bounds",
            [](SceneGraphNode& self) -> nb::object {
                return SceneBoundsTuple(ValidSceneBounds(self.GetGlobalBoundingBox()));
            },
            "World-space `((min.xyz), (max.xyz))` AABB for this node's subgraph.")
        .def("__repr__", [](SceneGraphNode& self) {
                return std::string("<caustica.SceneNode '") + self.GetName() + "'>";
            });

    // --- Scene ------------------------------------------------------------
    nb::class_<Scene>(m, "Scene",
        "Loaded caustica scene. Material and light access lives here so Python\n"
        "scripts can follow the same shape as the C++ Sample::GetScene() path.")
        .def("get_materials", [](Scene& self) {
                return GetSceneMaterials(self.GetSceneGraph());
            }, "Return every PTMaterial in this scene.")

        .def("find_material", [](Scene& self, const std::string& name) {
                return FindSceneMaterial(self.GetSceneGraph(), name);
            }, nb::arg("name"), "Look up a material by Name or UniqueName.")

        .def("find_material_by_id", [](Scene& self, int materialId) {
                return FindSceneMaterialById(self.GetSceneGraph(), materialId);
            }, nb::arg("material_id"), "Look up a material by engine material ID.")

        .def("get_lights", [](Scene& self) {
                return GetSceneLights(self.GetSceneGraph());
            }, "Return every Light in this scene.")

        .def("find_light", [](Scene& self, const std::string& name) {
                return FindSceneLight(self.GetSceneGraph(), name);
            }, nb::arg("name"), "Look up a light by scene node name.")
        .def("find_node", [](Scene& self, const std::string& path) {
                return FindSceneNode(self.GetSceneGraph(), path);
            }, nb::arg("path"), "Look up a scene graph node by name or path.")
        .def("get_meshes", [](Scene& self) {
                return GetSceneMeshes(self.GetSceneGraph());
            }, "Return every Mesh in this scene.")
        .def("find_mesh", [](Scene& self, const std::string& name) {
                return FindSceneMesh(self.GetSceneGraph(), name);
            }, nb::arg("name"), "Look up a mesh by mesh name.")

        .def_prop_ro("material_count", [](Scene& self) {
                auto sceneGraph = self.GetSceneGraph();
                return sceneGraph ? GetSceneMaterials(sceneGraph).size() : size_t(0);
            }, "Number of PTMaterial instances in this scene.")
        .def_prop_ro("mesh_count", [](Scene& self) {
                auto sceneGraph = self.GetSceneGraph();
                return sceneGraph ? sceneGraph->GetMeshes().size() : size_t(0);
            }, "Number of meshes in this scene.")
        .def_prop_ro("light_count", [](Scene& self) {
                auto sceneGraph = self.GetSceneGraph();
                return sceneGraph ? sceneGraph->GetLights().size() : size_t(0);
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
            "leaf in the scene graph (mesh instances, lights, splats, ...).\n"
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
                auto sceneGraph = self.GetSceneGraph();
                const auto materialCount = sceneGraph ? GetSceneMaterials(sceneGraph).size() : size_t(0);
                const auto lightCount = sceneGraph ? sceneGraph->GetLights().size() : size_t(0);
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
        .def_rw("enabled", &EnvironmentMapRuntimeParameters::Enabled)
        .def_rw("visible_to_camera", &EnvironmentMapRuntimeParameters::VisibleToCamera)
        .def_prop_rw("hide_source",
            [](EnvironmentMapRuntimeParameters& s) { return !s.VisibleToCamera; },
            [](EnvironmentMapRuntimeParameters& s, bool hide) { s.VisibleToCamera = !hide; });

    nb::class_<RenderSessionState>(m, "Settings",
        "Live renderer session state (path tracer settings and runtime flags).\n"
        "Mutating attributes is equivalent to moving the corresponding ImGui widget.")
        .def_rw("enable_animations",             &RenderSessionState::EnableAnimations)
        .def_rw("enable_vsync",                  &RenderSessionState::EnableVsync)
        .def_rw("fps_limiter",                   &RenderSessionState::FPSLimiter)

        // --- Path tracer top-level mode ----------------------------------
        .def_rw("realtime_mode",                 &RenderSessionState::RealtimeMode,
                "True for realtime mode, False for reference / accumulation mode.\n"
                "See `Settings.path_tracer_mode` for an enum-flavored version.")
        .def_prop_rw("path_tracer_mode",
            [](RenderSessionState& s) -> int { return s.RealtimeMode ? 0 /*Realtime*/ : 1 /*Reference*/; },
            [](RenderSessionState& s, int mode) {
                bool wasRealtime = s.RealtimeMode;
                s.RealtimeMode = (mode == 0);
                if (wasRealtime != s.RealtimeMode)
                    s.ResetAccumulation = true;
            },
            "Convenience wrapper around `realtime_mode`.\n"
            "Set to caustica.PathTracerMode.Reference or .Realtime.")

        .def_rw("realtime_samples_per_pixel",    &RenderSessionState::RealtimeSamplesPerPixel)
        .def_rw("accumulation_target",           &RenderSessionState::AccumulationTarget)
        .def_rw("reset_accumulation",            &RenderSessionState::ResetAccumulation)
        .def_rw("reset_realtime_caches",         &RenderSessionState::ResetRealtimeCaches)
        .def_rw("accumulation_aa",               &RenderSessionState::AccumulationAA)
        .def_rw("accumulation_prewarm_realtime_caches", &RenderSessionState::AccumulationPreWarmRealtimeCaches)

        .def_rw("bounce_count",                  &RenderSessionState::BounceCount)
        .def_rw("diffuse_bounce_count",          &RenderSessionState::DiffuseBounceCount)
        .def_rw("enable_russian_roulette",       &RenderSessionState::EnableRussianRoulette)
        .def_rw("texture_lod_bias",              &RenderSessionState::TexLODBias)

        .def_rw("use_nee",                       &RenderSessionState::UseNEE)
        .def_rw("nee_type",                      &RenderSessionState::NEEType,
                "0 = uniform, 1 = power-based, 2 = NEE-AT")
        .def_rw("nee_candidate_samples",         &RenderSessionState::NEECandidateSamples)
        .def_rw("nee_full_samples",              &RenderSessionState::NEEFullSamples)
        .def_rw("nee_mis_type",                  &RenderSessionState::NEEMISType)

        .def_rw("use_restir_di",                 &RenderSessionState::UseReSTIRDI)
        .def_rw("use_restir_gi",                 &RenderSessionState::UseReSTIRGI)
        .def_rw("use_restir_pt",                 &RenderSessionState::UseReSTIRPT)

        .def_rw("camera_aperture",               &RenderSessionState::CameraAperture)
        .def_rw("camera_focal_distance",         &RenderSessionState::CameraFocalDistance)
        .def_rw("camera_move_speed",             &RenderSessionState::CameraMoveSpeed)

        .def_rw("realtime_firefly_filter_enabled", &RenderSessionState::RealtimeFireflyFilterEnabled)
        .def_rw("realtime_firefly_filter_threshold", &RenderSessionState::RealtimeFireflyFilterThreshold)
        .def_rw("reference_firefly_filter_enabled",  &RenderSessionState::ReferenceFireflyFilterEnabled)
        .def_rw("reference_firefly_filter_threshold",&RenderSessionState::ReferenceFireflyFilterThreshold)

        .def_rw("enable_tone_mapping",           &RenderSessionState::EnableToneMapping)
        .def_rw("enable_bloom",                  &RenderSessionState::EnableBloom)
        .def_rw("bloom_intensity",               &RenderSessionState::BloomIntensity)
        .def_rw("bloom_radius",                  &RenderSessionState::BloomRadius)

        .def_rw("enable_gaussian_splats",        &RenderSessionState::EnableGaussianSplats)
        .def_rw("gaussian_splat_depth_test",     &RenderSessionState::GaussianSplatDepthTest)
        .def_rw("gaussian_splat_shadows",        &RenderSessionState::GaussianSplatShadows)
        .def_rw("gaussian_splat_hybrid_shadows", &RenderSessionState::GaussianSplatShadows)
        .def_rw("gaussian_splat_shadows_mode",   &RenderSessionState::GaussianSplatShadowsMode,
                "3DGS shadow mode (caustica.GaussianSplatShadowMode).")
        .def_rw("gaussian_splat_sorting_mode",   &RenderSessionState::GaussianSplatSortingMode,
                "3DGS sort mode (caustica.GaussianSplatSortMode).")
        .def_rw("gaussian_splat_sh_format",      &RenderSessionState::GaussianSplatSHFormat,
                "3DGS SH storage format (caustica.GaussianSplatStorageFormat).")
        .def_rw("gaussian_splat_rgba_format",    &RenderSessionState::GaussianSplatRGBAFormat,
                "3DGS RGBA storage format (caustica.GaussianSplatStorageFormat).")
        .def_rw("gaussian_splat_use_aabbs",      &RenderSessionState::GaussianSplatUseAABBs)
        .def_rw("gaussian_splat_use_tlas_instances", &RenderSessionState::GaussianSplatUseTLASInstances)
        .def_rw("gaussian_splat_blas_compaction", &RenderSessionState::GaussianSplatBlasCompaction)
        .def_rw("gaussian_splat_rtx_kernel_degree", &RenderSessionState::GaussianSplatRtxKernelDegree)
        .def_rw("gaussian_splat_rtx_adaptive_clamp", &RenderSessionState::GaussianSplatRtxAdaptiveClamp)
        .def_rw("gaussian_splat_rtx_alpha_clamp", &RenderSessionState::GaussianSplatRtxAlphaClamp)
        .def_rw("gaussian_splat_rtx_minimum_transmittance", &RenderSessionState::GaussianSplatRtxMinimumTransmittance)
        .def_rw("gaussian_splat_rtx_trace_strategy", &RenderSessionState::GaussianSplatRtxTraceStrategy)
        .def_rw("gaussian_splat_rtx_particle_samples_per_pass", &RenderSessionState::GaussianSplatRtxParticleSamplesPerPass)
        .def_rw("gaussian_splat_rtx_maximum_pass_count", &RenderSessionState::GaussianSplatRtxMaximumPassCount)
        .def_rw("gaussian_splat_rtx_particle_shadow_offset", &RenderSessionState::GaussianSplatRtxParticleShadowOffset)
        .def_rw("gaussian_splat_rtx_particle_shadow_threshold", &RenderSessionState::GaussianSplatRtxParticleShadowThreshold)
        .def_rw("gaussian_splat_rtx_colored_shadow_strength", &RenderSessionState::GaussianSplatRtxColoredShadowStrength)
        .def_rw("gaussian_splat_rtx_mesh_composite_threshold", &RenderSessionState::GaussianSplatRtxMeshCompositeThreshold)
        .def_rw("gaussian_splat_rtx_depth_iso_threshold", &RenderSessionState::GaussianSplatRtxDepthIsoThreshold)
        .def_rw("gaussian_splat_mip_antialiasing", &RenderSessionState::GaussianSplatMipAntialiasing)
        .def_rw("gaussian_splat_quantize_normals", &RenderSessionState::GaussianSplatQuantizeNormals)
        .def_rw("gaussian_splat_ftb_sync_mode", &RenderSessionState::GaussianSplatFTBSyncMode,
                "3DGS front-to-back synchronization mode (caustica.GaussianSplatFTBSyncMode).")
        .def_rw("gaussian_splat_depth_iso_threshold", &RenderSessionState::GaussianSplatDepthIsoThreshold)
        .def_rw("gaussian_splat_fragment_shader_barycentric", &RenderSessionState::GaussianSplatFragmentShaderBarycentric)
        .def_rw("gaussian_splat_frustum_culling", &RenderSessionState::GaussianSplatFrustumCulling,
                "3DGS frustum culling mode (caustica.GaussianSplatFrustumCulling).")
        .def_rw("gaussian_splat_frustum_dilation", &RenderSessionState::GaussianSplatFrustumDilation)
        .def_rw("gaussian_splat_screen_size_culling", &RenderSessionState::GaussianSplatScreenSizeCulling)
        .def_rw("gaussian_splat_min_pixel_coverage", &RenderSessionState::GaussianSplatMinPixelCoverage)
        .def_rw("gaussian_splat_scale",          &RenderSessionState::GaussianSplatScale)
        .def_rw("gaussian_splat_alpha_scale",    &RenderSessionState::GaussianSplatAlphaScale)
        .def_rw("gaussian_splat_brightness",     &RenderSessionState::GaussianSplatBrightness)
        .def_prop_rw("gaussian_splat_tint_color",
            [](RenderSessionState& s) { return Float3ToTuple(s.GaussianSplatTintColor); },
            [](RenderSessionState& s, nb::object v) { s.GaussianSplatTintColor = ToFloat3(v); })
        .def_rw("gaussian_splat_as_emitter",     &RenderSessionState::GaussianSplatAsEmitter)
        .def_rw("gaussian_splat_emission_intensity", &RenderSessionState::GaussianSplatEmissionIntensity)
        .def_rw("gaussian_splat_emission_max_proxy_count", &RenderSessionState::GaussianSplatEmissionMaxProxyCount)
        .def_rw("gaussian_splat_alpha_cull_threshold", &RenderSessionState::GaussianSplatAlphaCullThreshold)
        .def_rw("gaussian_splat_shadow_strength", &RenderSessionState::GaussianSplatShadowStrength)
        .def_rw("gaussian_splat_shadow_soft_radius", &RenderSessionState::GaussianSplatShadowSoftRadius)
        .def_rw("gaussian_splat_shadow_soft_sample_count", &RenderSessionState::GaussianSplatShadowSoftSampleCount)
        .def_prop_rw("gaussian_splat_translation",
            [](RenderSessionState& s) { return Float3ToTuple(s.GaussianSplatTranslation); },
            [](RenderSessionState& s, nb::object v) {
                s.GaussianSplatTranslation = ToFloat3(v);
                s.ResetAccumulation = true;
            })
        .def_prop_rw("gaussian_splat_rotation_euler_deg",
            [](RenderSessionState& s) { return Float3ToTuple(s.GaussianSplatRotationEulerDeg); },
            [](RenderSessionState& s, nb::object v) {
                s.GaussianSplatRotationEulerDeg = ToFloat3(v);
                s.ResetAccumulation = true;
            })
        .def_prop_rw("gaussian_splat_object_scale",
            [](RenderSessionState& s) { return Float3ToTuple(s.GaussianSplatObjectScale); },
            [](RenderSessionState& s, nb::object v) {
                s.GaussianSplatObjectScale = ToFloat3(v);
                s.ResetAccumulation = true;
            })
        .def_ro("gaussian_splat_count", [](const RenderSessionState& s) { return s.GaussianSplats.SplatCount; })
        .def_ro("gaussian_splat_object_count", [](const RenderSessionState& s) { return s.GaussianSplats.ObjectCount; })
        .def_ro("gaussian_splat_file_name", [](const RenderSessionState& s) { return s.GaussianSplats.FileName; })

        // --- AA / DLSS / DLSS-RR / DLSS-G / Reflex (realtime only) -------
        .def_rw("realtime_aa",                   &RenderSessionState::RealtimeAA,
                "Realtime AA mode (caustica.RealtimeAA enum):\n"
                "  0 = Off, 1 = TAA, 2 = DLSS, 3 = DLSS-RR")

        // DLSS quality (caustica.DLSSMode enum -> SI::DLSSMode underlying uint32)
        .def_prop_rw("dlss_mode",
            [](RenderSessionState& s) { return int(s.DLSSMode); },
            [](RenderSessionState& s, int v) { s.DLSSMode = SI::DLSSMode(v); },
            "DLSS quality preset (caustica.DLSSMode).\n"
            "Off, MaxPerformance, Balanced, MaxQuality, UltraPerformance, UltraQuality, DLAA.")
        .def_rw("dlss_lod_bias_use_override", &RenderSessionState::DLSSLodBiasUseOverride)
        .def_rw("dlss_lod_bias_override",     &RenderSessionState::DLSSLodBiasOverride)
        .def_rw("dlss_always_use_extents",    &RenderSessionState::DLSSAlwaysUseExtents)

        // DLSS-G (frame generation)
        .def_prop_rw("dlss_fg_mode",
            [](RenderSessionState& s) { return int(s.DLSSFGMode); },
            [](RenderSessionState& s, int v) { s.DLSSFGMode = SI::DLSSGMode(v); },
            "DLSS frame generation mode (caustica.DLSSFGMode).")
        .def_rw("dlss_fg_multiplier",            &RenderSessionState::DLSSFGMultiplier)
        .def_rw("dlss_fg_num_frames_to_generate",&RenderSessionState::DLSSFGNumFramesToGenerate)
        .def_rw("dlss_fg_max_num_frames_to_generate",&RenderSessionState::DLSSFGMaxNumFramesToGenerate)

        // DLSS-RR (ray reconstruction)
        .def_prop_rw("dlss_rr_preset",
            [](RenderSessionState& s) { return int(s.DLSRRPreset); },
            [](RenderSessionState& s, int v) { s.DLSRRPreset = SI::DLSSRRPreset(v); },
            "DLSS-RR preset (caustica.DLSSRRPreset).")
        .def_rw("dlss_rr_micro_jitter",          &RenderSessionState::DLSSRRMicroJitter)
        .def_rw("dlss_rr_brightness_clamp_k",    &RenderSessionState::DLSSRRBrightnessClampK)
        .def_rw("disable_restirs_with_dlss_rr",  &RenderSessionState::DisableReSTIRsWithDLSSRR)

        // Reflex (low latency)
        .def_rw("reflex_mode",                   &RenderSessionState::ReflexMode,
                "NVIDIA Reflex mode (caustica.ReflexMode).")
        .def_rw("reflex_capped_fps",             &RenderSessionState::ReflexCappedFps)

        // --- Read-only support flags -------------------------------------
        .def_ro("is_dlss_supported",     &RenderSessionState::IsDLSSSuported)
        .def_ro("is_dlss_fg_supported",  &RenderSessionState::IsDLSSFGSupported)
        .def_ro("is_dlss_rr_supported",  &RenderSessionState::IsDLSSRRSupported)
        .def_ro("is_reflex_supported",   &RenderSessionState::IsReflexSupported)

        // --- Standalone NRD denoiser (realtime, RealtimeAA != DLSS-RR) ---
        .def_rw("standalone_denoiser",           &RenderSessionState::StandaloneDenoiser,
                "Enable NRD denoiser in realtime mode (no effect with DLSS-RR).")
        .def_rw("denoiser_radiance_clamp_k",     &RenderSessionState::DenoiserRadianceClampK)

        // --- OIDN reference-mode denoiser --------------------------------
        .def_rw("oidn_enabled",            &RenderSessionState::ReferenceOIDNDenoiser,
                "(Reference mode) Run Intel Open Image Denoise after accumulation reaches the SPP target.")
        .def_rw("oidn_use_gpu",            &RenderSessionState::ReferenceOIDNUseGPU,
                "Use OIDN GPU device (CUDA/HIP/SYCL) when available, else CPU.")
        .def_rw("oidn_passes",             &RenderSessionState::ReferenceOIDNPasses,
                "Auxiliary guide passes (caustica.OidnPasses).")
        .def_rw("oidn_prefilter",          &RenderSessionState::ReferenceOIDNPrefilter,
                "Prefilter quality for guide passes (caustica.OidnPrefilter).")
        .def_rw("oidn_quality",            &RenderSessionState::ReferenceOIDNQuality,
                "Beauty filter quality (caustica.OidnQuality).")
        .def_rw("oidn_changed",            &RenderSessionState::ReferenceOIDNDenoiserChanged,
                "Set to True after editing any OIDN parameter to force a redenoise.\n"
                "Cleared automatically by the renderer.")
        .def("oidn_apply", [](RenderSessionState& s) { s.ReferenceOIDNDenoiserChanged = true; },
             "Mark OIDN parameters dirty so the next accumulation completion runs the filter again.")

        .def_rw("environment_map",               &RenderSessionState::EnvironmentMapParams,
                nb::rv_policy::reference_internal,
                "EnvironmentMapParams structure (intensity, tint, rotation, enabled, visible_to_camera).")
        ;

    nb::class_<SampleUIData, RenderSessionState>(m, "EditorSettings",
        "Desktop-editor settings that extend `Settings` with ImGui view state.")
        .def_rw("show_ui", &SampleUIData::ShowUI);

    // --- Sample (top-level renderer access) -------------------------------
    nb::class_<SceneEditor>(m, "Sample",
        "caustica renderer instance. In embed mode use caustica.app(); in extension\n"
        "mode use Renderer.app to retrieve the underlying instance.")
        .def_prop_ro("settings", [](SceneEditor& self) -> RenderSessionState* {
                return &self.GetRenderSessionState();
            }, nb::rv_policy::reference,
            "Live `Settings` mirror of the current UI state.")
        .def_prop_ro("scene", [](SceneEditor& self) {
                return self.GetScene();
            }, "Current loaded `Scene`, or None before a scene is available.")

        .def_prop_ro("scene_name",  [](SceneEditor& self) { return self.GetCurrentSceneName(); })
        .def_prop_ro("available_scenes", [](SceneEditor& self) { return self.GetAvailableScenes(); })

        .def("get_scene", [](SceneEditor& self) {
                return self.GetScene();
            }, "Return the current loaded Scene, matching the C++ GetScene() entry point.")

        .def("set_scene", [](SceneEditor& self, const std::string& name, bool forceReload)
            {
                self.SetCurrentScene(name, forceReload);
            },
            nb::arg("scene_name"), nb::arg("force_reload") = false,
            "Switch to a different scene file from caustica.Sample.available_scenes.")

        .def("load_gaussian_splats", [](SceneEditor& self, const std::string& fileName, bool convertRdfToRub)
            {
                return self.LoadGaussianSplatFile(fileName, convertRdfToRub);
            },
            nb::arg("file_name"), nb::arg("convert_rdf_to_rub") = true,
            "Load a 3DGS .ply file and rasterize it over the current scene.")

        .def("load_mesh_file", [](SceneEditor& self, const std::string& fileName)
            {
                return self.LoadMeshFile(fileName);
            },
            nb::arg("file_name"),
            "Append a mesh file (.gltf, .glb, or .obj) to the current scene.")

        .def_prop_ro("gaussian_splat_count", [](SceneEditor& self) { return self.GetGaussianSplatCount(); })
        .def_prop_ro("gaussian_splat_object_count", [](SceneEditor& self) { return self.GetGaussianSplatObjectCount(); })
        .def_prop_ro("gaussian_splat_file_name", [](SceneEditor& self) { return self.GetGaussianSplatFileName(); })

        .def("get_materials", [](SceneEditor& self) {
                return GetSceneMaterials(SceneGraphFromScene(self.GetScene()));
            }, "Compatibility alias for `sample.scene.get_materials()`.")

        .def("find_material", [](SceneEditor& self, const std::string& name) -> std::shared_ptr<PTMaterial> {
                return FindSceneMaterial(SceneGraphFromScene(self.GetScene()), name);
            }, nb::arg("name"), "Compatibility alias for `sample.scene.find_material(name)`.")

        .def("find_material_by_id", [](SceneEditor& self, int materialId) -> std::shared_ptr<PTMaterial> {
                return FindSceneMaterialById(SceneGraphFromScene(self.GetScene()), materialId);
            }, nb::arg("material_id"), "Compatibility alias for `sample.scene.find_material_by_id(material_id)`.")

        .def("get_lights", [](SceneEditor& self) {
                return GetSceneLights(SceneGraphFromScene(self.GetScene()));
            }, "Compatibility alias for `sample.scene.get_lights()`.")

        .def("get_scene_bounds", [](SceneEditor& self) {
                return SceneBoundsTuple(SceneBoundsFromScene(self.GetScene()));
            },
            "Compatibility alias for `sample.scene.get_scene_bounds()`.")

        .def_prop_ro("scene_bounds", [](SceneEditor& self) {
                return SceneBoundsTuple(SceneBoundsFromScene(self.GetScene()));
            },
            "Shortcut for `sample.scene.bounds`. Returns the world-space\n"
            "((min.xyz), (max.xyz)) AABB or `None` if no scene is loaded.")
        .def_prop_ro("scene_bounds_center", [](SceneEditor& self) {
                return SceneBoundsCenter(SceneBoundsFromScene(self.GetScene()));
            }, "Shortcut for `sample.scene.bounds_center` (or `None`).")
        .def_prop_ro("scene_bounds_size", [](SceneEditor& self) {
                return SceneBoundsSize(SceneBoundsFromScene(self.GetScene()));
            }, "Shortcut for `sample.scene.bounds_size` (or `None`).")

        .def("find_light", [](SceneEditor& self, const std::string& name) -> std::shared_ptr<Light> {
                return FindSceneLight(SceneGraphFromScene(self.GetScene()), name);
            }, nb::arg("name"), "Compatibility alias for `sample.scene.find_light(name)`.")
        .def("find_node", [](SceneEditor& self, const std::string& path) -> std::shared_ptr<SceneGraphNode> {
                return FindSceneNode(SceneGraphFromScene(self.GetScene()), path);
            }, nb::arg("path"), "Compatibility alias for `sample.scene.find_node(path)`.")

        .def("get_meshes", [](SceneEditor& self) {
                return GetSceneMeshes(SceneGraphFromScene(self.GetScene()));
            }, "Compatibility alias for `sample.scene.get_meshes()`.")
        .def("find_mesh", [](SceneEditor& self, const std::string& name) -> std::shared_ptr<MeshInfo> {
                return FindSceneMesh(SceneGraphFromScene(self.GetScene()), name);
            }, nb::arg("name"), "Compatibility alias for `sample.scene.find_mesh(name)`.")
        .def("get_mesh_vertices", [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh) {
                return Float3VectorToList(self.GetMeshVertices(mesh));
            }, nb::arg("mesh"),
            "Return unique mesh positions as a list of (x, y, z) tuples in object space.")
        .def("set_mesh_vertices",
            [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh, nb::object vertices,
               bool recomputeNormals, bool rebuildAccelerationStructure) {
                self.SetMeshVertices(mesh, ToFloat3Vector(vertices), recomputeNormals, rebuildAccelerationStructure);
            },
            nb::arg("mesh"), nb::arg("vertices"), nb::arg("recompute_normals") = true,
            nb::arg("rebuild_acceleration_structure") = true,
            "Replace all unique object-space positions for a mesh and refresh GPU buffers.\n"
            "Updates are propagated to all render vertices split by normals or UVs.")
        .def("deform_mesh",
            [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh, nb::object callback,
               bool recomputeNormals, bool rebuildAccelerationStructure) {
                std::vector<float3> vertices = self.GetMeshVertices(mesh);
                for (size_t i = 0; i < vertices.size(); ++i)
                {
                    nb::object updated = callback(i, Float3ToTuple(vertices[i]));
                    if (!updated.is_none())
                        vertices[i] = ToFloat3(updated);
                }
                self.SetMeshVertices(mesh, vertices, recomputeNormals, rebuildAccelerationStructure);
                return vertices.size();
            },
            nb::arg("mesh"), nb::arg("callback"), nb::arg("recompute_normals") = true,
            nb::arg("rebuild_acceleration_structure") = true,
            "Apply a Python callback to each unique vertex. callback(index, (x,y,z))\n"
            "may return a replacement triple, or None to keep the vertex unchanged.")
        .def("get_mesh_vertices_world", [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh) {
                return Float3VectorToList(self.GetMeshVerticesWorld(mesh));
            }, nb::arg("mesh"),
            "Return unique mesh positions as a list of (x, y, z) tuples in world space.\n"
            "The mesh must have exactly one scene instance; pass a SceneNode for instanced meshes.")
        .def("get_mesh_vertices_world", [](SceneEditor& self, const std::shared_ptr<SceneGraphNode>& node) {
                return Float3VectorToList(self.GetMeshVerticesWorld(node));
            }, nb::arg("node"),
            "Return unique vertex positions for this mesh node as world-space (x, y, z) tuples.")
        .def("set_mesh_vertices_world",
            [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh, nb::object vertices,
               bool recomputeNormals, bool rebuildAccelerationStructure) {
                self.SetMeshVerticesWorld(mesh, ToFloat3Vector(vertices), recomputeNormals, rebuildAccelerationStructure);
            },
            nb::arg("mesh"), nb::arg("vertices"), nb::arg("recompute_normals") = true,
            nb::arg("rebuild_acceleration_structure") = true,
            "Replace all unique positions using world-space coordinates. The mesh must have\n"
            "exactly one scene instance; pass a SceneNode for instanced meshes.")
        .def("set_mesh_vertices_world",
            [](SceneEditor& self, const std::shared_ptr<SceneGraphNode>& node, nb::object vertices,
               bool recomputeNormals, bool rebuildAccelerationStructure) {
                self.SetMeshVerticesWorld(node, ToFloat3Vector(vertices), recomputeNormals, rebuildAccelerationStructure);
            },
            nb::arg("node"), nb::arg("vertices"), nb::arg("recompute_normals") = true,
            nb::arg("rebuild_acceleration_structure") = true,
            "Replace all unique positions for this mesh node using world-space coordinates.")
        .def("deform_mesh_world",
            [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh, nb::object callback,
               bool recomputeNormals, bool rebuildAccelerationStructure) {
                std::vector<float3> vertices = self.GetMeshVerticesWorld(mesh);
                for (size_t i = 0; i < vertices.size(); ++i)
                {
                    nb::object updated = callback(i, Float3ToTuple(vertices[i]));
                    if (!updated.is_none())
                        vertices[i] = ToFloat3(updated);
                }
                self.SetMeshVerticesWorld(mesh, vertices, recomputeNormals, rebuildAccelerationStructure);
                return vertices.size();
            },
            nb::arg("mesh"), nb::arg("callback"), nb::arg("recompute_normals") = true,
            nb::arg("rebuild_acceleration_structure") = true,
            "Apply a Python callback to unique world-space vertices. callback(index, (x,y,z))\n"
            "may return a replacement world-space triple, or None to keep the vertex unchanged.")
        .def("deform_mesh_world",
            [](SceneEditor& self, const std::shared_ptr<SceneGraphNode>& node, nb::object callback,
               bool recomputeNormals, bool rebuildAccelerationStructure) {
                std::vector<float3> vertices = self.GetMeshVerticesWorld(node);
                for (size_t i = 0; i < vertices.size(); ++i)
                {
                    nb::object updated = callback(i, Float3ToTuple(vertices[i]));
                    if (!updated.is_none())
                        vertices[i] = ToFloat3(updated);
                }
                self.SetMeshVerticesWorld(node, vertices, recomputeNormals, rebuildAccelerationStructure);
                return vertices.size();
            },
            nb::arg("node"), nb::arg("callback"), nb::arg("recompute_normals") = true,
            nb::arg("rebuild_acceleration_structure") = true,
            "Apply a Python callback to this mesh node's unique world-space vertices.")

        .def("set_environment_map", [](SceneEditor& self, const std::string& path) {
                self.SetEnvMapOverrideSource(path);
            }, nb::arg("path"))

        .def("get_camera_pos_dir_up", [](SceneEditor& self) {
                return self.GetCurrentCameraPosDirUp();
            }, "Returns a comma-separated string of pos.xyz, dir.xyz, up.xyz.")

        .def("set_camera_pos_dir_up", [](SceneEditor& self, const std::string& v) {
                return self.SetCurrentCameraPosDirUp(v);
            }, nb::arg("pos_dir_up"))

        .def("set_camera_fov", [](SceneEditor& self, float fov) { self.SetCameraVerticalFOV(caustica::math::radians(fov)); },
            nb::arg("vertical_fov_degrees"))

        .def("set_camera_intrinsics",
            [](SceneEditor& self, float fx, float fy, float cx, float cy, float width, float height) {
                self.SetCameraIntrinsics(fx, fy, cx, cy, width, height);
            },
            nb::arg("fx"), nb::arg("fy"), nb::arg("cx"), nb::arg("cy"), nb::arg("width"), nb::arg("height"))

        .def("get_camera_fov", [](SceneEditor& self) { return self.GetCameraVerticalFOV(); })

        .def("save_current_camera",  [](SceneEditor& self) { self.SaveCurrentCamera(); })
        .def("load_current_camera",  [](SceneEditor& self) { self.LoadCurrentCamera(); })

        .def("request_shader_reload",  [](SceneEditor& self) { self.GetUIData().ShaderReloadRequested = true; })
        .def("request_accel_rebuild",  [](SceneEditor& self) { self.GetUIData().AccelerationStructRebuildRequested = true; })
        .def("request_mesh_accel_rebuild",
            [](SceneEditor& self, const std::shared_ptr<MeshInfo>& mesh) {
                self.RequestMeshAccelRebuild(mesh);
            },
            nb::arg("mesh"),
            "Request a BLAS rebuild for one dirty mesh without forcing a full scene AS rebuild.")
        .def("request_mesh_accel_rebuild",
            [](SceneEditor& self, const std::shared_ptr<SceneGraphNode>& node) {
                if (!node)
                    throw std::runtime_error("request_mesh_accel_rebuild: node is null");
                std::shared_ptr<MeshInfo> mesh = MeshFromNode(*node);
                if (!mesh)
                    throw std::runtime_error("request_mesh_accel_rebuild: node has no mesh");
                self.RequestMeshAccelRebuild(mesh);
            },
            nb::arg("node"),
            "Request a BLAS rebuild for the mesh attached to one scene node.")
        .def("reset_accumulation",     [](SceneEditor& self) { self.GetUIData().ResetAccumulation = true; })
        .def("reset_realtime_caches",  [](SceneEditor& self) { self.GetUIData().ResetRealtimeCaches = true; })

        .def("set_realtime_mode", [](SceneEditor& self, bool standaloneDenoiser, int realtimeAA)
            {
                auto& ui = self.GetUIData();
                if (!ui.RealtimeMode)
                    ui.ResetAccumulation = true;
                ui.RealtimeMode      = true;
                ui.StandaloneDenoiser = standaloneDenoiser;
                ui.RealtimeAA         = realtimeAA;
            },
            nb::arg("standalone_denoiser") = true,
            nb::arg("realtime_aa") = 2 /*DLSS*/,
            "Switch to realtime path tracing.\n"
            "Args:\n"
            "    standalone_denoiser: enable NRD (no effect with DLSS-RR)\n"
            "    realtime_aa        : 0=Off, 1=TAA, 2=DLSS, 3=DLSS-RR")

        .def("set_reference_mode", [](SceneEditor& self, int spp, bool oidn, int oidnQuality, int oidnPasses, int oidnPrefilter)
            {
                auto& ui = self.GetUIData();
                if (ui.RealtimeMode)
                    ui.ResetAccumulation = true;
                ui.RealtimeMode             = false;
                if (spp > 0)
                    ui.AccumulationTarget   = spp;
                ui.ReferenceOIDNDenoiser    = oidn;
                ui.ReferenceOIDNQuality     = oidnQuality;
                ui.ReferenceOIDNPasses      = oidnPasses;
                ui.ReferenceOIDNPrefilter   = oidnPrefilter;
                ui.ReferenceOIDNDenoiserChanged = true;
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
            [](SceneEditor& self) { return self.AccumulationCompleted(); })
        .def_prop_ro("accumulation_sample_index",
            [](SceneEditor& self) { return self.GetAccumulationSampleIndex(); })
        ;
}

} // namespace caustica_py

#endif // CAUSTICA_WITH_PYTHON
