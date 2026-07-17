#pragma once

#include <render/core/CameraController.h>

namespace caustica
{

// Logic-thread free camera (Extract / CameraPlugin / editor input).
// Render-thread camera lives on WorldRenderer (filled from ActiveCameraRenderProxy).
struct SessionCamera
{
    CameraController camera;
};

} // namespace caustica
