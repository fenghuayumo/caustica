#include <render/core/CameraController.h>

#include <render/passes/geometry/TemporalAntiAliasingPass.h>
#include <scene/SceneObjects.h>
#include <scene/SceneCameraAccess.h>
#include <core/file_utils.h>
#include <core/format.h>

#include <fstream>
#include <limits>

namespace caustica
{

namespace
{
    dm::float4x4 MakePinholeIntrinsicsProjection(
        float fx, float fy, float cx, float cy, float width, float height, float zNear)
    {
        width = std::max(width, 1.0f);
        height = std::max(height, 1.0f);
        const float xScale = 2.0f * fx / width;
        const float yScale = 2.0f * fy / height;
        const float xOffset = 2.0f * cx / width - 1.0f;
        const float yOffset = 1.0f - 2.0f * cy / height;
        return dm::float4x4(
            xScale, 0.0f, 0.0f, 0.0f,
            0.0f, yScale, 0.0f, 0.0f,
            xOffset, yOffset, 0.0f, 1.0f,
            0.0f, 0.0f, zNear, 0.0f);
    }
} // anonymous namespace

CameraController::CameraController()
{
    m_view = std::make_shared<PlanarView>();
    m_viewPrevious = std::make_shared<PlanarView>();

    setupDefaultCamera();

    CameraUpdateParams params;
    params.renderSize = dm::uint2(1, 1);
    params.displayAspectRatio = 1.0f;
    updateViews(params);
}

void CameraController::ensureViews(dm::uint2 renderSize)
{
    if (!m_view)
        m_view = std::make_shared<PlanarView>();
    if (!m_viewPrevious)
        m_viewPrevious = std::make_shared<PlanarView>();

    const ViewportDesc viewport(float(renderSize.x), float(renderSize.y));
    m_view->setViewport(viewport);
    m_viewPrevious->setViewport(viewport);
}

void CameraController::swapViews()
{
    std::swap(m_view, m_viewPrevious);
}

void CameraController::setVerticalFOV(float fov)
{
    m_verticalFOV = fov;
    m_useCustomIntrinsics = false;
}

void CameraController::setIntrinsics(float fx, float fy, float cx, float cy, float w, float h)
{
    if (fx <= 0.0f || fy <= 0.0f || w <= 0.0f || h <= 0.0f)
        return;
    m_intrinsics = dm::float4(fx, fy, cx, cy);
    m_intrinsicsViewport = dm::float2(w, h);
    m_verticalFOV = 2.0f * std::atan(h / (2.0f * fy));
    m_useCustomIntrinsics = true;
}

void CameraController::clearIntrinsics()
{
    m_useCustomIntrinsics = false;
}

dm::float2 CameraController::computeJitter(const CameraUpdateParams& params) const
{
    if (!params.realtimeMode || params.realtimeAA == 0 ||
        params.temporalAAPass == nullptr || params.dbgFreezeRealtimeNoiseSeed)
        return dm::float2(0, 0);
    return params.temporalAAPass->GetCurrentPixelOffset();
}

void CameraController::updateFromSceneCamera(const std::shared_ptr<PerspectiveCamera>& sceneCamera)
{
    dm::affine3 viewToWorld = sceneCamera->GetViewToWorldMatrix();
    dm::float3 cameraPos = viewToWorld.m_translation;
    m_camera.LookAt(cameraPos, cameraPos + viewToWorld.m_linear.row2, viewToWorld.m_linear.row1);
    m_verticalFOV = sceneCamera->verticalFov;
    m_zNear = sceneCamera->zNear;
}

void CameraController::updateFromSceneCamera(
    const scene::PerspectiveCameraData& camData,
    const dm::daffine3& globalTransform)
{
    const dm::affine3 viewToWorld = scene::GetCameraViewToWorldMatrix(globalTransform);
    const dm::float3 cameraPos = viewToWorld.m_translation;
    m_camera.LookAt(cameraPos, cameraPos + viewToWorld.m_linear.row2, viewToWorld.m_linear.row1);
    m_verticalFOV = camData.verticalFov;
    m_zNear = camData.zNear;
}

void CameraController::updateViews(const CameraUpdateParams& params)
{
    if (params.temporalAAPass)
        params.temporalAAPass->SetJitter(params.temporalAAJitter);

    ViewportDesc windowViewport(float(params.renderSize.x), float(params.renderSize.y));
    m_view->setViewport(windowViewport);

    const dm::float4x4 projection = m_useCustomIntrinsics
        ? MakePinholeIntrinsicsProjection(
            m_intrinsics.x, m_intrinsics.y, m_intrinsics.z, m_intrinsics.w,
            m_intrinsicsViewport.x, m_intrinsicsViewport.y, m_zNear)
        : dm::perspProjD3DStyleReverse(m_verticalFOV, params.displayAspectRatio, m_zNear);

    m_view->setMatrices(m_camera.GetWorldToViewMatrix(), projection);
    m_view->setPixelOffset(computeJitter(params));
    m_view->updateCache();

    if ((params.frameIndex & 0xFFFFFFFF) == 0 || params.syncPreviousView)
    {
        syncPreviousViewFromCurrent();
    }
}

void CameraController::syncPreviousViewFromCurrent()
{
    if (!m_view || !m_viewPrevious)
        return;

    m_viewPrevious->setMatrices(m_view->getViewMatrix(), m_view->getProjectionMatrix());
    m_viewPrevious->setPixelOffset(m_view->getPixelOffset());
    m_viewPrevious->updateCache();
    updateLastCameraState();
}

void CameraController::saveToFile(const std::filesystem::path& path,
                                  float zNear,
                                  float fovYRadians) const
{
    dm::float3 worldPos = m_camera.GetPosition();
    dm::float3 worldDir = m_camera.GetDir();
    dm::float3 worldUp = m_camera.GetUp();
    dm::dquat rotation;
    dm::affine3 sceneWorldToView = dm::scaling(dm::float3(1.f, 1.f, -1.f)) *
        dm::inverse(m_camera.GetWorldToViewMatrix());
    dm::decomposeAffine<double>(dm::daffine3(sceneWorldToView), nullptr, &rotation, nullptr);

    std::ofstream file;
    file.open(path, std::ios_base::out | std::ios_base::trunc);
    if (!file.is_open())
        return;

    file << worldPos.x << " " << worldPos.y << " " << worldPos.z << " " << std::endl;
    file << worldDir.x << " " << worldDir.y << " " << worldDir.z << " " << std::endl;
    file << worldUp.x << " " << worldUp.y << " " << worldUp.z << " " << std::endl;
    file << std::endl;
    file << "below is the camera node that can be included into the *.scene.json;" << std::endl;
    file << "'Cameras' node goes into 'Graph' array" << std::endl;
    file << std::endl;
    file << "{" << std::endl;
    file << "    \"name\": \"Cameras\"," << std::endl;
    file << "        \"children\" : [" << std::endl;
    file << "    {" << std::endl;
    file << "        \"name\": \"Default\"," << std::endl;
    file << "        \"type\" : \"PerspectiveCameraEx\"," << std::endl;
    file << "        \"translation\" : [" << std::to_string(worldPos.x) << ", "
         << std::to_string(worldPos.y) << ", " << std::to_string(worldPos.z) << "]," << std::endl;
    file << "        \"rotation\" : [" << std::to_string(rotation.x) << ", "
         << std::to_string(rotation.y) << ", " << std::to_string(rotation.z) << ", "
         << std::to_string(rotation.w) << "]," << std::endl;
    file << "        \"verticalFov\" : " << std::to_string(fovYRadians) << "," << std::endl;
    file << "        \"zNear\" : " << std::to_string(zNear) << std::endl;
    file << "    }" << std::endl;
    file << "        ]" << std::endl;
    file << "}," << std::endl;
}

void CameraController::loadFromFile(const std::filesystem::path& path)
{
    dm::float3 worldPos, worldDir, worldUp;
    std::ifstream file;
    file.open(path, std::ios_base::in);
    if (!file.is_open())
        return;

    file >> worldPos.x >> std::ws >> worldPos.y >> std::ws >> worldPos.z;
    file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    file >> worldDir.x >> std::ws >> worldDir.y >> std::ws >> worldDir.z;
    file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    file >> worldUp.x >> std::ws >> worldUp.y >> std::ws >> worldUp.z;
    file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    m_camera.LookAt(worldPos, worldPos + worldDir, worldUp);
}

std::string CameraController::getPosDirUpString() const
{
    auto toString = [](const dm::float3& val) {
        return std::to_string(val.x) + "," + std::to_string(val.y) + "," + std::to_string(val.z);
    };
    return toString(m_camera.GetPosition()) + "," +
           toString(m_camera.GetDir()) + "," +
           toString(m_camera.GetUp());
}

bool CameraController::setFromPosDirUpString(const std::string& val)
{
    if (val.empty())
        return false;

    bool ok = true;
    dm::float3 worldPos, worldDir, worldUp;
    std::string temp = val;
    ok &= ParseFloat3Consume(temp, worldPos);
    ok &= ParseFloat3Consume(temp, worldDir);
    ok &= ParseFloat3Consume(temp, worldUp);
    if (ok)
        m_camera.LookAt(worldPos, worldPos + worldDir, worldUp);
    return ok;
}

void CameraController::setupDefaultCamera()
{
    m_camera.LookAt(dm::float3(0.f, 1.8f, 0.f), dm::float3(1.f, 1.55f, 0.f), dm::float3(0, 1, 0));
    m_verticalFOV = dm::radians(60.0f);
    m_zNear = 0.001f;
}

void CameraController::setupFromSceneCamera(const std::shared_ptr<PerspectiveCamera>& sceneCamera)
{
    if (!sceneCamera)
        setupDefaultCamera();
    else
        updateFromSceneCamera(sceneCamera);
}

bool CameraController::cameraMovedSinceLastFrame() const
{
    const dm::float3 camPos = m_camera.GetPosition();
    const dm::float3 camDir = m_camera.GetDir();
    const dm::float3 camUp = m_camera.GetUp();
    return m_lastDir.x != camDir.x || m_lastDir.y != camDir.y || m_lastDir.z != camDir.z
        || m_lastPos.x != camPos.x || m_lastPos.y != camPos.y || m_lastPos.z != camPos.z
        || m_lastUp.x != camUp.x || m_lastUp.y != camUp.y || m_lastUp.z != camUp.z;
}

void CameraController::updateLastCameraState()
{
    m_lastPos = m_camera.GetPosition();
    m_lastDir = m_camera.GetDir();
    m_lastUp = m_camera.GetUp();
}

} // namespace caustica
