#include "ui/ui_macros.h"
#include "CaptureScriptManager.h"

#include <string>
#include <EditorUI.h>
#include "SceneEditor.h"
#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/Core/ScopedPerfMarker.h>
#include <render/Core/TextureUtils.h>
#include <engine/UserInterfaceUtils.h>
#include <math/math.h>

namespace caustica::editor
{

using namespace caustica;
using namespace caustica;

CaptureScriptManager::CaptureScriptManager(SceneEditor & sample, SampleUIData & sampleUI, const CommandLineOptions & cmdLine)
    : m_app(sample), m_ui(sampleUI), m_cmdLine(cmdLine), m_sequencer(cmdLine)
{
}

CaptureScriptManager::~CaptureScriptManager()
{
}

bool CaptureScriptManager::ScriptProgressUI()
{
    if (!m_sequencer.IsActive() && !m_sequencer.IsStarting())
        return false;

    const auto& settings = m_sequencer.Settings();
    if (m_sequencer.ResetAndWarmupCounter() > 0)
    {
        ImGui::Spacing();
        ImGui::TextWrapped("Running warm-up: %d out of %d", m_sequencer.ResetAndWarmupCounter(), settings.ResetAndWarmupFrames);
    }
    if (!m_ui.RealtimeMode)
    {
        ImGui::TextWrapped("Accumulation mode, sample %d (out of %d target)", m_app.GetAccumulationSampleIndex(), m_ui.AccumulationTarget);
    }
    if (m_sequencer.SequenceRecordCounter() > 0)
    {
        ImGui::Spacing();
        ImGui::TextWrapped("Running sequence export: %d", m_sequencer.SequenceRecordCounter());
    }
    if (m_sequencer.IsStarting())
    {
        ImGui::Spacing();
        ImGui::TextWrapped("Starting (scene loading might still in progress)...");
    }

    return true;
}

void CaptureScriptManager::ScriptMainUI(const ImVec4 & warnColor, const ImVec4 & categoryColor, float indent, float currentScale)
{
    assert(!m_sequencer.IsActive() && !m_sequencer.IsStarting()); // this point should never be reached if active because ScriptProgressUI should be up instead

    auto& settings = m_sequencer.Settings();

    ImGui::TextColored(categoryColor, "Options");

    if (ImGui::Combo("Capture type", &settings.Type, "SimpleScreenshot\0SequenceCapture\0Benchmark\00" ))
        settings.Type = dm::clamp(settings.Type, 0, 0 );

    if (settings.Type == 0)
    {
        ImGui::Checkbox("ResetAndWarmup", &settings.ResetAndWarmup);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("When 'Save screenshot' is used, all subsystem's temporal histories will first be reset,\nfollowed by a selected number of frames before saving the screenshot.\nIf animation is running, it will keep running - if not, it won't.");
        {
            UI_SCOPED_DISABLE(!settings.ResetAndWarmup);
            ImGui::SameLine();
            ImGui::PushItemWidth(-90.0f * currentScale);
            ImGui::InputInt("delay frames", &settings.ResetAndWarmupFrames); settings.ResetAndWarmupFrames = dm::clamp(settings.ResetAndWarmupFrames, 0, 10000);
            ImGui::PopItemWidth();
        }
    }

    // ImGui::Checkbox("Sequence     ", &m_screenshotMiniSequence);
    // if (ImGui::IsItemHovered()) ImGui::SetTooltip("When 'Save screenshot' used, a sequence of screenshots will be recorded instead of saving\na single one. Can work together with ResetAndDelay.");
    // {
    //     UI_SCOPED_DISABLE(!m_screenshotMiniSequence);
    //     ImGui::SameLine();
    //     ImGui::PushItemWidth(-90.0f * currentScale);
    //     ImGui::InputInt("length", &m_screenshotMiniSequenceFrames); m_screenshotMiniSequenceFrames = dm::clamp(m_screenshotMiniSequenceFrames, 1, 999);
    //     ImGui::PopItemWidth();
    // }

    float w = ImGui::GetContentRegionAvail().x;

    ImGui::TextColored(categoryColor, "Output file(s) path:");
    ImGui::SameLine(w * 0.65f);
    if (ImGui::Button("[ select path ]"))
    {
        std::string fileName;
        if (FileDialog(false, "PNG files\0*.png\0BMP files\0*.bmp\0All files\0*.*\0\0", fileName))
            settings.ScreenshotFileName = fileName;
    }

    {
        RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent); );
        RAII_SCOPE(ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x); , ImGui::PopItemWidth(); );
        std::string pathStr = settings.ScreenshotFileName.string();
        ImGui::InputText("##path", (char*)pathStr.c_str(), pathStr.length()+1, /*ImVec2(0, 0),*/ ImGuiInputTextFlags_ReadOnly);
    }

    {
        UI_SCOPED_DISABLE(settings.ScreenshotFileName.empty());

        std::string cmd = "";
        if (settings.Type == 0)
            cmd += "--captureSimple ";
        cmd += "--capturePath " + (settings.ScreenshotFileName.empty() ? "<error, empty>" : settings.ScreenshotFileName.string());

        ImGui::TextColored(categoryColor, "CmdLine, active settings:");
        ImGui::SameLine(w * 0.65f);
        if (ImGui::Button("[cpy to clpbrd]"))
            ImGui::SetClipboardText(cmd.c_str());

        {
            RAII_SCOPE(ImGui::Indent(indent);, ImGui::Unindent(indent); );
            RAII_SCOPE(ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);, ImGui::PopItemWidth(); );
            ImGui::InputText( "##cmdline", (char*)cmd.c_str(), cmd.length()+1, /*ImVec2(0,0),*/ ImGuiInputTextFlags_ReadOnly);
        }

        if (ImGui::Button("=== START CAPTURE ===", ImVec2(-FLT_MIN, 0.0f)))
            m_sequencer.RequestStart();
    }


#if 0
    ImGui::Separator();
    ImGui::TextColored(categoryColor, "[experimental] Save stable animation sequence, path:");
    ImGui::Text(" '%s'", m_screenshotSequencePath.c_str());
    if (ImGui::Checkbox("Save animation sequence", &m_screenshotSequenceCaptureActive))
        if (m_screenshotSequenceCaptureActive)
            m_ui.FPSLimiter = 60;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Example to convert to movie: \nffmpeg -r 60 -i frame_%%05d.bmp -vcodec libx265 -crf 13 -vf scale=1920:1080  outputvideo-1080p-60fps.mp4\n"
        "60 FPS limiter will be automatically enabled for smooth recording!");
    if (!m_screenshotSequenceCaptureActive)
        m_screenshotSequenceCaptureIndex = -64; // -x means x warmup frames for recording to stabilize denoiser
    else
    {
        if (m_screenshotSequenceCaptureIndex < 0) // first x frames are warmup!
            m_app.ResetSceneTime();
        else
        {
            char windowName[1024];
            snprintf(windowName, sizeof(windowName), "%s/frame_%05d.bmp", m_screenshotSequencePath.c_str(), m_screenshotSequenceCaptureIndex);
            settings.ScreenshotFileName = windowName;
        }
        m_screenshotSequenceCaptureIndex++;
    }
    // ImGui::Separator();
    // ImGui::Checkbox("Loop longest animation", &m_ui.LoopLongestAnimation);
    // if (ImGui::IsItemHovered()) ImGui::SetTooltip("If enabled, only restarts all animations when longest one played out. Otherwise loops them individually (and not in sync)!");
#endif
}

void CaptureScriptManager::PreAnim(float& fElapsedTimeSeconds)
{
    m_sequencer.PreAnim(
        fElapsedTimeSeconds,
        m_app.HasAsyncLoadingInProgress(),
        [this](double sceneTime) { m_app.SetSceneTime(sceneTime); });
}

void CaptureScriptManager::PostAnim()
{

}

void CaptureScriptManager::PreRender()
{
    const CaptureSequencerPreRenderActions actions = m_sequencer.PreRender();
    m_ui.ResetRealtimeCaches |= actions.ResetRealtimeCaches;
    m_ui.ResetAccumulation |= actions.ResetAccumulation;
}

void CaptureScriptManager::PostRender(const std::function<bool(const char*)>& dumpScreenshotCallback)
{
    const CaptureSequencerPostRenderResult result = m_sequencer.PostRender(
        m_ui.RealtimeMode,
        m_app.AccumulationCompleted(),
        m_app.GetSceneTime(),
        dumpScreenshotCallback);

    if (result.ExitRequested)
    {
        const std::filesystem::path& capturePath = result.CapturePath;
        if (result.CaptureSuccess)
        {
            caustica::info("Capture of '%s' finished successfully. Exiting.", capturePath.string().c_str());
            std::exit(0);
        }
        else
        {
            caustica::fatal("Unable capture '%s'. Exiting.", capturePath.string().c_str());
            std::exit(1);
        }
    }
}

} // namespace caustica::editor
