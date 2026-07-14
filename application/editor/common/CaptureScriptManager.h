#pragma once

#include <memory>
#include <filesystem>

#include <core/command_line.h>
#include <engine/CaptureSequencer.h>
#include <imgui/imgui_renderer.h>

namespace caustica::render { struct RenderSessionState; }

namespace caustica::editor
{

class SceneEditor;

class CaptureScriptManager
{
public:
	CaptureScriptManager(SceneEditor & sample, caustica::render::RenderSessionState & sessionState, const CommandLineOptions & cmdLine);
	~CaptureScriptManager();

    bool ScriptProgressUI();

    void ScriptMainUI(const ImVec4 & warnColor, const ImVec4 & categoryColor, float indent, float currentScale);

    void PreAnim(float & fElapsedTimeSeconds);
    void PostAnim();

    void preRender();
    void PostRender(const std::function<bool(const char*)>& dumpScreenshotCallback);

    bool IsDoingWork() const    { return m_sequencer.IsDoingWork(); }


private:
    SceneEditor &                    m_app;
    caustica::render::RenderSessionState & m_ui;
    const CommandLineOptions &  m_cmdLine;
    caustica::CaptureSequencer  m_sequencer;
};

} // namespace caustica::editor
