#pragma once

#include <scene/SceneRenderData.h>

namespace caustica
{

// App-world resource: free vs scene camera resolved after TransformPropagate.
// Extract copies this into SceneRenderData::camera — no re-resolve on the Extract path.
struct ResolvedActiveCamera
{
    scene::ActiveCameraRenderProxy camera;
};

} // namespace caustica
