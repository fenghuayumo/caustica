#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace caustica::editor
{

namespace
{

void DrawDiamond(ImDrawList* drawList, ImVec2 center, float radius, ImU32 color)
{
    const ImVec2 points[] = {
        ImVec2(center.x, center.y - radius),
        ImVec2(center.x + radius, center.y),
        ImVec2(center.x, center.y + radius),
        ImVec2(center.x - radius, center.y),
    };
    drawList->AddConvexPolyFilled(points, IM_ARRAYSIZE(points), color);
}

bool ContainsTime(const std::vector<float>& times, float time, float epsilon)
{
    const auto it = std::lower_bound(times.begin(), times.end(), time);
    if (it != times.end() && std::fabs(*it - time) <= epsilon)
        return true;
    return it != times.begin() && std::fabs(*std::prev(it) - time) <= epsilon;
}

} // namespace

void EditorUI::BuildTimelinePanel(const PanelLayout& layout)
{
    (void)layout;
    if (!m_editorUI.ShowTimeline)
        return;

    if (!ImGui::Begin("Timeline", &m_editorUI.ShowTimeline, ImGuiWindowFlags_NoScrollbar))
    {
        ImGui::End();
        return;
    }

    const int fps = std::max(1, m_editorUI.FramesPerSecond);
    const float frameSeconds = 1.f / static_cast<float>(fps);
    const float duration = m_sceneEditor.animationDuration();
    if (duration > 0.f)
        m_editorUI.EndFrame =
            std::max(m_editorUI.EndFrame, static_cast<int>(std::ceil(duration * fps)));
    m_editorUI.EndFrame = std::max(m_editorUI.StartFrame + 1, m_editorUI.EndFrame);

    float displayTime = static_cast<float>(m_sceneEditor.sceneTime());
    if (duration > 0.f && displayTime > duration)
        displayTime = std::fmod(displayTime, duration);
    int currentFrame = std::clamp(
        static_cast<int>(std::lround(displayTime * fps)),
        m_editorUI.StartFrame,
        m_editorUI.EndFrame);

    const auto setFrame = [&](int frame) {
        frame = std::clamp(frame, m_editorUI.StartFrame, m_editorUI.EndFrame);
        m_settings.EnableAnimations = false;
        m_sceneEditor.evaluateAnimationsAt(frame * frameSeconds);
    };

    if (ImGui::Button("|<"))
        setFrame(m_editorUI.StartFrame);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Jump to start");
    ImGui::SameLine();
    if (ImGui::Button("<"))
        setFrame(currentFrame - 1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Previous frame");
    ImGui::SameLine();

    ImGui::BeginDisabled(!m_settings.RealtimeMode);
    if (ImGui::Button(m_settings.EnableAnimations ? "Pause" : "Play"))
        m_settings.EnableAnimations = !m_settings.EnableAnimations;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(
            m_settings.RealtimeMode
                ? "Play or pause animation (Space)"
                : "Animation playback is unavailable in reference mode");

    ImGui::SameLine();
    if (ImGui::Button(">"))
        setFrame(currentFrame + 1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Next frame");
    ImGui::SameLine();
    if (ImGui::Button(">|"))
        setFrame(m_editorUI.EndFrame);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Jump to end");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    int editedFrame = currentFrame;
    if (ImGui::InputInt("Frame", &editedFrame, 1, 10))
        setFrame(editedFrame);

    const ecs::Entity selected = m_editorUI.SelectedEntity;
    const bool hasSelection = ecs::isValid(selected);
    const float keyTime = currentFrame * frameSeconds;
    const bool hasKey = hasSelection && m_sceneEditor.hasTransformKeyframe(selected, keyTime);

    ImGui::SameLine();
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button(hasKey ? "Update Key" : "Insert Key"))
    {
        m_settings.EnableAnimations = false;
        m_sceneEditor.insertTransformKeyframe(selected, keyTime);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Insert or update Location, Rotation and Scale at the current frame");
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasKey);
    if (ImGui::Button("Delete Key"))
        m_sceneEditor.deleteTransformKeyframe(selected, keyTime);
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.f);
    if (ImGui::DragInt("FPS", &m_editorUI.FramesPerSecond, 1.f, 1, 240))
        m_editorUI.FramesPerSecond = std::clamp(m_editorUI.FramesPerSecond, 1, 240);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    ImGui::DragInt("Start", &m_editorUI.StartFrame, 1.f, 0, m_editorUI.EndFrame - 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    ImGui::DragInt("End", &m_editorUI.EndFrame, 1.f, m_editorUI.StartFrame + 1, 1000000);

    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize(
        std::max(1.f, ImGui::GetContentRegionAvail().x),
        std::max(58.f, ImGui::GetContentRegionAvail().y));
    ImGui::InvisibleButton(
        "##TimelineCanvas",
        canvasSize,
        ImGuiButtonFlags_MouseButtonLeft);

    const ImVec2 canvasMax(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(canvasPos, canvasMax, ImGui::GetColorU32(ImGuiCol_FrameBg), 3.f);
    drawList->AddRect(canvasPos, canvasMax, ImGui::GetColorU32(ImGuiCol_Border), 3.f);

    const int startFrame = m_editorUI.StartFrame;
    const int endFrame = std::max(startFrame + 1, m_editorUI.EndFrame);
    const float frameRange = static_cast<float>(endFrame - startFrame);
    const auto frameToX = [&](float frame) {
        return canvasPos.x + ((frame - startFrame) / frameRange) * canvasSize.x;
    };
    const auto xToFrame = [&](float x) {
        const float normalized = std::clamp((x - canvasPos.x) / canvasSize.x, 0.f, 1.f);
        return startFrame + static_cast<int>(std::lround(normalized * frameRange));
    };

    int majorStep = 1;
    while (canvasSize.x / (frameRange / majorStep) < 70.f)
        majorStep *= (majorStep == 1 || majorStep == 10 || majorStep == 100) ? 5 : 2;
    const int minorStep = std::max(1, majorStep / 5);

    for (int frame = startFrame - (startFrame % minorStep); frame <= endFrame; frame += minorStep)
    {
        if (frame < startFrame)
            continue;
        const float x = frameToX(static_cast<float>(frame));
        const bool major = (frame % majorStep) == 0;
        const float tickBottom = canvasPos.y + (major ? 16.f : 8.f);
        drawList->AddLine(
            ImVec2(x, canvasPos.y),
            ImVec2(x, tickBottom),
            ImGui::GetColorU32(major ? ImGuiCol_TextDisabled : ImGuiCol_Border));
        if (major)
        {
            char label[32];
            std::snprintf(label, sizeof(label), "%d", frame);
            drawList->AddText(
                ImVec2(x + 3.f, canvasPos.y + 2.f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled),
                label);
        }
    }

    const std::vector<float> allTimes = m_sceneEditor.keyframeTimes();
    const std::vector<float> selectedTimes =
        hasSelection ? m_sceneEditor.keyframeTimes(selected) : std::vector<float>{};
    const float keyY = canvasPos.y + canvasSize.y * 0.62f;
    for (float time : allTimes)
    {
        const float frame = time * fps;
        if (frame < startFrame || frame > endFrame)
            continue;
        const float x = frameToX(frame);
        const bool selectedKey = ContainsTime(selectedTimes, time, 1e-4f);
        DrawDiamond(
            drawList,
            ImVec2(x, keyY),
            selectedKey ? 5.f : 3.5f,
            selectedKey
                ? ImGui::GetColorU32(GetEditorColors().Accent)
                : ImGui::GetColorU32(ImGuiCol_TextDisabled));
    }

    const float playheadX = frameToX(static_cast<float>(currentFrame));
    drawList->AddLine(
        ImVec2(playheadX, canvasPos.y),
        ImVec2(playheadX, canvasMax.y),
        ImGui::GetColorU32(GetEditorColors().TextWarning),
        2.f);
    DrawDiamond(
        drawList,
        ImVec2(playheadX, canvasPos.y + 5.f),
        5.f,
        ImGui::GetColorU32(GetEditorColors().TextWarning));

    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        setFrame(xToFrame(ImGui::GetIO().MousePos.x));

    if (allTimes.empty())
    {
        drawList->AddText(
            ImVec2(canvasPos.x + 12.f, canvasMax.y - ImGui::GetTextLineHeight() - 8.f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            "Select an entity, set a frame, then Insert Key.");
    }

    ImGui::End();
}

} // namespace caustica::editor
