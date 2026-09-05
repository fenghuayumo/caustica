#include <scene/SceneCameraAccess.h>

#include <scene/SceneEcs.h>

#include <cmath>
#include <string>

namespace caustica::scene
{

namespace
{

constexpr float kDirEpsilon = 1e-6f;

bool IsFinitePositive(float value)
{
    return std::isfinite(value) && value > 0.f;
}

math::float3 SafeNormalize(const math::float3& v, const math::float3& fallback)
{
    const float len = math::length(v);
    return len > kDirEpsilon ? v / len : fallback;
}

math::affine3 MakeCameraViewToWorld(const math::float3& position, const math::float3& direction, const math::float3& up)
{
    const math::float3 d = SafeNormalize(direction, math::float3(0.f, 0.f, -1.f));
    math::float3 r = math::cross(d, up);
    if (math::length(r) <= kDirEpsilon)
    {
        const math::float3 fallback = std::abs(d.y) < 0.99f
            ? math::float3(0.f, 1.f, 0.f)
            : math::float3(1.f, 0.f, 0.f);
        r = math::cross(d, fallback);
    }
    r = SafeNormalize(r, math::float3(1.f, 0.f, 0.f));
    const math::float3 u = SafeNormalize(math::cross(r, d), math::float3(0.f, 1.f, 0.f));
    return math::affine3(r, u, d, position);
}

void MarkCameraComponentChanged(SceneEntityWorld& world, ecs::Entity entity)
{
    world.world().notifyComponentChanged<CameraComponent>(entity);
}

PerspectiveCameraData* MutablePerspective(SceneEntityWorld& world, ecs::Entity entity)
{
    if (CameraComponent* camera = tryGetCamera(world.world(), entity))
        return tryGetPerspectiveCameraData(*camera);
    return nullptr;
}

const PerspectiveCameraData* ImmutablePerspective(const SceneEntityWorld& world, ecs::Entity entity)
{
    if (const CameraComponent* camera = tryGetCamera(world.world(), entity))
        return tryGetPerspectiveCameraData(*camera);
    return nullptr;
}

} // namespace

SceneContentFlags getCameraContentFlags()
{
    return SceneContentFlags::Cameras;
}

math::affine3 getCameraViewToWorldMatrix(const math::daffine3& globalTransform)
{
    return math::scaling(math::float3(1.f, 1.f, -1.f)) * math::affine3(globalTransform);
}

math::affine3 getCameraWorldToViewMatrix(const math::daffine3& globalTransform)
{
    return math::affine3(inverse(globalTransform)) * math::scaling(math::float3(1.f, 1.f, -1.f));
}

bool isPerspectiveCamera(const CameraComponent& component)
{
    return std::holds_alternative<PerspectiveCameraData>(component.data);
}

bool isOrthographicCamera(const CameraComponent& component)
{
    return std::holds_alternative<OrthographicCameraData>(component.data);
}

const PerspectiveCameraData* tryGetPerspectiveCameraData(const CameraComponent& component)
{
    return std::get_if<PerspectiveCameraData>(&component.data);
}

PerspectiveCameraData* tryGetPerspectiveCameraData(CameraComponent& component)
{
    return std::get_if<PerspectiveCameraData>(&component.data);
}

const OrthographicCameraData* tryGetOrthographicCameraData(const CameraComponent& component)
{
    return std::get_if<OrthographicCameraData>(&component.data);
}

OrthographicCameraData* tryGetOrthographicCameraData(CameraComponent& component)
{
    return std::get_if<OrthographicCameraData>(&component.data);
}

bool setCameraProperty(CameraComponent& component, const std::string& propName, const math::float4& value)
{
    if (auto* perspective = tryGetPerspectiveCameraData(component))
    {
        if (propName == "zNear") { perspective->zNear = value.x; return true; }
        if (propName == "verticalFov")
        {
            perspective->verticalFov = value.x;
            perspective->intrinsics.reset();
            return true;
        }
        if (propName == "zFar") { perspective->zFar = value.x; return true; }
        if (propName == "aspectRatio") { perspective->aspectRatio = value.x; return true; }
    }
    else if (auto* orthographic = tryGetOrthographicCameraData(component))
    {
        if (propName == "zNear") { orthographic->zNear = value.x; return true; }
        if (propName == "zFar") { orthographic->zFar = value.x; return true; }
        if (propName == "xMag") { orthographic->xMag = value.x; return true; }
        if (propName == "yMag") { orthographic->yMag = value.x; return true; }
    }
    return false;
}

const CameraComponent* tryGetCamera(const ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<CameraComponent>(entity);
}

CameraComponent* tryGetCamera(ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<CameraComponent>(entity);
}

const PerspectiveCameraData* tryGetPerspectiveCamera(const SceneEntityWorld& world, ecs::Entity entity)
{
    return ImmutablePerspective(world, entity);
}

PerspectiveCameraData* tryGetPerspectiveCamera(SceneEntityWorld& world, ecs::Entity entity)
{
    return MutablePerspective(world, entity);
}

bool tryGetCameraWorldLookTo(const SceneEntityWorld& world, ecs::Entity entity, CameraWorldLookTo& out)
{
    if (!tryGetCamera(world.world(), entity))
        return false;
    const auto* global = world.world().get<GlobalTransformComponent>(entity);
    if (!global)
        return false;

    const math::affine3 viewToWorld = getCameraViewToWorldMatrix(global->transform);
    out.position = viewToWorld.m_translation;
    out.direction = SafeNormalize(viewToWorld.m_linear.row2, math::float3(0.f, 0.f, -1.f));
    out.up = SafeNormalize(viewToWorld.m_linear.row1, math::float3(0.f, 1.f, 0.f));
    return true;
}

bool setCameraWorldLookTo(
    SceneEntityWorld& world,
    ecs::Entity entity,
    const math::float3& position,
    const math::float3& direction,
    const math::float3& up)
{
    if (!tryGetCamera(world.world(), entity))
        return false;
    if (!math::all(math::isfinite(position))
        || !math::all(math::isfinite(direction))
        || !math::all(math::isfinite(up))
        || math::length(direction) <= kDirEpsilon)
        return false;

    const math::affine3 viewToWorld = MakeCameraViewToWorld(position, direction, up);
    const math::affine3 zflip = math::scaling(math::float3(1.f, 1.f, -1.f));
    const math::daffine3 desiredWorld(zflip * viewToWorld);

    ecs::Entity parentEntity = ecs::NullEntity;
    if (const auto* parent = world.world().get<ParentComponent>(entity))
        parentEntity = parent->parent;

    math::daffine3 parentToWorld = math::daffine3::identity();
    if (ecs::isValid(parentEntity))
    {
        if (const auto* globalTransform = world.world().get<GlobalTransformComponent>(parentEntity))
            parentToWorld = globalTransform->transform;
    }

    // Scene hierarchy composition is row-vector based: world = local * parent.
    const math::daffine3 localToParent = desiredWorld * inverse(parentToWorld);
    math::double3 translation;
    math::dquat rotation;
    math::double3 scaling;
    decomposeAffine<double>(localToParent, &translation, &rotation, &scaling);
    world.setLocalTransform(entity, &translation, &rotation, &scaling);
    world.refreshHierarchy();
    return true;
}

bool setCameraVerticalFov(SceneEntityWorld& world, ecs::Entity entity, float radians)
{
    if (!std::isfinite(radians) || radians <= 0.f || radians >= math::PI_f)
        return false;
    PerspectiveCameraData* perspective = MutablePerspective(world, entity);
    if (!perspective)
        return false;
    perspective->verticalFov = radians;
    perspective->intrinsics.reset();
    MarkCameraComponentChanged(world, entity);
    return true;
}

float getCameraVerticalFov(const SceneEntityWorld& world, ecs::Entity entity)
{
    const PerspectiveCameraData* perspective = ImmutablePerspective(world, entity);
    return perspective ? perspective->verticalFov : 0.f;
}

bool setCameraZNear(SceneEntityWorld& world, ecs::Entity entity, float zNear)
{
    if (!IsFinitePositive(zNear))
        return false;
    if (PerspectiveCameraData* perspective = MutablePerspective(world, entity))
    {
        if (perspective->zFar && zNear >= *perspective->zFar)
            return false;
        perspective->zNear = zNear;
        MarkCameraComponentChanged(world, entity);
        return true;
    }
    if (CameraComponent* camera = tryGetCamera(world.world(), entity))
    {
        if (OrthographicCameraData* ortho = tryGetOrthographicCameraData(*camera))
        {
            if (zNear >= ortho->zFar)
                return false;
            ortho->zNear = zNear;
            MarkCameraComponentChanged(world, entity);
            return true;
        }
    }
    return false;
}

float getCameraZNear(const SceneEntityWorld& world, ecs::Entity entity)
{
    if (const PerspectiveCameraData* perspective = ImmutablePerspective(world, entity))
        return perspective->zNear;
    if (const CameraComponent* camera = tryGetCamera(world.world(), entity))
    {
        if (const OrthographicCameraData* ortho = tryGetOrthographicCameraData(*camera))
            return ortho->zNear;
    }
    return 0.f;
}

bool setCameraZFar(SceneEntityWorld& world, ecs::Entity entity, std::optional<float> zFar)
{
    PerspectiveCameraData* perspective = MutablePerspective(world, entity);
    if (perspective)
    {
        if (zFar && (!IsFinitePositive(*zFar) || *zFar <= perspective->zNear))
            return false;
        perspective->zFar = zFar;
        MarkCameraComponentChanged(world, entity);
        return true;
    }
    if (CameraComponent* camera = tryGetCamera(world.world(), entity))
    {
        if (OrthographicCameraData* ortho = tryGetOrthographicCameraData(*camera))
        {
            if (!zFar || !IsFinitePositive(*zFar) || *zFar <= ortho->zNear)
                return false;
            ortho->zFar = *zFar;
            MarkCameraComponentChanged(world, entity);
            return true;
        }
    }
    return false;
}

std::optional<float> getCameraZFar(const SceneEntityWorld& world, ecs::Entity entity)
{
    const PerspectiveCameraData* perspective = ImmutablePerspective(world, entity);
    if (perspective)
        return perspective->zFar;
    if (const CameraComponent* camera = tryGetCamera(world.world(), entity))
    {
        if (const OrthographicCameraData* ortho = tryGetOrthographicCameraData(*camera))
            return ortho->zFar;
    }
    return std::nullopt;
}

bool setCameraAspectRatio(SceneEntityWorld& world, ecs::Entity entity, std::optional<float> aspectRatio)
{
    if (aspectRatio && !IsFinitePositive(*aspectRatio))
        return false;
    PerspectiveCameraData* perspective = MutablePerspective(world, entity);
    if (!perspective)
        return false;
    perspective->aspectRatio = aspectRatio;
    MarkCameraComponentChanged(world, entity);
    return true;
}

std::optional<float> getCameraAspectRatio(const SceneEntityWorld& world, ecs::Entity entity)
{
    const PerspectiveCameraData* perspective = ImmutablePerspective(world, entity);
    return perspective ? perspective->aspectRatio : std::nullopt;
}

bool setCameraIntrinsics(
    SceneEntityWorld& world,
    ecs::Entity entity,
    float fx,
    float fy,
    float cx,
    float cy,
    float width,
    float height)
{
    if (!IsFinitePositive(fx) || !IsFinitePositive(fy)
        || !std::isfinite(cx) || !std::isfinite(cy)
        || !IsFinitePositive(width) || !IsFinitePositive(height)
        )
        return false;
    PerspectiveCameraData* perspective = MutablePerspective(world, entity);
    if (!perspective)
        return false;
    perspective->intrinsics = CameraIntrinsics{ fx, fy, cx, cy, width, height };
    perspective->verticalFov = 2.f * std::atan(height / (2.f * fy));
    MarkCameraComponentChanged(world, entity);
    return true;
}

bool clearCameraIntrinsics(SceneEntityWorld& world, ecs::Entity entity)
{
    PerspectiveCameraData* perspective = MutablePerspective(world, entity);
    if (!perspective)
        return false;
    perspective->intrinsics.reset();
    MarkCameraComponentChanged(world, entity);
    return true;
}

const CameraIntrinsics* tryGetCameraIntrinsics(const SceneEntityWorld& world, ecs::Entity entity)
{
    const PerspectiveCameraData* perspective = ImmutablePerspective(world, entity);
    if (!perspective || !perspective->intrinsics)
        return nullptr;
    return &*perspective->intrinsics;
}

} // namespace caustica::scene
