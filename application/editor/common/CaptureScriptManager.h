#pragma once

#include <memory>
#include <filesystem>

#include <core/command_line.h>
#include <engine/CaptureSequencer.h>
#include <imgui/imgui_renderer.h>

namespace caustica::render { struct RenderAppState; }

namespace caustica::editor
{

class SceneEditor;

class CaptureScriptManager
{
public:
	CaptureScriptManager(SceneEditor & sample, caustica::render::RenderAppState & renderState, const CommandLineOptions & cmdLine);
	~CaptureScriptManager();

    bool ScriptProgressUI();

    void ScriptMainUI(const ImVec4 & warnColor, const ImVec4 & categoryColor, float indent, float currentScale);

    void preAnim(float & fElapsedTimeSeconds);
    void PostAnim();

    void preRender();
    void postRender(const std::function<bool(const char*)>& dumpScreenshotCallback);

    bool isDoingWork() const    { return m_sequencer.isDoingWork(); }


private:
    SceneEditor &                    m_app;
    caustica::render::RenderAppState & m_ui;
    const CommandLineOptions &  m_cmdLine;
    caustica::CaptureSequencer  m_sequencer;
};

} // namespace caustica::editor
