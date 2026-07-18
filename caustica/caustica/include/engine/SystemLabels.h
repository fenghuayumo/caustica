#pragma once

#include <engine/SystemLabel.h>

namespace caustica::system_label
{

// App default schedules
struct BeforeFrame { static constexpr const char* name = "BeforeFrame"; };
struct ProcessEventQueue { static constexpr const char* name = "ProcessEventQueue"; };
struct SyncRenderThread { static constexpr const char* name = "SyncRenderThread"; };
struct NotifyDpiScale { static constexpr const char* name = "NotifyDpiScale"; };
struct BeforeAnimate { static constexpr const char* name = "BeforeAnimate"; };
struct SetRenderFrameIndex { static constexpr const char* name = "SetRenderFrameIndex"; };
struct AfterAnimate { static constexpr const char* name = "AfterAnimate"; };
#if CAUSTICA_WITH_STREAMLINE
struct StreamlineSimStart { static constexpr const char* name = "StreamlineSimStart"; };
#endif

// Scene runtime
struct SceneStartup { static constexpr const char* name = "Scene.Startup"; };
struct SceneBeginFrame { static constexpr const char* name = "Scene.beginFrame"; };
struct SceneAnimate { static constexpr const char* name = "Scene.animate"; };
struct SceneUpdateCamera { static constexpr const char* name = "Scene.updateCamera"; };
struct SceneTickSimulation { static constexpr const char* name = "Scene.TickSimulation"; };
struct SceneUpdateWindowTitle { static constexpr const char* name = "Scene.UpdateWindowTitle"; };
struct SceneRefreshEntityWorld { static constexpr const char* name = "Scene.RefreshEntityWorld"; };
struct ScenePrepareRenderFrame { static constexpr const char* name = "Scene.PrepareRenderFrame"; };
struct SceneRenderScene { static constexpr const char* name = "Scene.RenderScene"; };
struct SceneAfterWorldRender { static constexpr const char* name = "Scene.AfterWorldRender"; };

// GPU / assets
struct GpuRenderEndFrame { static constexpr const char* name = "GpuRender.endFrame"; };
struct GpuRenderShutdown { static constexpr const char* name = "GpuRender.shutdown"; };
struct AssetSystemShutdown { static constexpr const char* name = "AssetSystem.shutdown"; };

} // namespace caustica::system_label
