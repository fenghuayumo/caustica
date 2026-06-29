#pragma once

#include <memory>
#include <vector>

#include "engine/Application.h"
#include "core/log.h"
#include <core/Timer.h>
#include <core/command_line.h>

#include "ui/EditorUIData.h"
#include "SceneEditor.h"
#include <render/SessionDiagnostics.h>
#include "SceneEditorFrameExtension.h"
#include <render/WorldRenderer/PathTracingFrameExtension.h>

using caustica::FPSLimiter;

// Forward declarations — full definitions only needed in .cpp
namespace caustica::editor {
class EditorUI;
}
class SceneManager;
namespace caustica {
    class EngineRenderer;
    class GpuDevice;
    class RenderCore;
    class Window;
    struct GpuDeviceCreateDesc;
    namespace render {
        class PathTracingWorldRenderer;
    }
}

namespace caustica::editor {

// Desktop editor — owns Application lifecycle and composes SceneEditor.
class EditorApplication : public caustica::Application
{
public:
    EditorApplication();
    ~EditorApplication() override;

    enum class StartupResult
    {
        Success,
        FailProcessingCommandLine,
        FailToCreateDevice,
        FailDeviceFeatureSupport
    };

    bool init(int argc, const char* const* argv) override;
    void shutdown() override;

    StartupResult startup(int argc, const char* const* argv);

    EditorUIData& GetEditorUIData() { return m_editorUIData; }
    const EditorUIData& GetEditorUIData() const { return m_editorUIData; }

    SceneEditor& GetSceneEditor() { return m_sceneEditor; }
    const SceneEditor& GetSceneEditor() const { return m_sceneEditor; }

    SceneManager* GetSceneManager();
    const SceneManager* GetSceneManager() const;

    caustica::RenderCore* GetRenderCore();
    const caustica::RenderCore* GetRenderCore() const;

    caustica::render::PathTracingWorldRenderer* GetWorldRenderer();
    const caustica::render::PathTracingWorldRenderer* GetWorldRenderer() const;

    bool IsSERSupported() const;

protected:
    void onUpdate(float elapsedTimeSeconds, bool windowFocused) override;
    void onRender() override;
    void onEvent(caustica::Event& event) override;
    void onBackBufferResizing() override;
    void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;
    void onDisplayScaleChanged(float scaleX, float scaleY) override;
    bool shouldRenderWhenUnfocused() const override;

private:
    void RegisterLogCallback();
    void SampleLogCallback(caustica::Severity severity, const char* message);
    void syncPassesToBackBuffer();

    CommandLineOptions CmdLine;
    EditorUIData m_editorUIData;
    caustica::render::SessionDiagnostics m_sessionDiagnostics;
    SceneEditor m_sceneEditor;
    SceneEditorFrameExtension m_sceneEditorFrameExtension{ m_sceneEditor };
    std::vector<caustica::render::IPathTracingFrameExtension*> m_frameExtensions{ &m_sceneEditorFrameExtension };

    caustica::Callback m_DefaultLogCallback = nullptr;
    FPSLimiter m_FPSLimiter;

    std::unique_ptr<caustica::EngineRenderer> m_engineRenderer;
    std::unique_ptr<EditorUI> m_uiPass;
};

} // namespace caustica::editor
