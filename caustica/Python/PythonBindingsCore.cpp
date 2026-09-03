#if CAUSTICA_WITH_PYTHON

#include "PythonBindingsCore.h"
#include "PythonEngineApp.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/optional.h>
#include <nanobind/operators.h>
#include <nanobind/ndarray.h>

#include <engine/EngineApp.h>
#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/GpuSharedCaches.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/CameraApi.h>
#include <engine/SensorApi.h>
#include <engine/SceneLifecycle.h>
#include <engine/MeshDeformApi.h>
#include <engine/SceneSpawn.h>
#include <scene/ScenePoseAccess.h>
#include <engine/RenderSessionApi.h>
#include <engine/RenderFrameApi.h>
#include <backend/GpuDevice.h>
#include <render/RenderAppState.h>
#include <render/WorldRenderer.h>
#include <render/core/ToneMappingParameters.h>
#include <assets/Handle.h>
#include <assets/TypedAssets.h>
#include <scene/Scene.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/lighting/LightSamplingCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>

#include <scene/Scene.h>
#include <scene/SceneTypes.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneSemanticIds.h>
#include <ecs/Entity.h>
#include <core/log.h>
#include <math/math.h>
#include <shaders/light_types.h>

#include <stdexcept>
#include <cmath>
#include <string>
#include <vector>
#include <optional>
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
using caustica::render::RenderAppState;

// Distinct C++ enum types so nanobind can register them as separate Python
// enums (nb::enum_<T> requires T to be unique across the module).  All map
// 1:1 to ints already used by the underlying Sample / OIDN / Streamline UI.
namespace py_enums
{
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
    enum class GaussianSplatPrimaryMethod : int { GS = 0, GUT = 1 };
    enum class GaussianSplatShadowMode : int { Disabled = 0, Hard = 1, Soft = 2 };
    enum class GaussianSplatFTBSyncMode : int { Disabled = 0, Interlock = 1 };
    enum class LightType : int {
        None = LightType_None,
        Directional = LightType_Directional,
        Spot = LightType_Spot,
        Point = LightType_Point,
        Rect = LightType_Rect,
        Environment = LightType_Environment,
    };
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

    void SetMaterialModelFromPython(StandardMaterial& self, const std::string& value)
    {
        (void)value;
        self.materialModel = "OpenPBR";
        self.useSpecularGlossModel = false;
        if (self.specularColor.x == 0.f && self.specularColor.y == 0.f && self.specularColor.z == 0.f)
            self.specularColor = float3(1.f);
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

    nb::tuple EntityPoseToTuple(const scene::EntityPose& pose)
    {
        return nb::make_tuple(
            Double3ToTuple(pose.position),
            DQuatToXYZWTuple(pose.rotation),
            Double3ToTuple(pose.scaling));
    }

    scene::EntityPose EntityPoseFromPython(
        const nb::object& position,
        const nb::object& rotation,
        const nb::object& scaling)
    {
        scene::EntityPose pose;
        pose.position = ToDouble3(position);
        pose.rotation = ToDQuatXYZW(rotation);
        pose.scaling = ToDouble3(scaling);
        if (!dm::all(dm::isfinite(pose.position))
            || !dm::all(dm::isfinite(pose.rotation))
            || !dm::all(dm::isfinite(pose.scaling)))
        {
            throw std::runtime_error("pose values must be finite");
        }
        const double rotationNorm = dm::length(pose.rotation);
        if (!std::isfinite(rotationNorm) || rotationNorm <= 1e-12)
            throw std::runtime_error("pose rotation quaternion must be non-zero");
        pose.rotation /= rotationNorm;
        return pose;
    }

    nb::tuple CameraPoseToTuple(const CameraPose& pose)
    {
        return nb::make_tuple(
            Float3ToTuple(pose.position),
            Float3ToTuple(pose.direction),
            Float3ToTuple(pose.up));
    }

    CameraPose CameraPoseFromPython(const nb::object& value)
    {
        nb::sequence pose = nb::cast<nb::sequence>(value);
        if (nb::len(pose) != 3)
            throw std::runtime_error("camera_pose must be (position, direction, up)");
        CameraPose result{
            ToFloat3(nb::borrow<nb::object>(pose[0])),
            ToFloat3(nb::borrow<nb::object>(pose[1])),
            ToFloat3(nb::borrow<nb::object>(pose[2])) };
        if (!dm::all(dm::isfinite(result.position))
            || !dm::all(dm::isfinite(result.direction))
            || !dm::all(dm::isfinite(result.up)))
        {
            throw std::runtime_error("camera_pose values must be finite");
        }
        return result;
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

    struct PyScene
    {
        std::shared_ptr<Scene> scene;
        std::shared_ptr<caustica_py::PyEngineAppContext> owner;
    };

    struct PySceneEntity
    {
        std::shared_ptr<Scene> scene;
        std::shared_ptr<caustica_py::PyEngineAppContext> owner;
        ecs::Entity entity = ecs::NullEntity;

        [[nodiscard]] bool isUsable() const
        {
            if (!scene || !owner || !owner->engine || !owner->engine->isValid())
                return false;
            const std::shared_ptr<Scene> active = caustica::activeScene(owner->engine->app());
            if (!active || active.get() != scene.get())
                return false;
            const scene::SceneEntityWorld* world = scene->getEntityWorld();
            return world && world->world().isAlive(entity);
        }

        [[nodiscard]] App* ownerApp() const
        {
            return isUsable() ? &owner->engine->app() : nullptr;
        }

        [[nodiscard]] scene::SceneEntityWorld* entityWorld() const
        {
            return isUsable() ? scene->getEntityWorld() : nullptr;
        }
    };

    std::shared_ptr<PySceneEntity> MakePySceneEntity(
        const std::shared_ptr<Scene>& scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner,
        ecs::Entity entity)
    {
        if (!scene || !owner || !ecs::isValid(entity))
            return nullptr;
        return std::make_shared<PySceneEntity>(PySceneEntity{ scene, owner, entity });
    }

    std::shared_ptr<PyScene> MakePyScene(
        const std::shared_ptr<Scene>& scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner)
    {
        if (!scene || !owner)
            return nullptr;
        return std::make_shared<PyScene>(PyScene{ scene, owner });
    }

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
        if (world.has<scene::RectLightComponent>(self.entity))
            return LightType_Rect;
        if (world.has<scene::EnvironmentLightComponent>(self.entity))
            return LightType_Environment;
        return LightType_None;
    }

    scene::SceneEntityWorld& RequireEntityWorld(PySceneEntity& entity, const char* property);

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
        if (auto* rect = scene::tryGetRectLight(world, self.entity))
            return &rect->color;
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
        if (auto* rect = scene::tryGetRectLight(world, self.entity))
            return &rect->intensity;
        return nullptr;
    }

    void SetLightProperty(PySceneEntity& self, const char* property, const dm::float4& value)
    {
        scene::SceneEntityWorld& entityWorld = RequireEntityWorld(self, property);
        if (!scene::setLightProperty(entityWorld.world(), self.entity, property, value))
        {
            throw std::runtime_error(
                std::string("SceneEntity ") + property + " setter failed: unsupported light property");
        }
    }

    void SetEnvironmentLightPath(PySceneEntity& self, const std::string& path)
    {
        scene::SceneEntityWorld& entityWorld = RequireEntityWorld(self, "environment_path");
        if (auto* environment = scene::tryGetEnvironmentLight(entityWorld.world(), self.entity))
        {
            environment->path = path;
            entityWorld.world().notifyComponentChanged<scene::EnvironmentLightComponent>(self.entity);
            return;
        }
        throw std::runtime_error(
            "SceneEntity environment_path setter failed: entity is not an environment light");
    }


    const App* SceneOwnerApp(const std::shared_ptr<caustica_py::PyEngineAppContext>& owner)
    {
        return owner && owner->engine && owner->engine->isValid()
            ? &owner->engine->app()
            : nullptr;
    }

    std::vector<std::shared_ptr<StandardMaterial>> GetSceneMaterials(
        const Scene* scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner)
    {
        std::vector<std::shared_ptr<StandardMaterial>> result;
        if (!scene)
            return result;

        const App* app = SceneOwnerApp(owner);
        for (const auto& mat : scene->getMaterials())
        {
            const std::shared_ptr<Material> linked = app
                ? linkRuntimeMaterialData(*app, mat)
                : mat;
            if (auto pt = StandardMaterial::safeCast(linked))
                result.push_back(pt);
        }
        return result;
    }

    std::shared_ptr<StandardMaterial> FindSceneMaterial(
        const Scene* scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner,
        const std::string& name)
    {
        if (!scene)
            return nullptr;

        const App* app = SceneOwnerApp(owner);
        for (const auto& mat : scene->getMaterials())
        {
            const std::shared_ptr<Material> linked = app
                ? linkRuntimeMaterialData(*app, mat)
                : mat;
            auto pt = StandardMaterial::safeCast(linked);
            if (pt && (pt->name == name || pt->uniqueName == name))
                return pt;
        }
        return nullptr;
    }

    std::shared_ptr<StandardMaterial> FindSceneMaterialById(
        const Scene* scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner,
        int materialId)
    {
        if (!scene || materialId < 0)
            return nullptr;

        // Scene-only lookup: StandardMaterial::gpuDataIndex only.
        // Prefer EngineApp.find_material / caustica::findMaterial (cache-backed pick id).
        const App* app = SceneOwnerApp(owner);
        for (const auto& mat : scene->getMaterials())
        {
            const std::shared_ptr<Material> linked = app
                ? linkRuntimeMaterialData(*app, mat)
                : mat;
            const auto pt = StandardMaterial::safeCast(linked);
            if (pt && int(pt->gpuDataIndex) == materialId)
                return pt;
        }
        return nullptr;
    }

    std::vector<std::shared_ptr<PySceneEntity>> GetSceneLights(
        const std::shared_ptr<Scene>& scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner)
    {
        std::vector<std::shared_ptr<PySceneEntity>> result;
        if (!scene)
            return result;

        for (ecs::Entity entity : scene->getLightEntities())
            if (auto pyEntity = MakePySceneEntity(scene, owner, entity))
                result.push_back(std::move(pyEntity));
        return result;
    }

    std::vector<std::shared_ptr<PySceneEntity>> GetSceneCameras(
        const std::shared_ptr<Scene>& scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner)
    {
        std::vector<std::shared_ptr<PySceneEntity>> result;
        if (!scene)
            return result;

        for (ecs::Entity entity : scene->getCameraEntities())
            if (auto pyEntity = MakePySceneEntity(scene, owner, entity))
                result.push_back(std::move(pyEntity));
        return result;
    }

    void WalkLightsByName(
        const scene::SceneEntityWorld& entityWorld,
        ecs::Entity entity,
        const std::string& name,
        ecs::Entity& outEntity)
    {
        if (!ecs::isValid(entity) || ecs::isValid(outEntity))
            return;

        const ecs::World& world = entityWorld.world();
        const bool isLight = scene::tryGetDirectionalLight(world, entity)
            || scene::tryGetSpotLight(world, entity)
            || scene::tryGetPointLight(world, entity)
            || scene::tryGetRectLight(world, entity)
            || scene::tryGetEnvironmentLight(world, entity);
        if (isLight && entityWorld.getEntityName(entity) == name)
        {
            outEntity = entity;
            return;
        }

        for (ecs::Entity child : entityWorld.getEntityChildren(entity))
            WalkLightsByName(entityWorld, child, name, outEntity);
    }

    std::shared_ptr<PySceneEntity> FindSceneLight(
        const std::shared_ptr<Scene>& scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner,
        const std::string& name)
    {
        if (!scene || name.empty())
            return nullptr;

        scene::SceneEntityWorld* entityWorld = scene->getEntityWorld();
        if (!entityWorld)
            return nullptr;

        ecs::Entity entity = ecs::NullEntity;
        WalkLightsByName(*entityWorld, entityWorld->root(), name, entity);
        if (!ecs::isValid(entity))
            return nullptr;
        return MakePySceneEntity(scene, owner, entity);
    }

    void WalkCamerasByName(
        const scene::SceneEntityWorld& entityWorld,
        ecs::Entity entity,
        const std::string& name,
        ecs::Entity& outEntity)
    {
        if (!ecs::isValid(entity) || ecs::isValid(outEntity))
            return;

        if (scene::tryGetCamera(entityWorld.world(), entity) && entityWorld.getEntityName(entity) == name)
        {
            outEntity = entity;
            return;
        }

        for (ecs::Entity child : entityWorld.getEntityChildren(entity))
            WalkCamerasByName(entityWorld, child, name, outEntity);
    }

    std::shared_ptr<PySceneEntity> FindSceneCamera(
        const std::shared_ptr<Scene>& scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner,
        const std::string& name)
    {
        if (!scene || name.empty())
            return nullptr;

        scene::SceneEntityWorld* entityWorld = scene->getEntityWorld();
        if (!entityWorld)
            return nullptr;

        ecs::Entity entity = ecs::NullEntity;
        WalkCamerasByName(*entityWorld, entityWorld->root(), name, entity);
        if (!ecs::isValid(entity))
            return nullptr;
        return MakePySceneEntity(scene, owner, entity);
    }

    std::string MakeUniqueLightName(
        const std::shared_ptr<Scene>& scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner,
        const std::string& requested,
        const char* prefix)
    {
        if (!requested.empty())
            return requested;

        for (int i = 0; ; ++i)
        {
            const std::string candidate = i == 0
                ? std::string(prefix)
                : std::string(prefix) + "_" + std::to_string(i);
            if (!FindSceneLight(scene, owner, candidate))
                return candidate;
        }
    }

    std::shared_ptr<PySceneEntity> PyEntityFromEntity(
        const std::shared_ptr<Scene>& scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner,
        ecs::Entity entity)
    {
        return MakePySceneEntity(scene, owner, entity);
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

    std::shared_ptr<PySceneEntity> FindSceneEntity(
        const std::shared_ptr<Scene>& scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner,
        const std::string& path)
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

        return MakePySceneEntity(scene, owner, entity);
    }

    std::vector<std::shared_ptr<PySceneEntity>> GetSceneMeshEntities(
        const std::shared_ptr<Scene>& scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner)
    {
        std::vector<std::shared_ptr<PySceneEntity>> result;
        if (!scene)
            return result;

        for (ecs::Entity entity : scene->getMeshInstances())
        {
            if (auto pyEntity = PyEntityFromEntity(scene, owner, entity))
                result.push_back(std::move(pyEntity));
        }
        return result;
    }

    std::shared_ptr<PySceneEntity> FindSceneMeshEntity(
        const std::shared_ptr<Scene>& scene,
        const std::shared_ptr<caustica_py::PyEngineAppContext>& owner,
        const std::string& name)
    {
        if (!scene || name.empty())
            return nullptr;

        scene::SceneEntityWorld* entityWorld = scene->getEntityWorld();
        for (ecs::Entity entity : scene->getMeshInstances())
        {
            if (!ecs::isValid(entity))
                continue;

            if (entityWorld && entityWorld->getEntityName(entity) == name)
                return PyEntityFromEntity(scene, owner, entity);

            if (entityWorld)
            {
                const auto* meshComponent =
                    entityWorld->world().tryGet<scene::MeshInstanceComponent>(entity);
                if (meshComponent && meshComponent->mesh && meshComponent->mesh->name == name)
                    return PyEntityFromEntity(scene, owner, entity);
            }
        }
        return nullptr;
    }

    ecs::Entity EntityFromPy(const std::shared_ptr<PySceneEntity>& entity)
    {
        if (!entity || !ecs::isValid(entity->entity))
            throw std::runtime_error("SceneEntity is null or invalid");
        return entity->entity;
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

    App& RequirePyApp(caustica_py::PyEngineApp& self)
    {
        App* app = self.tryApp();
        if (!app)
            throw std::runtime_error("caustica.EngineApp: engine is closed");
        return *app;
    }

    bool EntityIsCamera(const PySceneEntity& self)
    {
        scene::SceneEntityWorld* entityWorld = self.entityWorld();
        return entityWorld && scene::tryGetCamera(entityWorld->world(), self.entity) != nullptr;
    }

    void RequireCamera(const PySceneEntity& self)
    {
        if (!self.isUsable())
            throw std::runtime_error("SceneEntity is stale, closed, or belongs to another EngineApp");
        if (!EntityIsCamera(self))
            throw std::runtime_error("SceneEntity is not a camera");
    }

    std::shared_ptr<Scene> RequirePyScene(caustica_py::PyEngineApp& self)
    {
        std::shared_ptr<Scene> scene = caustica::activeScene(RequirePyApp(self));
        if (!scene)
            throw std::runtime_error("caustica.EngineApp: no active scene");
        return scene;
    }

    App& RequireEntityApp(const PySceneEntity& entity)
    {
        if (App* app = entity.ownerApp())
            return *app;
        throw std::runtime_error("SceneEntity is stale, closed, or belongs to another EngineApp");
    }

    void RequireEntityForApp(
        const caustica_py::PyEngineApp& app,
        const std::shared_ptr<PySceneEntity>& entity)
    {
        if (!entity || !entity->isUsable() || entity->owner != app.context())
            throw std::runtime_error("SceneEntity belongs to another EngineApp or is no longer active");
    }

    scene::SceneEntityWorld& RequireEntityWorld(PySceneEntity& entity, const char* property)
    {
        if (scene::SceneEntityWorld* world = entity.entityWorld())
            return *world;
        throw std::runtime_error(
            std::string("SceneEntity ") + property + " setter failed: entity is stale or invalid");
    }

    nb::object MaterialTexturePath(const StandardMaterial& material, StandardMaterialTextureSlot slot)
    {
        const StandardMaterialTexture& texture = material.getTexture(slot);
        if (texture.loaded == nullptr || texture.localPath.empty())
            return nb::none();
        return nb::str(texture.localPath.generic_string().c_str());
    }

    bool SetMaterialTextureFromPython(
        StandardMaterial& material,
        StandardMaterialTextureSlot slot,
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

    void ClearMaterialTextureFromPython(StandardMaterial& material, StandardMaterialTextureSlot slot)
    {
        if (material.runtimeMaterialGpuCache == nullptr)
            throw std::runtime_error("Material is not attached to a live MaterialGpuCache. Reload the scene and look up the material again.");

        material.runtimeMaterialGpuCache->clearMaterialTexture(material, slot);
    }
}

namespace caustica_py
{

namespace
{
    caustica::EngineApp* g_embedEngine = nullptr;
}

void setEmbedEngine(caustica::EngineApp* engine)
{
    g_embedEngine = engine;
}

caustica::EngineApp* embedEngine()
{
    return g_embedEngine;
}

namespace
{

bool SensorShape(const caustica::SensorOutput& output, uint32_t channels, size_t count, uint32_t& width, uint32_t& height)
{
    if (count == 0 || channels == 0)
        return false;
    width = output.width;
    height = output.height;
    const size_t pixels = count / channels;
    if (channels != 0 && count % channels != 0)
        return false;
    const size_t expected = size_t(width) * size_t(height) * size_t(channels);
    if (count == expected)
        return width != 0 && height != 0;
    if (width != 0 && pixels % width == 0)
    {
        height = uint32_t(pixels / width);
        return height != 0;
    }
    return false;
}

} // namespace

nb::object sensorRgbNumpy(const caustica::SensorOutput& output)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (!SensorShape(output, 4, output.rgb.size(), width, height))
        return nb::none();
    auto* data = new std::vector<uint8_t>(output.rgb);
    nb::capsule owner(data, [](void* p) noexcept { delete static_cast<std::vector<uint8_t>*>(p); });
    return nb::cast(nb::ndarray<nb::numpy, uint8_t, nb::shape<-1, -1, 4>, nb::c_contig, nb::device::cpu>(
        data->data(), { height, width, 4 }, owner));
}

nb::object sensorDepthNumpy(const caustica::SensorOutput& output)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (!SensorShape(output, 1, output.depth.size(), width, height))
        return nb::none();
    auto* data = new std::vector<float>(output.depth);
    nb::capsule owner(data, [](void* p) noexcept { delete static_cast<std::vector<float>*>(p); });
    return nb::cast(nb::ndarray<nb::numpy, float, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu>(
        data->data(), { height, width }, owner));
}

nb::object sensorNormalNumpy(const caustica::SensorOutput& output)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (!SensorShape(output, 3, output.normal.size(), width, height))
        return nb::none();
    auto* data = new std::vector<float>(output.normal);
    nb::capsule owner(data, [](void* p) noexcept { delete static_cast<std::vector<float>*>(p); });
    return nb::cast(nb::ndarray<nb::numpy, float, nb::shape<-1, -1, 3>, nb::c_contig, nb::device::cpu>(
        data->data(), { height, width, 3 }, owner));
}

nb::object sensorInstanceIdNumpy(const caustica::SensorOutput& output)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (!SensorShape(output, 1, output.instanceId.size(), width, height))
        return nb::none();
    auto* data = new std::vector<uint32_t>(output.instanceId);
    nb::capsule owner(data, [](void* p) noexcept { delete static_cast<std::vector<uint32_t>*>(p); });
    return nb::cast(nb::ndarray<nb::numpy, uint32_t, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu>(
        data->data(), { height, width }, owner));
}

nb::object sensorSemanticIdNumpy(const caustica::SensorOutput& output)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (!SensorShape(output, 1, output.semanticId.size(), width, height))
        return nb::none();
    auto* data = new std::vector<uint32_t>(output.semanticId);
    nb::capsule owner(data, [](void* p) noexcept { delete static_cast<std::vector<uint32_t>*>(p); });
    return nb::cast(nb::ndarray<nb::numpy, uint32_t, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu>(
        data->data(), { height, width }, owner));
}

nb::object sensorMotionVectorNumpy(const caustica::SensorOutput& output)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (!SensorShape(output, 2, output.motionVector.size(), width, height))
        return nb::none();
    auto* data = new std::vector<float>(output.motionVector);
    nb::capsule owner(data, [](void* p) noexcept { delete static_cast<std::vector<float>*>(p); });
    return nb::cast(nb::ndarray<nb::numpy, float, nb::shape<-1, -1, 2>, nb::c_contig, nb::device::cpu>(
        data->data(), { height, width, 2 }, owner));
}

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

    nb::enum_<ToneMapperOperator>(m, "ToneMapOperator",
        "Tone-mapping curve applied when Settings.enable_tone_mapping is true.",
        nb::is_arithmetic())
        .value("Linear", ToneMapperOperator::Linear)
        .value("Reinhard", ToneMapperOperator::Reinhard)
        .value("ReinhardModified", ToneMapperOperator::ReinhardModified)
        .value("HejiHableAlu", ToneMapperOperator::HejiHableAlu)
        .value("HableUc2", ToneMapperOperator::HableUc2)
        .value("Aces", ToneMapperOperator::Aces)
        .value("PbrNeutral", ToneMapperOperator::PbrNeutral)
        .value("IdentitySoftShoulder", ToneMapperOperator::IdentitySoftShoulder)
        .value("AgX", ToneMapperOperator::AgX)
        .value("CameraLut", ToneMapperOperator::CameraLut);

    nb::enum_<ExposureMode>(m, "ExposureMode",
        "Camera exposure control mode used by ToneMappingParams.",
        nb::is_arithmetic())
        .value("AperturePriority", ExposureMode::AperturePriority)
        .value("ShutterPriority", ExposureMode::ShutterPriority);

    nb::enum_<CameraLutPreset>(m, "CameraLutPreset",
        "Built-in optional 1D camera-LUT looks.",
        nb::is_arithmetic())
        .value("Disabled", CameraLutPreset::None)
        .value("Neutral", CameraLutPreset::Neutral)
        .value("SoftContrast", CameraLutPreset::SoftContrast)
        .value("WarmFilm", CameraLutPreset::WarmFilm)
        .value("CoolFilm", CameraLutPreset::CoolFilm);

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

    nb::enum_<py_enums::GaussianSplatPrimaryMethod>(m, "GaussianSplatPrimaryMethod",
        "Primary splat color path (orthogonal to mesh shadows).",
        nb::is_arithmetic())
        .value("GS",  py_enums::GaussianSplatPrimaryMethod::GS)
        .value("GUT", py_enums::GaussianSplatPrimaryMethod::GUT)
        .export_values();

    nb::enum_<GaussianSplatShadowMode>(m, "GaussianSplatShadowMode",
        "Mesh BVH shadow mode for the splat primary path (Off disables RT shadows).",
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

    nb::enum_<StandardMaterialTextureSlot>(m, "TextureSlot",
        "Material texture slot for runtime texture replacement.",
        nb::is_arithmetic())
        .value("Base", StandardMaterialTextureSlot::Base)
        .value("ORM", StandardMaterialTextureSlot::OcclusionRoughnessMetallic)
        .value("OcclusionRoughnessMetallic", StandardMaterialTextureSlot::OcclusionRoughnessMetallic)
        .value("Normal", StandardMaterialTextureSlot::Normal)
        .value("CoatNormal", StandardMaterialTextureSlot::CoatNormal)
        .value("Emissive", StandardMaterialTextureSlot::Emissive)
        .value("Transmission", StandardMaterialTextureSlot::Transmission)
        .export_values();

    nb::enum_<LightType>(m, "LightType",
        "Light entity kind. Matches SceneEntity.light_type and shaders/light_types.h.",
        nb::is_arithmetic())
        .value("None_", LightType::None, "Not a light (LightType_None / 0).")
        .value("Directional", LightType::Directional)
        .value("Spot", LightType::Spot)
        .value("Point", LightType::Point)
        .value("Rect", LightType::Rect)
        .value("Environment", LightType::Environment);

    nb::enum_<Aov>(m, "Aov",
        "Sensor / AOV mask. Combine with bitwise OR: Aov.rgb | Aov.depth.",
        nb::is_arithmetic())
        .value("none", Aov::None)
        .value("rgb", Aov::Rgb)
        .value("depth", Aov::Depth)
        .value("normal", Aov::Normal)
        .value("instance_id", Aov::InstanceId)
        .value("semantic_id", Aov::SemanticId)
        .value("motion_vector", Aov::MotionVector)
        .value("segmentation", Aov::Segmentation)
        .value("all", Aov::All)
        .def("__or__", [](Aov a, Aov b) { return uint32_t(a) | uint32_t(b); })
        .def("__or__", [](Aov a, uint32_t b) { return uint32_t(a) | b; })
        .def("__ror__", [](Aov a, uint32_t b) { return uint32_t(a) | b; });

    nb::class_<SensorOutput>(m, "SensorOutput",
        "One captured camera + AOV set. Empty arrays mean the AOV was not requested.")
        .def_ro("name", &SensorOutput::name)
        .def_ro("width", &SensorOutput::width)
        .def_ro("height", &SensorOutput::height)
        .def_ro("aovs", &SensorOutput::aovs)
        .def_prop_ro("rgb", [](const SensorOutput& self) { return sensorRgbNumpy(self); },
            "NumPy (H, W, 4) uint8 RGBA, or None.")
        .def_prop_ro("depth", [](const SensorOutput& self) { return sensorDepthNumpy(self); },
            "NumPy (H, W) float32 linear |view Z| meters. 0 = miss.")
        .def_prop_ro("normal", [](const SensorOutput& self) { return sensorNormalNumpy(self); },
            "NumPy (H, W, 3) float32 camera-space normals.")
        .def_prop_ro("instance_id", [](const SensorOutput& self) { return sensorInstanceIdNumpy(self); },
            "NumPy (H, W) uint32. 0 = miss.")
        .def_prop_ro("semantic_id", [](const SensorOutput& self) { return sensorSemanticIdNumpy(self); },
            "NumPy (H, W) uint32. 0 = unlabeled / miss.")
        .def_prop_ro("segmentation", [](const SensorOutput& self) { return sensorInstanceIdNumpy(self); },
            "Alias of instance_id.")
        .def_prop_ro("motion_vector", [](const SensorOutput& self) { return sensorMotionVectorNumpy(self); },
            "NumPy (H, W, 2) float32 screen-space motion in pixels.")
        .def("__repr__", [](const SensorOutput& self) {
            return std::string("<caustica.SensorOutput '") + self.name + "' "
                + std::to_string(self.width) + "x" + std::to_string(self.height) + ">";
        });

    nb::class_<Handle<ScenePrefabAsset>>(m, "ScenePrefab",
        "CPU-side prefab handle from Sample.load() / assets.load. Pass to Sample.spawn().")
        .def("__bool__", [](const Handle<ScenePrefabAsset>& self) { return bool(self); })
        .def_prop_ro("valid", [](const Handle<ScenePrefabAsset>& self) { return self.isValid(); })
        .def_prop_ro("name", [](const Handle<ScenePrefabAsset>& self) -> std::string {
                return self ? self->name : std::string{};
            })
        .def_prop_ro("source_path", [](const Handle<ScenePrefabAsset>& self) -> std::string {
                return self ? self->sourcePath.string() : std::string{};
            });

    // --- StandardMaterial -------------------------------------------------------
    nb::class_<StandardMaterial>(m, "Material",
        "caustica material wrapper (StandardMaterial). All edits flag the material as\n"
        "dirty so the GPU buffer is re-uploaded the following frame.")
        .def_ro("name",         &StandardMaterial::name)
        .def_ro("model_name",   &StandardMaterial::modelName)
        .def_ro("unique_name",  &StandardMaterial::uniqueName)

        .def_prop_rw("base_color",
            [](StandardMaterial& self) { return Float3ToTuple(self.baseOrDiffuseColor); },
            [](StandardMaterial& self, nb::object v) { self.baseOrDiffuseColor = ToFloat3(v); self.gpuDataDirty = true; },
            "Metal-rough base color or spec-gloss diffuse color (linear RGB).")
        .def_prop_rw("specular_color",
            [](StandardMaterial& self) { return Float3ToTuple(self.specularColor); },
            [](StandardMaterial& self, nb::object v) { self.specularColor = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("emissive_color",
            [](StandardMaterial& self) { return Float3ToTuple(self.emissiveColor); },
            [](StandardMaterial& self, nb::object v) { self.emissiveColor = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("emissive_intensity",
            [](StandardMaterial& self) { return self.emissiveIntensity; },
            [](StandardMaterial& self, float v) { self.emissiveIntensity = v; self.gpuDataDirty = true; })
        .def_prop_rw("metalness",
            [](StandardMaterial& self) { return self.metalness; },
            [](StandardMaterial& self, float v) { self.metalness = v; self.gpuDataDirty = true; })
        .def_prop_rw("roughness",
            [](StandardMaterial& self) { return self.roughness; },
            [](StandardMaterial& self, float v) { self.roughness = v; self.gpuDataDirty = true; })
        .def_prop_rw("material_model",
            [](StandardMaterial& self) { return self.materialModel; },
            [](StandardMaterial& self, const std::string& v) { SetMaterialModelFromPython(self, v); },
            "Always OpenPBR. Writes coerce any value to OpenPBR.")
        .def_prop_rw("base_weight",
            [](StandardMaterial& self) { return self.baseWeight; },
            [](StandardMaterial& self, float v) { self.baseWeight = v; self.gpuDataDirty = true; })
        .def_prop_rw("base_diffuse_roughness",
            [](StandardMaterial& self) { return self.baseDiffuseRoughness; },
            [](StandardMaterial& self, float v) { self.baseDiffuseRoughness = v; self.gpuDataDirty = true; })
        .def_prop_rw("specular_weight",
            [](StandardMaterial& self) { return self.specularWeight; },
            [](StandardMaterial& self, float v) { self.specularWeight = v; self.gpuDataDirty = true; })
        .def_prop_rw("anisotropy",
            [](StandardMaterial& self) { return self.anisotropy; },
            [](StandardMaterial& self, float v) { self.anisotropy = v; self.gpuDataDirty = true; })
        .def_prop_rw("fuzz_weight",
            [](StandardMaterial& self) { return self.fuzzWeight; },
            [](StandardMaterial& self, float v) { self.fuzzWeight = v; self.gpuDataDirty = true; })
        .def_prop_rw("fuzz_color",
            [](StandardMaterial& self) { return Float3ToTuple(self.fuzzColor); },
            [](StandardMaterial& self, nb::object v) { self.fuzzColor = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("fuzz_roughness",
            [](StandardMaterial& self) { return self.fuzzRoughness; },
            [](StandardMaterial& self, float v) { self.fuzzRoughness = v; self.gpuDataDirty = true; })
        .def_prop_rw("coat_weight",
            [](StandardMaterial& self) { return self.coatWeight; },
            [](StandardMaterial& self, float v) { self.coatWeight = v; self.gpuDataDirty = true; })
        .def_prop_rw("coat_color",
            [](StandardMaterial& self) { return Float3ToTuple(self.coatColor); },
            [](StandardMaterial& self, nb::object v) { self.coatColor = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("coat_roughness",
            [](StandardMaterial& self) { return self.coatRoughness; },
            [](StandardMaterial& self, float v) { self.coatRoughness = v; self.gpuDataDirty = true; })
        .def_prop_rw("coat_roughness_anisotropy",
            [](StandardMaterial& self) { return self.coatAnisotropy; },
            [](StandardMaterial& self, float v) { self.coatAnisotropy = v; self.gpuDataDirty = true; })
        .def_prop_rw("coat_ior",
            [](StandardMaterial& self) { return self.coatIor; },
            [](StandardMaterial& self, float v) { self.coatIor = v; self.gpuDataDirty = true; })
        .def_prop_rw("coat_darkening",
            [](StandardMaterial& self) { return self.coatDarkening; },
            [](StandardMaterial& self, float v) { self.coatDarkening = v; self.gpuDataDirty = true; })
        .def_prop_rw("subsurface_weight",
            [](StandardMaterial& self) { return self.subsurfaceWeight; },
            [](StandardMaterial& self, float v) { self.subsurfaceWeight = v; self.gpuDataDirty = true; })
        .def_prop_rw("subsurface_color",
            [](StandardMaterial& self) { return Float3ToTuple(self.subsurfaceColor); },
            [](StandardMaterial& self, nb::object v) { self.subsurfaceColor = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("subsurface_radius",
            [](StandardMaterial& self) { return self.subsurfaceRadius; },
            [](StandardMaterial& self, float v) { self.subsurfaceRadius = v; self.gpuDataDirty = true; })
        .def_prop_rw("subsurface_radius_scale",
            [](StandardMaterial& self) { return Float3ToTuple(self.subsurfaceRadiusScale); },
            [](StandardMaterial& self, nb::object v) { self.subsurfaceRadiusScale = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("subsurface_anisotropy",
            [](StandardMaterial& self) { return self.subsurfaceAnisotropy; },
            [](StandardMaterial& self, float v) { self.subsurfaceAnisotropy = v; self.gpuDataDirty = true; })
        .def_prop_rw("thin_film_weight",
            [](StandardMaterial& self) { return self.thinFilmWeight; },
            [](StandardMaterial& self, float v) { self.thinFilmWeight = v; self.gpuDataDirty = true; })
        .def_prop_rw("thin_film_thickness",
            [](StandardMaterial& self) { return self.thinFilmThickness; },
            [](StandardMaterial& self, float v) { self.thinFilmThickness = v; self.gpuDataDirty = true; })
        .def_prop_rw("thin_film_ior",
            [](StandardMaterial& self) { return self.thinFilmIor; },
            [](StandardMaterial& self, float v) { self.thinFilmIor = v; self.gpuDataDirty = true; })
        .def_prop_rw("transmission_color",
            [](StandardMaterial& self) { return Float3ToTuple(self.transmissionColor); },
            [](StandardMaterial& self, nb::object v) { self.transmissionColor = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("transmission_depth",
            [](StandardMaterial& self) { return self.transmissionDepth; },
            [](StandardMaterial& self, float v) { self.transmissionDepth = v; self.gpuDataDirty = true; })
        .def_prop_rw("transmission_scatter",
            [](StandardMaterial& self) { return Float3ToTuple(self.transmissionScatter); },
            [](StandardMaterial& self, nb::object v) { self.transmissionScatter = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("transmission_scatter_anisotropy",
            [](StandardMaterial& self) { return self.transmissionScatterAnisotropy; },
            [](StandardMaterial& self, float v) { self.transmissionScatterAnisotropy = v; self.gpuDataDirty = true; })
        .def_prop_rw("transmission_dispersion_scale",
            [](StandardMaterial& self) { return self.transmissionDispersionScale; },
            [](StandardMaterial& self, float v) { self.transmissionDispersionScale = v; self.gpuDataDirty = true; })
        .def_prop_rw("transmission_dispersion_abbe_number",
            [](StandardMaterial& self) { return self.transmissionDispersionAbbeNumber; },
            [](StandardMaterial& self, float v) { self.transmissionDispersionAbbeNumber = v; self.gpuDataDirty = true; })
        .def_prop_rw("opacity",
            [](StandardMaterial& self) { return self.opacity; },
            [](StandardMaterial& self, float v) { self.opacity = v; self.gpuDataDirty = true; })
        .def_prop_rw("transmission_factor",
            [](StandardMaterial& self) { return self.transmissionFactor; },
            [](StandardMaterial& self, float v) { self.transmissionFactor = v; self.gpuDataDirty = true; })
        .def_prop_rw("diffuse_transmission_factor",
            [](StandardMaterial& self) { return self.diffuseTransmissionFactor; },
            [](StandardMaterial& self, float v) { self.diffuseTransmissionFactor = v; self.gpuDataDirty = true; })
        .def_prop_rw("normal_texture_scale",
            [](StandardMaterial& self) { return self.normalTextureScale; },
            [](StandardMaterial& self, float v) { self.normalTextureScale = v; self.gpuDataDirty = true; })
        .def_prop_rw("coat_normal_scale",
            [](StandardMaterial& self) { return self.coatNormalTextureScale; },
            [](StandardMaterial& self, float v) { self.coatNormalTextureScale = v; self.gpuDataDirty = true; })
        .def_prop_rw("ior",
            [](StandardMaterial& self) { return self.IoR; },
            [](StandardMaterial& self, float v) { self.IoR = v; self.gpuDataDirty = true; })
        .def_prop_rw("alpha_cutoff",
            [](StandardMaterial& self) { return self.alphaCutoff; },
            [](StandardMaterial& self, float v) { self.alphaCutoff = v; self.gpuDataDirty = true; })

        .def_prop_rw("volume_attenuation_distance",
            [](StandardMaterial& self) { return self.volumeAttenuationDistance; },
            [](StandardMaterial& self, float v) { self.volumeAttenuationDistance = v; self.gpuDataDirty = true; })
        .def_prop_rw("volume_attenuation_color",
            [](StandardMaterial& self) { return Float3ToTuple(self.volumeAttenuationColor); },
            [](StandardMaterial& self, nb::object v) { self.volumeAttenuationColor = ToFloat3(v); self.gpuDataDirty = true; })
        .def_prop_rw("nested_priority",
            [](StandardMaterial& self) { return self.nestedPriority; },
            [](StandardMaterial& self, int v) { self.nestedPriority = v; self.gpuDataDirty = true; })

        .def_prop_rw("use_specular_gloss",
            [](StandardMaterial& self) { return self.useSpecularGlossModel; },
            [](StandardMaterial& self, bool v) { self.useSpecularGlossModel = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_alpha_testing",
            [](StandardMaterial& self) { return self.enableAlphaTesting; },
            [](StandardMaterial& self, bool v) { self.enableAlphaTesting = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_transmission",
            [](StandardMaterial& self) { return self.enableTransmission; },
            [](StandardMaterial& self, bool v) { self.enableTransmission = v; self.gpuDataDirty = true; })
        .def_prop_rw("thin_surface",
            [](StandardMaterial& self) { return self.thinSurface; },
            [](StandardMaterial& self, bool v) { self.thinSurface = v; self.gpuDataDirty = true; })
        .def_prop_rw("exclude_from_nee",
            [](StandardMaterial& self) { return self.excludeFromNEE; },
            [](StandardMaterial& self, bool v) { self.excludeFromNEE = v; self.gpuDataDirty = true; })
        .def_prop_rw("unlit_receive_shadows",
            [](StandardMaterial& self) { return self.unlitReceiveShadows; },
            [](StandardMaterial& self, bool v) { self.unlitReceiveShadows = v; self.gpuDataDirty = true; })
        .def_prop_rw("unlit_shadow_strength",
            [](StandardMaterial& self) { return self.unlitShadowStrength; },
            [](StandardMaterial& self, float v) { self.unlitShadowStrength = std::clamp(v, 0.0f, 1.0f); self.gpuDataDirty = true; })
        .def_prop_rw("enable_as_analytic_light_proxy",
            [](StandardMaterial& self) { return self.enableAsAnalyticLightProxy; },
            [](StandardMaterial& self, bool v) { self.enableAsAnalyticLightProxy = v; self.gpuDataDirty = true; })
        .def_prop_rw("skip_render",
            [](StandardMaterial& self) { return self.skipRender; },
            [](StandardMaterial& self, bool v) { self.skipRender = v; self.gpuDataDirty = true; })
        .def_prop_rw("metalness_in_red_channel",
            [](StandardMaterial& self) { return self.metalnessInRedChannel; },
            [](StandardMaterial& self, bool v) { self.metalnessInRedChannel = v; self.gpuDataDirty = true; })

        .def_prop_rw("enable_base_texture",
            [](StandardMaterial& self) { return self.enableBaseTexture; },
            [](StandardMaterial& self, bool v) { self.enableBaseTexture = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_orm_texture",
            [](StandardMaterial& self) { return self.enableOcclusionRoughnessMetallicTexture; },
            [](StandardMaterial& self, bool v) { self.enableOcclusionRoughnessMetallicTexture = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_normal_texture",
            [](StandardMaterial& self) { return self.enableNormalTexture; },
            [](StandardMaterial& self, bool v) { self.enableNormalTexture = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_coat_normal_texture",
            [](StandardMaterial& self) { return self.enableCoatNormalTexture; },
            [](StandardMaterial& self, bool v) { self.enableCoatNormalTexture = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_emissive_texture",
            [](StandardMaterial& self) { return self.enableEmissiveTexture; },
            [](StandardMaterial& self, bool v) { self.enableEmissiveTexture = v; self.gpuDataDirty = true; })
        .def_prop_rw("enable_transmission_texture",
            [](StandardMaterial& self) { return self.enableTransmissionTexture; },
            [](StandardMaterial& self, bool v) { self.enableTransmissionTexture = v; self.gpuDataDirty = true; })

        .def_prop_ro("base_texture_path",
            [](StandardMaterial& self) { return MaterialTexturePath(self, StandardMaterialTextureSlot::Base); })
        .def_prop_ro("orm_texture_path",
            [](StandardMaterial& self) { return MaterialTexturePath(self, StandardMaterialTextureSlot::OcclusionRoughnessMetallic); })
        .def_prop_ro("normal_texture_path",
            [](StandardMaterial& self) { return MaterialTexturePath(self, StandardMaterialTextureSlot::Normal); })
        .def_prop_ro("coat_normal_texture_path",
            [](StandardMaterial& self) { return MaterialTexturePath(self, StandardMaterialTextureSlot::CoatNormal); })
        .def_prop_ro("emissive_texture_path",
            [](StandardMaterial& self) { return MaterialTexturePath(self, StandardMaterialTextureSlot::Emissive); })
        .def_prop_ro("transmission_texture_path",
            [](StandardMaterial& self) { return MaterialTexturePath(self, StandardMaterialTextureSlot::Transmission); })

        .def("set_texture",
            [](StandardMaterial& self, StandardMaterialTextureSlot slot, const std::string& path, std::optional<bool> sRGB, std::optional<bool> normalMap) {
                return SetMaterialTextureFromPython(self, slot, path, sRGB, normalMap);
            },
            nb::arg("slot"), nb::arg("path"), nb::arg("srgb") = nb::none(), nb::arg("normal_map") = nb::none(),
            "Replace one material texture slot from a file path. Returns False if the file cannot be resolved.")
        .def("set_base_texture",
            [](StandardMaterial& self, const std::string& path, std::optional<bool> sRGB) {
                return SetMaterialTextureFromPython(self, StandardMaterialTextureSlot::Base, path, sRGB, false);
            },
            nb::arg("path"), nb::arg("srgb") = nb::none())
        .def("set_orm_texture",
            [](StandardMaterial& self, const std::string& path, std::optional<bool> sRGB) {
                return SetMaterialTextureFromPython(self, StandardMaterialTextureSlot::OcclusionRoughnessMetallic, path, sRGB, false);
            },
            nb::arg("path"), nb::arg("srgb") = nb::none())
        .def("set_normal_texture",
            [](StandardMaterial& self, const std::string& path) {
                return SetMaterialTextureFromPython(self, StandardMaterialTextureSlot::Normal, path, false, true);
            },
            nb::arg("path"))
        .def("set_coat_normal_texture",
            [](StandardMaterial& self, const std::string& path) {
                return SetMaterialTextureFromPython(self, StandardMaterialTextureSlot::CoatNormal, path, false, true);
            },
            nb::arg("path"))
        .def("set_emissive_texture",
            [](StandardMaterial& self, const std::string& path, std::optional<bool> sRGB) {
                return SetMaterialTextureFromPython(self, StandardMaterialTextureSlot::Emissive, path, sRGB, false);
            },
            nb::arg("path"), nb::arg("srgb") = nb::none())
        .def("set_transmission_texture",
            [](StandardMaterial& self, const std::string& path, std::optional<bool> sRGB) {
                return SetMaterialTextureFromPython(self, StandardMaterialTextureSlot::Transmission, path, sRGB, false);
            },
            nb::arg("path"), nb::arg("srgb") = nb::none())
        .def("clear_texture",
            [](StandardMaterial& self, StandardMaterialTextureSlot slot) { ClearMaterialTextureFromPython(self, slot); },
            nb::arg("slot"),
            "Disconnect and disable one material texture slot.")
        .def("clear_base_texture",
            [](StandardMaterial& self) { ClearMaterialTextureFromPython(self, StandardMaterialTextureSlot::Base); })
        .def("clear_orm_texture",
            [](StandardMaterial& self) { ClearMaterialTextureFromPython(self, StandardMaterialTextureSlot::OcclusionRoughnessMetallic); })
        .def("clear_normal_texture",
            [](StandardMaterial& self) { ClearMaterialTextureFromPython(self, StandardMaterialTextureSlot::Normal); })
        .def("clear_coat_normal_texture",
            [](StandardMaterial& self) { ClearMaterialTextureFromPython(self, StandardMaterialTextureSlot::CoatNormal); })
        .def("clear_emissive_texture",
            [](StandardMaterial& self) { ClearMaterialTextureFromPython(self, StandardMaterialTextureSlot::Emissive); })
        .def("clear_transmission_texture",
            [](StandardMaterial& self) { ClearMaterialTextureFromPython(self, StandardMaterialTextureSlot::Transmission); })

        .def("mark_dirty", [](StandardMaterial& self) { self.gpuDataDirty = true; },
             "Force this material's GPU buffer slot to be refreshed next frame.")
        .def("__repr__", [](const StandardMaterial& self) {
                return std::string("<caustica.Material '") + self.name + "'>";
            });

    // Lights are ECS typed components on SceneEntity (no OO Light hierarchy).

    nb::class_<MeshHandle>(m, "MeshHandle",
        "Asset-system mesh identity. Prefer SceneEntity + Sample.*_mesh*(entity=...).")
        .def_prop_ro("valid", [](const MeshHandle& self) { return bool(self); })
        .def_prop_ro("name", [](const MeshHandle& self) -> std::string {
                return self ? self->name : std::string{};
            })
        .def("__repr__", [](const MeshHandle& self) {
                return self
                    ? (std::string("<caustica.MeshHandle '") + self->name + "'>")
                    : std::string("<caustica.MeshHandle invalid>");
            });

    nb::class_<PySceneEntity>(m, "SceneEntity",
        "ECS scene entity wrapper for runtime mesh/light/camera transforms.")
        .def_prop_ro("name", [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                return entityWorld ? entityWorld->getEntityName(self.entity) : std::string{};
            })
        .def_prop_ro("path", [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                return entityWorld ? entityWorld->getEntityPath(self.entity).generic_string() : std::string{};
            })
        .def_prop_ro("mesh_handle", [](PySceneEntity& self) -> MeshHandle {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld || !ecs::isValid(self.entity))
                    return {};
                const auto* meshComponent =
                    entityWorld->world().tryGet<scene::MeshInstanceComponent>(self.entity);
                return meshComponent ? meshComponent->meshHandle() : MeshHandle{};
            }, "MeshHandle for this mesh instance, or invalid when not a mesh.")
        .def_prop_ro("is_mesh", [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld || !ecs::isValid(self.entity))
                    return false;
                return entityWorld->world().tryGet<scene::MeshInstanceComponent>(self.entity) != nullptr;
            })
        .def_prop_ro("is_camera", [](PySceneEntity& self) {
                return EntityIsCamera(self);
            }, "True when this entity has a CameraComponent.")
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
                const dm::float3 color = ToFloat3(v);
                SetLightProperty(self, "color", dm::float4(color.x, color.y, color.z, 0.f));
            },
            "Light color when this entity has a light component.")
        .def_prop_rw("intensity",
            [](PySceneEntity& self) {
                const float* intensity = TryMutableLightIntensity(self);
                return intensity ? *intensity : 0.f;
            },
            [](PySceneEntity& self, nb::object v) {
                SetLightProperty(self, "intensity", dm::float4(nb::cast<float>(v), 0.f, 0.f, 0.f));
            },
            "Point/spot intensity, or emitted radiance multiplier for a rectangular light.")
        .def_prop_rw("width",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* rect = entityWorld ? scene::tryGetRectLight(entityWorld->world(), self.entity) : nullptr;
                return rect ? rect->width : 0.f;
            },
            [](PySceneEntity& self, float value) {
                SetLightProperty(self, "width", dm::float4(value, 0.f, 0.f, 0.f));
            }, "RectLight width in local X.")
        .def_prop_rw("height",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* rect = entityWorld ? scene::tryGetRectLight(entityWorld->world(), self.entity) : nullptr;
                return rect ? rect->height : 0.f;
            },
            [](PySceneEntity& self, float value) {
                SetLightProperty(self, "height", dm::float4(value, 0.f, 0.f, 0.f));
            }, "RectLight height in local Y.")
        .def_prop_rw("irradiance",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* directional = entityWorld
                    ? scene::tryGetDirectionalLight(entityWorld->world(), self.entity) : nullptr;
                return directional ? directional->irradiance : 0.f;
            },
            [](PySceneEntity& self, nb::object v) {
                SetLightProperty(self, "irradiance", dm::float4(nb::cast<float>(v), 0.f, 0.f, 0.f));
            })
        .def_prop_rw("angular_size",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* directional = entityWorld
                    ? scene::tryGetDirectionalLight(entityWorld->world(), self.entity) : nullptr;
                return directional ? directional->angularSize : 0.f;
            },
            [](PySceneEntity& self, nb::object v) {
                SetLightProperty(self, "angularSize", dm::float4(nb::cast<float>(v), 0.f, 0.f, 0.f));
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
                SetLightProperty(self, "radius", dm::float4(nb::cast<float>(v), 0.f, 0.f, 0.f));
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
                SetLightProperty(self, "range", dm::float4(nb::cast<float>(v), 0.f, 0.f, 0.f));
            })
        .def_prop_rw("inner_angle",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* spot = entityWorld ? scene::tryGetSpotLight(entityWorld->world(), self.entity) : nullptr;
                return spot ? spot->innerAngle : 0.f;
            },
            [](PySceneEntity& self, nb::object v) {
                SetLightProperty(self, "innerAngle", dm::float4(nb::cast<float>(v), 0.f, 0.f, 0.f));
            })
        .def_prop_rw("outer_angle",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* spot = entityWorld ? scene::tryGetSpotLight(entityWorld->world(), self.entity) : nullptr;
                return spot ? spot->outerAngle : 0.f;
            },
            [](PySceneEntity& self, nb::object v) {
                SetLightProperty(self, "outerAngle", dm::float4(nb::cast<float>(v), 0.f, 0.f, 0.f));
            })
        .def_prop_rw("environment_path",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const auto* environment = entityWorld
                    ? scene::tryGetEnvironmentLight(entityWorld->world(), self.entity) : nullptr;
                return environment ? environment->path : std::string{};
            },
            [](PySceneEntity& self, nb::object v) {
                SetEnvironmentLightPath(self, nb::cast<std::string>(v));
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
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld)
                    throw std::runtime_error("position setter failed for stale or invalid SceneEntity");
                scene::EntityPose pose;
                if (!scene::getEntityWorldPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("position setter failed: entity has no world pose");
                pose.position = ToDouble3(v);
                if (!scene::setEntityWorldPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("position setter failed");
            },
            "World-space position (updates local translation). Lights and cameras.")
        .def_prop_rw("direction",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (entityWorld && EntityIsCamera(self))
                {
                    scene::CameraWorldLookTo look;
                    if (scene::tryGetCameraWorldLookTo(*entityWorld, self.entity, look))
                        return Float3ToTuple(look.direction);
                }
                const auto* global = entityWorld
                    ? entityWorld->world().tryGet<scene::GlobalTransformComponent>(self.entity) : nullptr;
                return Double3ToTuple(global ? scene::getLightDirection(global->transform) : double3(0.0, -1.0, 0.0));
            },
            [](PySceneEntity& self, nb::object v) {
                scene::SceneEntityWorld& entityWorld = RequireEntityWorld(self, "direction");
                if (EntityIsCamera(self))
                {
                    scene::CameraWorldLookTo look;
                    if (!scene::tryGetCameraWorldLookTo(entityWorld, self.entity, look))
                        throw std::runtime_error("direction setter failed: camera pose is unavailable");
                    const dm::float3 dir = ToFloat3(v);
                    if (!setSceneCameraLookTo(RequireEntityApp(self), self.entity, look.position, dir, look.up))
                        throw std::runtime_error("direction setter failed");
                    return;
                }
                if (EntityLightType(self) == LightType_None)
                    throw std::runtime_error("SceneEntity direction setter failed: entity has no light component");
                scene::setLightWorldDirection(entityWorld, self.entity, ToDouble3(v));
            },
            "World-space look direction. Cameras keep the current up; lights use lookatZ.")
        .def_prop_rw("camera_pose",
            [](PySceneEntity& self) {
                RequireCamera(self);
                scene::CameraWorldLookTo look;
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld || !scene::tryGetCameraWorldLookTo(*entityWorld, self.entity, look))
                    throw std::runtime_error("camera_pose unavailable");
                return nb::make_tuple(
                    Float3ToTuple(look.position),
                    Float3ToTuple(look.direction),
                    Float3ToTuple(look.up));
            },
            [](PySceneEntity& self, nb::object value) {
                RequireCamera(self);
                const CameraPose pose = CameraPoseFromPython(value);
                if (!setSceneCameraLookTo(
                    RequireEntityApp(self), self.entity,
                    pose.position, pose.direction, pose.up))
                {
                    throw std::runtime_error("camera_pose setter failed");
                }
            },
            "Typed camera pose: (position.xyz, direction.xyz, up.xyz), all in world space.")
        .def_prop_rw("translation",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld)
                    return Double3ToTuple(double3(0.0));
                const auto* local = entityWorld->world().get<scene::LocalTransformComponent>(self.entity);
                return Double3ToTuple(local ? local->translation : double3(0.0));
            },
            [](PySceneEntity& self, nb::object v) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                scene::EntityPose pose;
                if (!entityWorld || !scene::getEntityLocalPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("translation setter failed");
                pose.position = ToDouble3(v);
                if (!scene::setEntityLocalPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("translation setter failed");
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
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                scene::EntityPose pose;
                if (!entityWorld || !scene::getEntityLocalPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("rotation setter failed");
                pose.rotation = ToDQuatXYZW(v);
                if (!scene::setEntityLocalPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("rotation setter failed");
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
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                scene::EntityPose pose;
                if (!entityWorld || !scene::getEntityLocalPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("euler setter failed");
                pose.rotation = caustica::math::rotationQuat(ToDouble3(v));
                if (!scene::setEntityLocalPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("euler setter failed");
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
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                scene::EntityPose pose;
                if (!entityWorld || !scene::getEntityLocalPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("scaling setter failed");
                pose.scaling = ToDouble3(v);
                if (!scene::setEntityLocalPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("scaling setter failed");
            },
            "Local non-uniform scaling.")
        .def_prop_ro("local_pose",
            [](PySceneEntity& self) {
                scene::EntityPose pose;
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld || !scene::getEntityLocalPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("local_pose unavailable for stale or invalid SceneEntity");
                return EntityPoseToTuple(pose);
            },
            "Local entity TRS: ((x,y,z), (x,y,z,w), (sx,sy,sz)). Prefer this over assigning translation/rotation/scaling separately.")
        .def_prop_rw("world_pose",
            [](PySceneEntity& self) {
                scene::EntityPose pose;
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld || !scene::getEntityWorldPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("world_pose unavailable for stale or invalid SceneEntity");
                return EntityPoseToTuple(pose);
            },
            [](PySceneEntity& self, nb::object value) {
                nb::sequence pose = nb::cast<nb::sequence>(value);
                if (nb::len(pose) != 3)
                    throw std::runtime_error("world_pose must be (position, rotation, scaling)");
                const scene::EntityPose parsed = EntityPoseFromPython(
                    nb::borrow<nb::object>(pose[0]),
                    nb::borrow<nb::object>(pose[1]),
                    nb::borrow<nb::object>(pose[2]));
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld || !scene::setEntityWorldPose(*entityWorld, self.entity, parsed))
                    throw std::runtime_error("world_pose setter failed");
            },
            "World entity TRS: ((x,y,z), (x,y,z,w), (sx,sy,sz)). Entity space, no camera Z-flip — aim cameras with look_to.")
        .def("set_local_pose",
            [](PySceneEntity& self, nb::object position, nb::object rotation, nb::object scaling) {
                const scene::EntityPose pose = EntityPoseFromPython(position, rotation, scaling);
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld || !scene::setEntityLocalPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("set_local_pose failed");
            },
            nb::arg("position"), nb::arg("rotation"), nb::arg("scaling") = nb::make_tuple(1.0, 1.0, 1.0),
            "Write local TRS in one hierarchy refresh. Prefer this over assigning translation/rotation/scaling separately.")
        .def("set_world_pose",
            [](PySceneEntity& self, nb::object position, nb::object rotation, nb::object scaling) {
                const scene::EntityPose pose = EntityPoseFromPython(position, rotation, scaling);
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                if (!entityWorld || !scene::setEntityWorldPose(*entityWorld, self.entity, pose))
                    throw std::runtime_error("set_world_pose failed");
            },
            nb::arg("position"), nb::arg("rotation"), nb::arg("scaling") = nb::make_tuple(1.0, 1.0, 1.0),
            "Write world entity TRS through the parent. Not camera view space — aim cameras with look_to.")
        .def_prop_rw("vertical_fov",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                return entityWorld ? scene::getCameraVerticalFov(*entityWorld, self.entity) : 0.f;
            },
            [](PySceneEntity& self, float radians) {
                RequireCamera(self);
                if (!setSceneCameraVerticalFOV(RequireEntityApp(self), self.entity, radians))
                    throw std::runtime_error("vertical_fov setter failed");
            },
            "Perspective vertical FOV in radians. Assigning this clears custom pinhole intrinsics.")
        .def_prop_rw("z_near",
            [](PySceneEntity& self) {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                return entityWorld ? scene::getCameraZNear(*entityWorld, self.entity) : 0.f;
            },
            [](PySceneEntity& self, float zNear) {
                RequireCamera(self);
                if (!setSceneCameraZNear(RequireEntityApp(self), self.entity, zNear))
                    throw std::runtime_error("z_near setter failed");
            },
            "Near clip plane.")
        .def_prop_rw("z_far",
            [](PySceneEntity& self) -> std::optional<float> {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                return entityWorld ? scene::getCameraZFar(*entityWorld, self.entity) : std::nullopt;
            },
            [](PySceneEntity& self, std::optional<float> zFar) {
                RequireCamera(self);
                if (!setSceneCameraZFar(RequireEntityApp(self), self.entity, zFar))
                    throw std::runtime_error("z_far setter failed");
            },
            "Far clip plane, or None when unbounded.")
        .def_prop_rw("aspect_ratio",
            [](PySceneEntity& self) -> std::optional<float> {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                return entityWorld ? scene::getCameraAspectRatio(*entityWorld, self.entity) : std::nullopt;
            },
            [](PySceneEntity& self, std::optional<float> aspectRatio) {
                RequireCamera(self);
                if (!setSceneCameraAspectRatio(RequireEntityApp(self), self.entity, aspectRatio))
                    throw std::runtime_error("aspect_ratio setter failed");
            },
            "Optional aspect ratio. None lets the render target decide.")
        .def_prop_ro("intrinsics",
            [](PySceneEntity& self) -> nb::object {
                scene::SceneEntityWorld* entityWorld = self.entityWorld();
                const scene::CameraIntrinsics* k = entityWorld
                    ? scene::tryGetCameraIntrinsics(*entityWorld, self.entity) : nullptr;
                if (!k)
                    return nb::none();
                return nb::make_tuple(k->fx, k->fy, k->cx, k->cy, k->width, k->height);
            },
            "Pinhole `(fx, fy, cx, cy, width, height)` or None when using symmetric FOV.")
        .def("look_to",
            [](PySceneEntity& self, nb::object position, nb::object direction, nb::object up) {
                RequireCamera(self);
                const dm::float3 pos = ToFloat3(position);
                const dm::float3 dir = ToFloat3(direction);
                const dm::float3 upVec = ToFloat3(up);
                if (!setSceneCameraLookTo(RequireEntityApp(self), self.entity, pos, dir, upVec))
                    throw std::runtime_error("look_to failed");
            },
            nb::arg("position"), nb::arg("direction"),
            nb::arg("up") = nb::make_tuple(0.0f, 1.0f, 0.0f),
            "Aim this camera in view space (renderer Z-flip). Do not copy a mesh world_pose here.")
        .def("set_intrinsics",
            [](PySceneEntity& self, float fx, float fy, float cx, float cy, float width, float height) {
                RequireCamera(self);
                if (!setSceneCameraIntrinsics(RequireEntityApp(self), self.entity, fx, fy, cx, cy, width, height))
                    throw std::runtime_error("set_intrinsics failed (need a perspective camera and finite positive fx,fy,width,height)");
            },
            nb::arg("fx"), nb::arg("fy"), nb::arg("cx"), nb::arg("cy"),
            nb::arg("width"), nb::arg("height"),
            "Off-center pinhole. Overrides symmetric vertical_fov until cleared.")
        .def("clear_intrinsics",
            [](PySceneEntity& self) {
                RequireCamera(self);
                if (!clearSceneCameraIntrinsics(RequireEntityApp(self), self.entity))
                    throw std::runtime_error("clear_intrinsics failed");
            },
            "Restore symmetric vertical_fov projection.")
        .def("activate",
            [](PySceneEntity& self) {
                RequireCamera(self);
                if (!setActiveCamera(RequireEntityApp(self), self.entity))
                    throw std::runtime_error("SceneEntity is not a registered scene camera");
            },
            "Make this the rendered / main camera.")
        .def_prop_rw("instance_id",
            [](PySceneEntity& self) {
                return entityInstanceId(RequireEntityApp(self), self.entity);
            },
            [](PySceneEntity& self, uint32_t value) {
                App& app = RequireEntityApp(self);
                uint32_t semantic = 0;
                std::string label;
                if (ecs::World* world = sceneEcs(app))
                {
                    if (const auto* component = world->tryGet<scene::SemanticLabelComponent>(self.entity))
                    {
                        semantic = component->semanticId;
                        label = component->semanticLabel;
                    }
                }
                if (!setEntitySemanticLabel(app, self.entity, value, semantic, std::move(label)))
                    throw std::runtime_error("instance_id setter failed");
            },
            "Stable instance id written into the instance_id AOV. 0 auto-hashes authoring id / path.")
        .def_prop_rw("semantic_id",
            [](PySceneEntity& self) {
                return entitySemanticId(RequireEntityApp(self), self.entity);
            },
            [](PySceneEntity& self, uint32_t value) {
                App& app = RequireEntityApp(self);
                uint32_t instance = 0;
                std::string label;
                if (ecs::World* world = sceneEcs(app))
                {
                    if (const auto* component = world->tryGet<scene::SemanticLabelComponent>(self.entity))
                    {
                        instance = component->instanceId;
                        label = component->semanticLabel;
                    }
                }
                if (!setEntitySemanticLabel(app, self.entity, instance, value, std::move(label)))
                    throw std::runtime_error("semantic_id setter failed");
            },
            "Stable class id written into the semantic_id AOV. 0 = unlabeled unless semantic_label is set.")
        .def_prop_rw("semantic_label",
            [](PySceneEntity& self) {
                return entitySemanticLabel(RequireEntityApp(self), self.entity);
            },
            [](PySceneEntity& self, const std::string& value) {
                App& app = RequireEntityApp(self);
                uint32_t instance = 0;
                uint32_t semantic = 0;
                if (ecs::World* world = sceneEcs(app))
                {
                    if (const auto* component = world->tryGet<scene::SemanticLabelComponent>(self.entity))
                    {
                        instance = component->instanceId;
                        semantic = component->semanticId;
                    }
                }
                if (!setEntitySemanticLabel(app, self.entity, instance, semantic, value))
                    throw std::runtime_error("semantic_label setter failed");
            },
            "Optional class name. Hashed into semantic_id when semantic_id is 0.")
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
                return std::string("<caustica.SceneEntity '") + name + "'>";
            });

    // --- Scene ------------------------------------------------------------
    nb::class_<PyScene>(m, "Scene",
        "Loaded caustica scene. Materials, lights, and SceneEntity lookup live here.\n"
        "Prefer Sample.find_entity / get_mesh_entities over digging engine MeshInfo.")
        .def("get_materials", [](PyScene& self) {
                return GetSceneMaterials(self.scene.get(), self.owner);
            }, "Return every StandardMaterial in this scene.")

        .def("find_material", [](PyScene& self, const std::string& name) {
                return FindSceneMaterial(self.scene.get(), self.owner, name);
            }, nb::arg("name"), "Look up a material by Name or uniqueName.")

        .def("find_material_by_id", [](PyScene& self, int materialId) {
                return FindSceneMaterialById(self.scene.get(), self.owner, materialId);
            }, nb::arg("material_id"),
            "Look up by gpuDataIndex only. Prefer EngineApp.find_material (cache-backed).")

        .def("get_lights", [](PyScene& self) {
                return GetSceneLights(self.scene, self.owner);
            }, "Return every light entity as SceneEntity.")

        .def("find_light", [](PyScene& self, const std::string& name) {
                return FindSceneLight(self.scene, self.owner, name);
            }, nb::arg("name"), "Look up a light entity by name; returns SceneEntity or None.")
        .def("get_cameras", [](PyScene& self) {
                return GetSceneCameras(self.scene, self.owner);
            }, "Return every camera entity as SceneEntity.")
        .def("find_camera", [](PyScene& self, const std::string& name) {
                return FindSceneCamera(self.scene, self.owner, name);
            }, nb::arg("name"), "Look up a camera entity by name; returns SceneEntity or None.")
        .def("find_entity", [](PyScene& self, const std::string& path) {
                return FindSceneEntity(self.scene, self.owner, path);
            }, nb::arg("path"), "Look up a scene entity by name or path.")
        .def("get_mesh_entities", [](PyScene& self) {
                return GetSceneMeshEntities(self.scene, self.owner);
            }, "Return every mesh-instance entity as SceneEntity.")
        .def("find_mesh_entity", [](PyScene& self, const std::string& name) {
                return FindSceneMeshEntity(self.scene, self.owner, name);
            }, nb::arg("name"),
            "Look up a mesh-instance entity by MeshInfo name or entity name.")

        .def_prop_ro("material_count", [](PyScene& self) {
                return GetSceneMaterials(self.scene.get(), self.owner).size();
            }, "Number of StandardMaterial instances in this scene.")
        .def_prop_ro("mesh_count", [](PyScene& self) {
                return self.scene ? self.scene->getMeshes().size() : 0;
            }, "Number of meshes in this scene.")
        .def_prop_ro("light_count", [](PyScene& self) {
                return self.scene ? self.scene->getLightEntities().size() : 0;
            }, "Number of lights in this scene.")
        .def_prop_ro("camera_count", [](PyScene& self) {
                return self.scene ? self.scene->getCameraEntities().size() : 0;
            }, "Number of camera entities in this scene (does not include the free / controller camera).")

        .def_prop_ro("bounds", [](PyScene& self) {
                return SceneBoundsTuple(SceneBoundsFromScene(self.scene));
            },
            "World-space axis-aligned bounding box that covers every renderable\n"
            "leaf in the scene (mesh instances, lights, splats, ...).\n"
            "Returns ``((min_x, min_y, min_z), (max_x, max_y, max_z))`` or\n"
            "``None`` when the scene is empty / not refreshed yet.")
        .def_prop_ro("bounds_center", [](PyScene& self) {
                return SceneBoundsCenter(SceneBoundsFromScene(self.scene));
            },
            "Center point of `Scene.bounds`, or ``None`` for an empty scene.")
        .def_prop_ro("bounds_size", [](PyScene& self) {
                return SceneBoundsSize(SceneBoundsFromScene(self.scene));
            },
            "Diagonal extent (max - min) of `Scene.bounds`, or ``None`` for an empty scene.")

        .def("__repr__", [](PyScene& self) {
                const auto materialCount = GetSceneMaterials(self.scene.get(), self.owner).size();
                const auto lightCount = self.scene ? self.scene->getLightEntities().size() : 0;
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
        .def_rw("visible_to_camera", &EnvironmentMapRuntimeParameters::VisibleToCamera);

    nb::class_<ToneMappingParameters>(m, "ToneMappingParams",
        "Tone-mapping and exposure settings. Access the live instance through\n"
        "``engine.settings.tone_mapping_params``.")
        .def(nb::init<>())
        .def_rw("exposure_mode", &ToneMappingParameters::exposureMode)
        .def_rw("tone_map_operator", &ToneMappingParameters::toneMapOperator)
        .def_rw("auto_exposure", &ToneMappingParameters::autoExposure)
        .def_rw("exposure_compensation", &ToneMappingParameters::exposureCompensation)
        .def_rw("exposure_value", &ToneMappingParameters::exposureValue)
        .def_rw("film_speed", &ToneMappingParameters::filmSpeed)
        .def_rw("f_number", &ToneMappingParameters::fNumber)
        .def_rw("shutter", &ToneMappingParameters::shutter)
        .def_rw("white_balance", &ToneMappingParameters::whiteBalance)
        .def_rw("white_point", &ToneMappingParameters::whitePoint)
        .def_rw("white_max_luminance", &ToneMappingParameters::whiteMaxLuminance)
        .def_rw("white_scale", &ToneMappingParameters::whiteScale)
        .def_rw("clamped", &ToneMappingParameters::clamped)
        .def_rw("exposure_value_min", &ToneMappingParameters::exposureValueMin)
        .def_rw("exposure_value_max", &ToneMappingParameters::exposureValueMax)
        .def_rw("camera_lut_enabled", &ToneMappingParameters::cameraLutEnabled)
        .def_rw("camera_lut_after_tone_map", &ToneMappingParameters::cameraLutAfterToneMap)
        .def_rw("camera_lut_preset", &ToneMappingParameters::cameraLutPreset)
        .def_prop_ro("camera_lut_is_3d", [](ToneMappingParameters& s) { return s.cameraLutIs3D; })
        .def_prop_rw("camera_lut_domain_min",
            [](ToneMappingParameters& s) { return Float3ToTuple(s.cameraLutDomainMin); },
            [](ToneMappingParameters& s, nb::object v) { s.cameraLutDomainMin = ToFloat3(v); })
        .def_prop_rw("camera_lut_domain_max",
            [](ToneMappingParameters& s) { return Float3ToTuple(s.cameraLutDomainMax); },
            [](ToneMappingParameters& s, nb::object v) { s.cameraLutDomainMax = ToFloat3(v); })
        .def_prop_ro("camera_lut_path", [](ToneMappingParameters& s) { return s.cameraLutPath; })
        .def("load_camera_lut", [](ToneMappingParameters& s, const std::string& path) {
            std::string error;
            if (!s.loadCameraLut(path, &error))
                throw std::runtime_error(error);
        }, nb::arg("path"), "Load and enable a 1D .cube camera LUT.")
        .def("apply_camera_lut_preset", &ToneMappingParameters::applyCameraLutPreset,
            nb::arg("preset"), "Apply a built-in optional 1D camera-LUT look.")
        .def("clear_camera_lut", &ToneMappingParameters::clearCameraLut,
            "Disable the camera LUT and restore its identity table.");

    nb::class_<PathTracerSettings>(m, "Settings",
        "Live renderer session state (path tracer settings and runtime flags).\n"
        "Mutating attributes is equivalent to moving the corresponding ImGui widget.")
        .def_rw("enable_animations",             &PathTracerSettings::EnableAnimations)
        .def_rw("enable_keyframes",              &PathTracerSettings::EnableKeyframes)
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
                "True for realtime mode, False for reference / accumulation mode.")
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
        .def_rw("tone_mapping_params",           &PathTracerSettings::ToneMappingParams,
                "Live caustica.ToneMappingParams object used by the tone-mapping pass.")
        .def("load_camera_lut", [](PathTracerSettings& s, const std::string& path) {
            std::string error;
            if (!s.ToneMappingParams.loadCameraLut(path, &error))
                throw std::runtime_error(error);
        }, nb::arg("path"), "Load and enable a 1D .cube camera LUT.")
        .def("clear_camera_lut", [](PathTracerSettings& s) {
            s.ToneMappingParams.clearCameraLut();
        })
        .def_rw("enable_bloom",                  &PathTracerSettings::EnableBloom)
        .def_rw("bloom_intensity",               &PathTracerSettings::BloomIntensity)
        .def_rw("bloom_radius",                  &PathTracerSettings::BloomRadius)

        .def_rw("enable_gaussian_splats",        &PathTracerSettings::EnableGaussianSplats)
        .def_rw("gaussian_splat_depth_test",     &PathTracerSettings::GaussianSplatDepthTest)
        .def_rw("gaussian_splat_depth_bias",     &PathTracerSettings::GaussianSplatDepthBias,
                "Relative reverse-Z bias for stable mesh/3DGS intersections.")
        .def_rw("gaussian_splat_depth_edge_dilation", &PathTracerSettings::GaussianSplatDepthEdgeDilation,
                "Use one-pixel conservative mesh depth at silhouettes to reduce temporal flicker.")
        .def_rw("gaussian_splat_primary_method", &PathTracerSettings::GaussianSplatPrimaryMethod,
                "Primary color path (caustica.GaussianSplatPrimaryMethod): GS=3DGS, GUT=3DGUT.")
        .def_rw("gaussian_splat_shadows",        &PathTracerSettings::GaussianSplatShadows)
        .def_rw("gaussian_splat_shadows_mode",   &PathTracerSettings::GaussianSplatShadowsMode,
                "Mesh shadow mode (caustica.GaussianSplatShadowMode). Orthogonal to primary 3DGS/3DGUT.")
        .def_rw("gaussian_splat_sorting_mode",   &PathTracerSettings::GaussianSplatSortingMode,
                "3DGS sort mode (caustica.GaussianSplatSortMode).")
        .def_rw("gaussian_splat_sh_format",      &PathTracerSettings::GaussianSplatSHFormat,
                "3DGS SH storage format (caustica.GaussianSplatStorageFormat).")
        .def_rw("gaussian_splat_rgba_format",    &PathTracerSettings::GaussianSplatRGBAFormat,
                "3DGS RGBA storage format (caustica.GaussianSplatStorageFormat).")
        .def_rw("gaussian_splat_use_aabbs",      &PathTracerSettings::GaussianSplatUseAABBs)
        .def_rw("gaussian_splat_use_tlas_instances", &PathTracerSettings::GaussianSplatUseTLASInstances)
        .def_rw("gaussian_splat_blas_compaction", &PathTracerSettings::GaussianSplatBlasCompaction)
        .def_rw("gaussian_splat_shadow_kernel_degree", &PathTracerSettings::GaussianSplatShadowKernelDegree)
        .def_rw("gaussian_splat_shadow_adaptive_clamp", &PathTracerSettings::GaussianSplatShadowAdaptiveClamp)
        .def_rw("gaussian_splat_shadow_ray_offset", &PathTracerSettings::GaussianSplatShadowRayOffset)
        .def_rw("gaussian_splat_projection_method", &PathTracerSettings::GaussianSplatProjectionMethod,
                "3DGUT extent method: 0=oriented Eigen, 1=paper/reference Conic. Ignored by 3DGS.")
        .def_rw("gaussian_splat_covariance_dilation", &PathTracerSettings::GaussianSplatCovarianceDilation,
                "Screen-space covariance low-pass kernel size; typically 0.1 or 0.3.")
        .def_rw("gaussian_splat_mip_antialiasing", &PathTracerSettings::GaussianSplatMipAntialiasing)
        .def_rw("gaussian_splat_reference_gamma_compositing",
                &PathTracerSettings::GaussianSplatReferenceGammaCompositing,
                "Use vk_gaussian_splatting-compatible sRGB alpha compositing for GPU-sorted splats.")
        .def_rw("gaussian_splat_apply_tone_mapping",
                &PathTracerSettings::GaussianSplatApplyToneMapping,
                "Composite Gaussian splats before tone mapping; disable to composite them into the post-tone-map LDR target.")
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
}

void BindEngineApp(nb::class_<PyEngineApp>& cls)
{

    cls
        .def_prop_ro("valid", &PyEngineApp::isValid)
        .def("shutdown", &PyEngineApp::shutdown)
        .def("run", [](PyEngineApp& self) { self.engine().run(); })
        .def("request_exit", [](PyEngineApp& self) { self.engine().requestExit(); })
        .def("step_frame", &PyEngineApp::step,
             nb::arg("dt") = -1.0f,
             "Advance one frame. dt < 0 uses the engine clock (1/60 in headless).")
        .def("step_n", &PyEngineApp::stepN, nb::arg("frames"),
             "Python sugar: call step_frame() N times.")
        .def("step_until_accumulated", &PyEngineApp::stepUntilAccumulated,
             nb::arg("max_frames") = 0,
             "Python sugar: step until accumulation_completed.")
        .def("wait_until_ready",
             [](PyEngineApp& self, double timeoutSeconds, int warmupFrames) {
                 return self.engine().waitUntilReady(timeoutSeconds, warmupFrames);
             },
             nb::arg("timeout_seconds") = 600.0, nb::arg("warmup_frames") = 4)

        .def_prop_ro("is_scene_loaded", [](PyEngineApp& self) { return self.engine().isSceneLoaded(); })
        .def_prop_ro("is_scene_loading", [](PyEngineApp& self) { return self.engine().isSceneLoading(); })
        .def_prop_ro("is_scene_ready", [](PyEngineApp& self) { return self.engine().isSceneReady(); })
        .def("set_scene",
             [](PyEngineApp& self, const std::string& name, bool forceReload) {
                 self.engine().setScene(name, forceReload);
             },
             nb::arg("scene_name"), nb::arg("force_reload") = false)

        .def_prop_ro("settings",
             [](PyEngineApp& self) -> PathTracerSettings* { return &self.engine().settings(); },
             nb::rv_policy::reference,
             "Live PathTracerSettings (same object as C++ EngineApp::settings()).")
        .def_prop_ro("scene",
             [](PyEngineApp& self) {
                 return MakePyScene(caustica::activeScene(RequirePyApp(self)), self.context());
             },
             "Read-only Scene view, or None before a scene is available.")
        .def_prop_ro("scene_name", [](PyEngineApp& self) { return self.engine().currentSceneName(); })
        .def_prop_ro("available_scenes", [](PyEngineApp& self) { return self.engine().availableScenes(); })

        .def("load_gaussian_splat_file",
             [](PyEngineApp& self, const std::string& fileName, bool convertRdfToRub) {
                 return self.engine().loadGaussianSplatFile(fileName, convertRdfToRub);
             },
             nb::arg("file_name"), nb::arg("convert_rdf_to_rub") = true)
        .def_prop_ro("gaussian_splat_count", [](PyEngineApp& self) { return self.engine().gaussianSplatCount(); })
        .def_prop_ro("gaussian_splat_object_count", [](PyEngineApp& self) { return self.engine().gaussianSplatObjectCount(); })
        .def_prop_ro("gaussian_splat_file_name", [](PyEngineApp& self) { return self.engine().gaussianSplatFileName(); })

        .def("find_entity", [](PyEngineApp& self, const std::string& path) {
                return FindSceneEntity(RequirePyScene(self), self.context(), path);
            }, nb::arg("path"))
        .def("find_material", [](PyEngineApp& self, int materialId) {
                return StandardMaterial::safeCast(self.engine().findMaterial(materialId));
            }, nb::arg("material_id"))

        .def("set_env_map_override_source", [](PyEngineApp& self, const std::string& path) {
                self.engine().setEnvMapOverrideSource(path);
            }, nb::arg("path"))

        .def("set_camera_pos_dir_up",
             [](PyEngineApp& self, nb::object pos, nb::object dir, nb::object up) {
                 if (!self.engine().setCameraPosDirUp(ToFloat3(pos), ToFloat3(dir), ToFloat3(up)))
                     throw std::runtime_error("set_camera_pos_dir_up failed");
             },
             nb::arg("position"), nb::arg("direction"),
             nb::arg("up") = nb::make_tuple(0.0f, 1.0f, 0.0f))
        .def_prop_ro("current_camera_pos_dir_up", [](PyEngineApp& self) {
                return self.engine().currentCameraPosDirUp();
            })
        .def_prop_rw("camera_pose",
             [](PyEngineApp& self) { return CameraPoseToTuple(self.engine().currentCameraPose()); },
             [](PyEngineApp& self, nb::object value) {
                 if (!self.engine().setCameraPose(CameraPoseFromPython(value)))
                     throw std::runtime_error("camera_pose setter failed");
             },
             "Typed active-camera pose: (position.xyz, direction.xyz, up.xyz), all in world space.")
        .def("set_camera_pose",
             [](PyEngineApp& self, nb::object position, nb::object direction, nb::object up) {
                 CameraPose pose{
                     ToFloat3(position), ToFloat3(direction), ToFloat3(up) };
                 if (!self.engine().setCameraPose(pose))
                     throw std::runtime_error("set_camera_pose failed");
             },
             nb::arg("position"), nb::arg("direction"),
             nb::arg("up") = nb::make_tuple(0.0f, 1.0f, 0.0f),
             "Set the typed active-camera pose in world space.")
        .def("set_camera_vertical_fov", [](PyEngineApp& self, float radians) {
                if (!self.engine().setCameraVerticalFOV(radians))
                    throw std::runtime_error("set_camera_vertical_fov failed");
            }, nb::arg("radians"))
        .def_prop_ro("camera_vertical_fov", [](PyEngineApp& self) {
                return self.engine().cameraVerticalFOV();
            })
        .def("set_camera_intrinsics",
             [](PyEngineApp& self, float fx, float fy, float cx, float cy, float width, float height) {
                 if (!self.engine().setCameraIntrinsics(fx, fy, cx, cy, width, height))
                     throw std::runtime_error("set_camera_intrinsics failed");
             },
             nb::arg("fx"), nb::arg("fy"), nb::arg("cx"), nb::arg("cy"),
             nb::arg("width"), nb::arg("height"))
        .def("clear_camera_intrinsics", [](PyEngineApp& self) {
                if (!self.engine().clearCameraIntrinsics())
                    throw std::runtime_error("clear_camera_intrinsics failed");
            })
        .def_prop_ro("scene_camera_count", [](PyEngineApp& self) { return self.engine().sceneCameraCount(); })
        .def_prop_rw("selected_camera_index",
             [](PyEngineApp& self) { return self.engine().selectedCameraIndex(); },
             [](PyEngineApp& self, unsigned int index) {
                 if (!self.engine().setSelectedCameraIndex(index))
                     throw std::runtime_error("selected_camera_index is out of range");
             },
             "0 is the free / controller camera. 1..N are scene cameras from get_cameras() order.")
        .def_prop_ro("active_camera",
             [](PyEngineApp& self) -> std::shared_ptr<PySceneEntity> {
                 const ecs::Entity entity = self.engine().activeCameraEntity();
                 if (!ecs::isValid(entity))
                     return nullptr;
                  return PyEntityFromEntity(RequirePyScene(self), self.context(), entity);
             },
             "Scene camera currently driving the renderer, or None for the free camera.")
        .def_prop_ro("active_camera_is_free", [](PyEngineApp& self) {
                 return self.engine().activeCameraIsFree();
             })
        .def_prop_ro("active_camera_path", [](PyEngineApp& self) {
                 return self.engine().activeCameraPath();
             })
        .def_prop_ro("active_camera_name", [](PyEngineApp& self) {
                 return self.engine().activeCameraName();
             })
        .def("use_camera",
               [](PyEngineApp& self, const std::shared_ptr<PySceneEntity>& entity) {
                   if (entity)
                   {
                       RequireEntityForApp(self, entity);
                       if (!self.engine().setActiveCamera(EntityFromPy(entity)))
                           throw std::runtime_error("use_camera failed: entity is not a selectable perspective camera");
                       return;
                   }
                   if (!self.engine().setActiveCamera(ecs::NullEntity))
                       throw std::runtime_error("use_camera(None) failed");
               },
             nb::arg("entity").none(),
             "Select a scene camera, or None to return to the free / controller camera.")
        .def("use_camera_path",
             [](PyEngineApp& self, const std::string& path) {
                 if (!self.engine().setActiveCameraByPath(path))
                     throw std::runtime_error("use_camera_path failed: path is not a registered scene camera");
             },
             nb::arg("path"),
             "Select a registered scene camera by stable hierarchy path.")
        .def("save_current_camera", [](PyEngineApp& self) { self.engine().saveCurrentCamera(); })
        .def("load_current_camera", [](PyEngineApp& self) { self.engine().loadCurrentCamera(); })

        .def("add_render_product",
             [](PyEngineApp& self, const std::string& name, const std::shared_ptr<PySceneEntity>& camera, uint32_t aovs) {
                 RenderProductDesc desc;
                 desc.name = name;
                 desc.camera = camera ? EntityFromPy(camera) : ecs::NullEntity;
                 desc.aovs = aovs == 0u ? uint32_t(Aov::All) : aovs;
                 if (!self.engine().addRenderProduct(std::move(desc)))
                     throw std::runtime_error("add_render_product failed");
             },
             nb::arg("name"),
             nb::arg("camera").none() = std::shared_ptr<PySceneEntity>{},
             nb::arg("aovs") = uint32_t(Aov::All),
             "Register a named camera + AOV set. camera=None uses the active camera.")
        .def("remove_render_product",
             [](PyEngineApp& self, const std::string& name) {
                 if (!self.engine().removeRenderProduct(name))
                     throw std::runtime_error("remove_render_product failed");
             },
             nb::arg("name"))
        .def("clear_render_products", [](PyEngineApp& self) { self.engine().clearRenderProducts(); })
        .def("read_sensor_output",
             [](PyEngineApp& self, uint32_t aovs) {
                 auto output = self.engine().readSensorOutput(aovs);
                 if (!output)
                     throw std::runtime_error("read_sensor_output failed (call step_frame() first)");
                 return std::move(*output);
             },
             nb::arg("aovs") = uint32_t(Aov::All),
             "Read AOVs for the camera that was just rendered.")
        .def("capture_sensor_outputs",
             [](PyEngineApp& self) {
                 return self.engine().captureSensorOutputs();
             },
             "Capture every registered RenderProduct at the current physical time.")

        .def("load", [](PyEngineApp& self, const std::string& path) {
                return self.engine().load(path);
            }, nb::arg("path"))
        .def("spawn", [](PyEngineApp& self, const Handle<ScenePrefabAsset>& prefab) {
                return PyEntityFromEntity(RequirePyScene(self), self.context(), self.engine().spawn(prefab));
            }, nb::arg("prefab"))
        .def("spawn_from_file", [](PyEngineApp& self, const std::string& path) {
                return PyEntityFromEntity(RequirePyScene(self), self.context(), self.engine().spawnFromFile(path));
            }, nb::arg("path"))
        .def("spawn_from_source", [](PyEngineApp& self, const std::string& source) {
                return PyEntityFromEntity(RequirePyScene(self), self.context(), self.engine().spawnFromSource(source));
            }, nb::arg("source"))
        .def("despawn", [](PyEngineApp& self, const std::shared_ptr<PySceneEntity>& entity) {
                RequireEntityForApp(self, entity);
                return self.engine().despawn(EntityFromPy(entity));
            }, nb::arg("entity"))

        .def("spawn_directional_light",
             [](PyEngineApp& self, nb::object color, float irradiance, float angularSize, const std::string& name) {
                 scene::DirectionalLightComponent component;
                 component.color = ToFloat3(color);
                 component.irradiance = irradiance;
                 component.angularSize = angularSize;
                 std::shared_ptr<Scene> scene = RequirePyScene(self);
                 const std::string lightName = MakeUniqueLightName(scene, self.context(), name, "DirectionalLight");
                 return PyEntityFromEntity(scene, self.context(), self.engine().spawnDirectionalLight(std::move(component), lightName));
             },
             nb::arg("color") = nb::make_tuple(1.f, 1.f, 1.f),
             nb::arg("irradiance") = 1.f, nb::arg("angular_size") = 0.f, nb::arg("name") = std::string())
        .def("spawn_point_light",
             [](PyEngineApp& self, nb::object color, float intensity, float radius, float range, const std::string& name) {
                 scene::PointLightComponent component;
                 component.color = ToFloat3(color);
                 component.intensity = intensity;
                 component.radius = radius;
                 component.range = range;
                 std::shared_ptr<Scene> scene = RequirePyScene(self);
                 const std::string lightName = MakeUniqueLightName(scene, self.context(), name, "PointLight");
                 return PyEntityFromEntity(scene, self.context(), self.engine().spawnPointLight(std::move(component), lightName));
             },
             nb::arg("color") = nb::make_tuple(1.f, 1.f, 1.f),
             nb::arg("intensity") = 1.f, nb::arg("radius") = 0.f, nb::arg("range") = 0.f,
             nb::arg("name") = std::string())
        .def("spawn_spot_light",
             [](PyEngineApp& self, nb::object color, float intensity, float radius, float range,
                float innerAngle, float outerAngle, const std::string& name) {
                 scene::SpotLightComponent component;
                 component.color = ToFloat3(color);
                 component.intensity = intensity;
                 component.radius = radius;
                 component.range = range;
                 component.innerAngle = innerAngle;
                 component.outerAngle = outerAngle;
                 std::shared_ptr<Scene> scene = RequirePyScene(self);
                 const std::string lightName = MakeUniqueLightName(scene, self.context(), name, "SpotLight");
                 return PyEntityFromEntity(scene, self.context(), self.engine().spawnSpotLight(std::move(component), lightName));
             },
             nb::arg("color") = nb::make_tuple(1.f, 1.f, 1.f),
             nb::arg("intensity") = 1.f, nb::arg("radius") = 0.f, nb::arg("range") = 0.f,
             nb::arg("inner_angle") = 180.f, nb::arg("outer_angle") = 180.f, nb::arg("name") = std::string())
        .def("spawn_rect_light",
             [](PyEngineApp& self, nb::object color, float intensity, float width, float height, const std::string& name) {
                 scene::RectLightComponent component;
                 component.color = ToFloat3(color);
                 component.intensity = intensity;
                 component.width = width;
                 component.height = height;
                 std::shared_ptr<Scene> scene = RequirePyScene(self);
                 const std::string lightName = MakeUniqueLightName(scene, self.context(), name, "RectLight");
                 return PyEntityFromEntity(scene, self.context(), self.engine().spawnRectLight(std::move(component), lightName));
             },
             nb::arg("color") = nb::make_tuple(1.f, 1.f, 1.f),
             nb::arg("intensity") = 1.f, nb::arg("width") = 1.f, nb::arg("height") = 1.f,
             nb::arg("name") = std::string())
        .def("spawn_environment_light",
             [](PyEngineApp& self, nb::object color, const std::string& path, float rotation, const std::string& name) {
                 scene::EnvironmentLightComponent component;
                 component.color = ToFloat3(color);
                 component.path = path;
                 component.rotation = rotation;
                 std::shared_ptr<Scene> scene = RequirePyScene(self);
                 const std::string lightName = MakeUniqueLightName(scene, self.context(), name, "EnvironmentLight");
                 return PyEntityFromEntity(scene, self.context(), self.engine().spawnEnvironmentLight(std::move(component), lightName));
             },
             nb::arg("color") = nb::make_tuple(1.f, 1.f, 1.f),
             nb::arg("path") = std::string(), nb::arg("rotation") = 0.f, nb::arg("name") = std::string())

        .def("get_mesh_vertices", [](PyEngineApp& self, const std::shared_ptr<PySceneEntity>& entity) {
                RequireEntityForApp(self, entity);
                return Float3VectorToList(self.engine().getMeshVertices(EntityFromPy(entity)));
            }, nb::arg("entity"))
        .def("set_mesh_vertices",
             [](PyEngineApp& self, const std::shared_ptr<PySceneEntity>& entity, nb::object vertices,
                bool recomputeNormals, bool rebuildAccelerationStructure) {
                 RequireEntityForApp(self, entity);
                 self.engine().setMeshVertices(
                     EntityFromPy(entity),
                     ToFloat3Vector(vertices),
                     { .recomputeNormals = recomputeNormals,
                       .rebuildAccelerationStructure = rebuildAccelerationStructure });
             },
             nb::arg("entity"), nb::arg("vertices"), nb::arg("recompute_normals") = true,
             nb::arg("rebuild_acceleration_structure") = true)
        .def("deform_mesh",
             [](PyEngineApp& self, const std::shared_ptr<PySceneEntity>& entity, nb::object callback,
                bool recomputeNormals, bool rebuildAccelerationStructure) {
                 RequireEntityForApp(self, entity);
                 const ecs::Entity handle = EntityFromPy(entity);
                 std::vector<float3> vertices = self.engine().getMeshVertices(handle);
                 for (size_t i = 0; i < vertices.size(); ++i)
                 {
                     nb::object updated = callback(i, Float3ToTuple(vertices[i]));
                     if (!updated.is_none())
                         vertices[i] = ToFloat3(updated);
                 }
                 self.engine().setMeshVertices(
                     handle, vertices,
                     { .recomputeNormals = recomputeNormals,
                       .rebuildAccelerationStructure = rebuildAccelerationStructure });
                 return vertices.size();
             },
             nb::arg("entity"), nb::arg("callback"), nb::arg("recompute_normals") = true,
             nb::arg("rebuild_acceleration_structure") = true)
        .def("get_mesh_vertices_world", [](PyEngineApp& self, const std::shared_ptr<PySceneEntity>& entity) {
                RequireEntityForApp(self, entity);
                return Float3VectorToList(self.engine().getMeshVerticesWorld(EntityFromPy(entity)));
            }, nb::arg("entity"))
        .def("set_mesh_vertices_world",
             [](PyEngineApp& self, const std::shared_ptr<PySceneEntity>& entity, nb::object vertices,
                bool recomputeNormals, bool rebuildAccelerationStructure) {
                 RequireEntityForApp(self, entity);
                 self.engine().setMeshVerticesWorld(
                     EntityFromPy(entity),
                     ToFloat3Vector(vertices),
                     { .recomputeNormals = recomputeNormals,
                       .rebuildAccelerationStructure = rebuildAccelerationStructure });
             },
             nb::arg("entity"), nb::arg("vertices"), nb::arg("recompute_normals") = true,
             nb::arg("rebuild_acceleration_structure") = true)
        .def("deform_mesh_world",
             [](PyEngineApp& self, const std::shared_ptr<PySceneEntity>& entity, nb::object callback,
                bool recomputeNormals, bool rebuildAccelerationStructure) {
                 RequireEntityForApp(self, entity);
                 const ecs::Entity handle = EntityFromPy(entity);
                 std::vector<float3> vertices = self.engine().getMeshVerticesWorld(handle);
                 for (size_t i = 0; i < vertices.size(); ++i)
                 {
                     nb::object updated = callback(i, Float3ToTuple(vertices[i]));
                     if (!updated.is_none())
                         vertices[i] = ToFloat3(updated);
                 }
                 self.engine().setMeshVerticesWorld(
                     handle, vertices,
                     { .recomputeNormals = recomputeNormals,
                       .rebuildAccelerationStructure = rebuildAccelerationStructure });
                 return vertices.size();
             },
             nb::arg("entity"), nb::arg("callback"), nb::arg("recompute_normals") = true,
             nb::arg("rebuild_acceleration_structure") = true)

        .def("request_shader_reload", [](PyEngineApp& self) {
                self.engine().renderAppState().runtime.Invalidation.ShaderReloadRequested = true;
            })
        .def("request_full_accel_rebuild", [](PyEngineApp& self) {
                self.engine().requestFullAccelRebuild();
            })
        .def("request_mesh_accel_rebuild",
             [](PyEngineApp& self, const std::shared_ptr<PySceneEntity>& entity) {
                 RequireEntityForApp(self, entity);
                 self.engine().requestMeshAccelRebuild(EntityFromPy(entity));
             }, nb::arg("entity"))
        .def("precache_rt_feature_presets",
             [](PyEngineApp& self, bool showProgress) {
                 return self.engine().precacheRtFeaturePresets(showProgress);
             }, nb::arg("show_progress") = true)
        .def("reset_accumulation", [](PyEngineApp& self) {
                self.engine().settings().ResetAccumulation = true;
            })
        .def("reset_realtime_caches", [](PyEngineApp& self) {
                self.engine().settings().ResetRealtimeCaches = true;
            })
        .def("set_realtime_mode",
             [](PyEngineApp& self, bool standaloneDenoiser, int realtimeAA) {
                 self.engine().setRealtimeMode(standaloneDenoiser, realtimeAA);
             },
             nb::arg("standalone_denoiser") = true, nb::arg("realtime_aa") = 2)
        .def("set_reference_mode",
             [](PyEngineApp& self, int spp, bool oidn, int oidnQuality, int oidnPasses, int oidnPrefilter) {
                 self.engine().setReferenceMode(spp, oidn, oidnQuality, oidnPasses, oidnPrefilter);
             },
             nb::arg("spp") = 0, nb::arg("oidn") = false,
             nb::arg("oidn_quality") = 1, nb::arg("oidn_passes") = 1, nb::arg("oidn_prefilter") = 1)
        .def("prepare_animation_frame",
             [](PyEngineApp& self, double sceneTime, bool importedAnimations, bool keyframes) {
                 return self.engine().prepareAnimationFrame(sceneTime, importedAnimations, keyframes);
             },
             nb::arg("time_seconds"), nb::arg("imported_animations") = true, nb::arg("keyframes") = true)
        .def("render_reference_frame", &PyEngineApp::renderReferenceFrame,
             nb::arg("spp") = 64, nb::arg("oidn") = true, nb::arg("max_frames") = 0)
        .def("render_realtime_frame", &PyEngineApp::renderRealtimeFrame,
             nb::arg("dt") = 1.0f / 60.0f)
        .def("save_screenshot",
             [](PyEngineApp& self, const std::string& path) {
                 return self.engine().saveScreenshot(path);
             }, nb::arg("output_path"))

        .def_prop_ro("accumulation_completed", [](PyEngineApp& self) { return self.engine().accumulationCompleted(); })
        .def_prop_ro("accumulation_sample_index", [](PyEngineApp& self) { return self.engine().accumulationSampleIndex(); })
        .def_prop_rw("scene_time",
             [](PyEngineApp& self) { return self.engine().sceneTime(); },
             [](PyEngineApp& self, double value) {
                 if (!std::isfinite(value))
                     throw std::runtime_error("scene_time must be finite");
                 self.engine().setSceneTime(value);
             })
        .def_prop_ro("fps_info", [](PyEngineApp& self) { return self.engine().fpsInfo(); })
        .def_prop_ro("resolution_info", [](PyEngineApp& self) { return self.engine().resolutionInfo(); })
        .def_prop_ro("avg_time_per_frame", [](PyEngineApp& self) { return self.engine().avgTimePerFrame(); })
        .def_prop_ro("frame_index", [](PyEngineApp& self) { return self.engine().frameIndex(); })
        .def_prop_ro("render_size", [](PyEngineApp& self) {
                const auto size = self.engine().renderSize();
                return nb::make_tuple(size.x, size.y);
            })

        .def("__enter__", [](PyEngineApp& self) -> PyEngineApp* { return &self; },
             nb::rv_policy::reference)
        .def("__exit__", [](PyEngineApp& self, nb::object, nb::object, nb::object) -> bool {
             self.shutdown();
             return false;
         }, nb::arg().none(), nb::arg().none(), nb::arg().none());
}


} // namespace caustica_py

#endif // CAUSTICA_WITH_PYTHON
