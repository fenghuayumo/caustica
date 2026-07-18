#pragma once

#include <engine/SystemLabel.h>

namespace caustica::editor::system_label
{

struct EditorScenePreStartup { static constexpr const char* name = "EditorScene.PreStartup"; };
struct EditorScenePostStartup { static constexpr const char* name = "EditorScene.PostStartup"; };
struct EditorUIStartup { static constexpr const char* name = "EditorUI.Startup"; };
struct EditorUIShutdown { static constexpr const char* name = "EditorUI.shutdown"; };

struct EditorSceneBeginFrame { static constexpr const char* name = "EditorScene.beginFrame"; };
struct EditorSceneRequestUnfocusedRender { static constexpr const char* name = "EditorScene.RequestUnfocusedRender"; };
struct EditorSceneProcessPendingMutations { static constexpr const char* name = "EditorScene.ProcessPendingMutations"; };
struct EditorSceneAnimateBegin { static constexpr const char* name = "EditorScene.AnimateBegin"; };
struct EditorSceneSyncLoadedScene { static constexpr const char* name = "EditorScene.SyncLoadedScene"; };
struct EditorSceneAnimateEnd { static constexpr const char* name = "EditorScene.AnimateEnd"; };
struct EditorSceneHandleDroppedFiles { static constexpr const char* name = "EditorScene.handleDroppedFiles"; };
struct EditorScenePrepareEditorFrame { static constexpr const char* name = "EditorScene.prepareEditorFrame"; };
struct EditorSceneAfterWorldRender { static constexpr const char* name = "EditorScene.AfterWorldRender"; };

struct EditorUIAnimate { static constexpr const char* name = "EditorUI.animate"; };
struct EditorUIPrepareViewport { static constexpr const char* name = "EditorUI.PrepareViewport"; };
struct EditorUIRenderScene { static constexpr const char* name = "EditorUI.RenderScene"; };

} // namespace caustica::editor::system_label
