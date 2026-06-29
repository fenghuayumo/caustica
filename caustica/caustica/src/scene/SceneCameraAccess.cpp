#include <scene/SceneCameraAccess.h>

#include <scene/SceneEcs.h>

namespace caustica::scene
{

SceneContentFlags GetCameraContentFlags()
{
    return SceneContentFlags::Cameras;
}

dm::affine3 GetCameraViewToWorldMatrix(const dm::daffine3& globalTransform)
{
    return dm::scaling(dm::float3(1.f, 1.f, -1.f)) * dm::affine3(globalTransform);
}

dm::affine3 GetCameraWorldToViewMatrix(const dm::daffine3& globalTransform)
{
    return dm::affine3(inverse(globalTransform)) * dm::scaling(dm::float3(1.f, 1.f, -1.f));
}

bool IsPerspectiveCamera(const CameraComponent& component)
{
    return std::holds_alternative<PerspectiveCameraData>(component.data);
}

bool IsOrthographicCamera(const CameraComponent& component)
{
    return std::holds_alternative<OrthographicCameraData>(component.data);
}

const PerspectiveCameraData* TryGetPerspectiveCameraData(const CameraComponent& component)
{
    return std::get_if<PerspectiveCameraData>(&component.data);
}

PerspectiveCameraData* TryGetPerspectiveCameraData(CameraComponent& component)
{
    return std::get_if<PerspectiveCameraData>(&component.data);
}

const OrthographicCameraData* TryGetOrthographicCameraData(const CameraComponent& component)
{
    return std::get_if<OrthographicCameraData>(&component.data);
}

OrthographicCameraData* TryGetOrthographicCameraData(CameraComponent& component)
{
    return std::get_if<OrthographicCameraData>(&component.data);
}

bool SetCameraProperty(CameraComponent& component, const std::string& propName, const dm::float4& value)
{
    if (auto* perspective = TryGetPerspectiveCameraData(component))
    {
        if (propName == "zNear") { perspective->zNear = value.x; return true; }
        if (propName == "verticalFov") { perspective->verticalFov = value.x; return true; }
        if (propName == "zFar") { perspective->zFar = value.x; return true; }
        if (propName == "aspectRatio") { perspective->aspectRatio = value.x; return true; }
    }
    else if (auto* orthographic = TryGetOrthographicCameraData(component))
    {
        if (propName == "zNear") { orthographic->zNear = value.x; return true; }
        if (propName == "zFar") { orthographic->zFar = value.x; return true; }
        if (propName == "xMag") { orthographic->xMag = value.x; return true; }
        if (propName == "yMag") { orthographic->yMag = value.x; return true; }
    }
    return false;
}

void InitializeCameraComponent(CameraComponent& component, const SceneCamera& camera)
{
    if (const auto* perspective = dynamic_cast<const PerspectiveCamera*>(&camera))
    {
        component.data = PerspectiveCameraData{
            perspective->zNear,
            perspective->verticalFov,
            perspective->zFar,
            perspective->aspectRatio,
            perspective->enableAutoExposure,
            perspective->exposureCompensation,
            perspective->exposureValue,
            perspective->exposureValueMin,
            perspective->exposureValueMax,
        };
    }
    else if (const auto* orthographic = dynamic_cast<const OrthographicCamera*>(&camera))
    {
        component.data = OrthographicCameraData{
            orthographic->zNear,
            orthographic->zFar,
            orthographic->xMag,
            orthographic->yMag,
        };
    }
}

void InitializeCameraComponent(CameraComponent& component, const std::shared_ptr<SceneCamera>& camera)
{
    if (camera)
        InitializeCameraComponent(component, *camera);
}

const CameraComponent* TryGetCamera(const ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<CameraComponent>(entity);
}

CameraComponent* TryGetCamera(ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<CameraComponent>(entity);
}

} // namespace caustica::scene
