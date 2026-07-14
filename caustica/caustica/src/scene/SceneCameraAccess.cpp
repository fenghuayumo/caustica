#include <scene/SceneCameraAccess.h>

#include <scene/SceneEcs.h>

namespace caustica::scene
{

SceneContentFlags getCameraContentFlags()
{
    return SceneContentFlags::Cameras;
}

dm::affine3 getCameraViewToWorldMatrix(const dm::daffine3& globalTransform)
{
    return dm::scaling(dm::float3(1.f, 1.f, -1.f)) * dm::affine3(globalTransform);
}

dm::affine3 getCameraWorldToViewMatrix(const dm::daffine3& globalTransform)
{
    return dm::affine3(inverse(globalTransform)) * dm::scaling(dm::float3(1.f, 1.f, -1.f));
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

bool setCameraProperty(CameraComponent& component, const std::string& propName, const dm::float4& value)
{
    if (auto* perspective = tryGetPerspectiveCameraData(component))
    {
        if (propName == "zNear") { perspective->zNear = value.x; return true; }
        if (propName == "verticalFov") { perspective->verticalFov = value.x; return true; }
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

void initializeCameraComponent(CameraComponent& component, const SceneCamera& camera)
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

void initializeCameraComponent(CameraComponent& component, const std::shared_ptr<SceneCamera>& camera)
{
    if (camera)
        initializeCameraComponent(component, *camera);
}

const CameraComponent* tryGetCamera(const ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<CameraComponent>(entity);
}

CameraComponent* tryGetCamera(ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<CameraComponent>(entity);
}

} // namespace caustica::scene
