#pragma once

#include <render/core/CameraController.h>

namespace caustica
{

// Logic-thread free camera (Extract / CameraPlugin / editor input).
// Render-thread camera stays on GpuRenderSubsystem (filled from ActiveCameraRenderProxy).
struct SessionCamera
{
    CameraController camera;
};

} // namespace caustica
