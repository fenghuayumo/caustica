#pragma once

#include <memory>
#include <vector>

#include <engine/EngineFrameApplication.h>
#include <core/log.h>
#include <core/Timer.h>
#include <core/command_line.h>

#include "ui/EditorUIData.h"
#include "SceneEditor.h"
#include <render/SessionDiagnostics.h>
#include "SceneEditorFrameExtension.h"
#include <render/WorldRenderer/PathTracingFrameExtension.h>

using caustica::FPSLimiter;

namespace caustica::editor {
class EditorUI;
}

namespace caustica::editor {

// Desktop editor — thin shell over EngineFrameApplication + editor subsystems.
class EditorApplication : public caustica::EngineFrameApplication
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

    caustica::Engine& GetEngine() { return m_engine; }
    const caustica::Engine& GetEngine() const { return m_engine; }

protected:
    void onEvent(caustica::Event& event) override;
    void onDisplayScaleChanged(float scaleX, float scaleY) override;

private:
    void RegisterLogCallback();
    void SampleLogCallback(caustica::Severity severity, const char* message);

    CommandLineOptions CmdLine;
    EditorUIData m_editorUIData;
    caustica::render::SessionDiagnostics m_sessionDiagnostics;
    SceneEditor m_sceneEditor;
    SceneEditorFrameExtension m_sceneEditorFrameExtension{ m_sceneEditor };
    std::vector<caustica::render::IPathTracingFrameExtension*> m_frameExtensions{ &m_sceneEditorFrameExtension };

    caustica::Engine m_engine;
    caustica::Callback m_DefaultLogCallback = nullptr;
    FPSLimiter m_FPSLimiter;
};

} // namespace caustica::editor
