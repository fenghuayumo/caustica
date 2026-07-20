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

enum class TimelineIcon
{
    JumpStart,
    PreviousFrame,
    Play,
    Pause,
    NextFrame,
    JumpEnd,
    InsertKey,
    DeleteKey,
};

void DrawTimelineIcon(
    ImDrawList* drawList,
    ImVec2 min,
    ImVec2 max,
    TimelineIcon icon,
    ImU32 color)
{
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float s = std::min(max.x - min.x, max.y - min.y) * 0.25f;
    const float thickness = std::max(1.5f, s * 0.24f);

    const auto drawChevron = [&](bool right) {
        const float direction = right ? 1.f : -1.f;
        drawList->AddLine(
            ImVec2(center.x - direction * s * 0.45f, center.y - s * 0.72f),
            ImVec2(center.x + direction * s * 0.30f, center.y),
            color,
            thickness);
        drawList->AddLine(
            ImVec2(center.x + direction * s * 0.30f, center.y),
            ImVec2(center.x - direction * s * 0.45f, center.y + s * 0.72f),
            color,
            thickness);
    };

    switch (icon)
    {
    case TimelineIcon::JumpStart:
        drawList->AddRectFilled(
            ImVec2(center.x - s * 0.90f, center.y - s * 0.78f),
            ImVec2(center.x - s * 0.66f, center.y + s * 0.78f),
            color,
            1.f);
        drawList->AddTriangleFilled(
            ImVec2(center.x - s * 0.48f, center.y),
            ImVec2(center.x + s * 0.62f, center.y - s * 0.76f),
            ImVec2(center.x + s * 0.62f, center.y + s * 0.76f),
            color);
        break;
    case TimelineIcon::PreviousFrame:
        drawChevron(false);
        break;
    case TimelineIcon::Play:
        drawList->AddTriangleFilled(
            ImVec2(center.x - s * 0.58f, center.y - s * 0.82f),
            ImVec2(center.x + s * 0.78f, center.y),
            ImVec2(center.x - s * 0.58f, center.y + s * 0.82f),
            color);
        break;
    case TimelineIcon::Pause:
        drawList->AddRectFilled(
            ImVec2(center.x - s * 0.62f, center.y - s * 0.80f),
            ImVec2(center.x - s * 0.16f, center.y + s * 0.80f),
            color,
            1.f);
        drawList->AddRectFilled(
            ImVec2(center.x + s * 0.16f, center.y - s * 0.80f),
            ImVec2(center.x + s * 0.62f, center.y + s * 0.80f),
            color,
            1.f);
        break;
    case TimelineIcon::NextFrame:
        drawChevron(true);
        break;
    case TimelineIcon::JumpEnd:
        drawList->AddTriangleFilled(
            ImVec2(center.x + s * 0.48f, center.y),
            ImVec2(center.x - s * 0.62f, center.y - s * 0.76f),
            ImVec2(center.x - s * 0.62f, center.y + s * 0.76f),
            color);
        drawList->AddRectFilled(
            ImVec2(center.x + s * 0.66f, center.y - s * 0.78f),
            ImVec2(center.x + s * 0.90f, center.y + s * 0.78f),
            color,
            1.f);
        break;
    case TimelineIcon::InsertKey:
    case TimelineIcon::DeleteKey:
    {
        const ImVec2 diamond[] = {
            ImVec2(center.x, center.y - s * 0.88f),
            ImVec2(center.x + s * 0.88f, center.y),
            ImVec2(center.x, center.y + s * 0.88f),
            ImVec2(center.x - s * 0.88f, center.y),
        };
        if (icon == TimelineIcon::InsertKey)
            drawList->AddConvexPolyFilled(diamond, IM_ARRAYSIZE(diamond), color);
        else
            drawList->AddPolyline(
                diamond,
                IM_ARRAYSIZE(diamond),
                color,
                ImDrawFlags_Closed,
                thickness);

        const ImU32 markColor =
            icon == TimelineIcon::InsertKey ? IM_COL32(24, 27, 31, 255) : color;
        drawList->AddLine(
            ImVec2(center.x - s * 0.38f, center.y),
            ImVec2(center.x + s * 0.38f, center.y),
            markColor,
            std::max(1.2f, thickness * 0.72f));
        if (icon == TimelineIcon::InsertKey)
        {
            drawList->AddLine(
                ImVec2(center.x, center.y - s * 0.38f),
                ImVec2(center.x, center.y + s * 0.38f),
                markColor,
                std::max(1.2f, thickness * 0.72f));
        }
        break;
    }
    }
}

bool TimelineIconButton(
    const char* id,
    TimelineIcon icon,
    bool selected,
    bool enabled,
    const char* tooltip)
{
    constexpr float width = 29.f;
    constexpr float height = 27.f;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);

    if (!enabled)
        ImGui::BeginDisabled();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
    const bool hovered =
        ImGui::IsItemHovered(enabled ? ImGuiHoveredFlags_None : ImGuiHoveredFlags_AllowWhenDisabled);
    if (!enabled)
        ImGui::EndDisabled();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const EditorColors& colors = GetEditorColors();
    if (selected)
    {
        drawList->AddRectFilled(
            min, max, ImGui::ColorConvertFloat4ToU32(colors.ToolbarIdleActive), 4.f);
        drawList->AddRect(
            min, max, ImGui::ColorConvertFloat4ToU32(colors.AccentHovered), 4.f, 0, 1.f);
    }
    else if (hovered && enabled)
    {
        drawList->AddRectFilled(
            min, max, ImGui::ColorConvertFloat4ToU32(colors.ToolbarIdleHovered), 4.f);
    }
    else
    {
        drawList->AddRectFilled(
            min, max, ImGui::ColorConvertFloat4ToU32(colors.ToolbarIdle), 4.f);
    }

    const ImU32 foreground = ImGui::ColorConvertFloat4ToU32(
        !enabled
            ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
            : (selected ? colors.AccentHovered : colors.Text));
    DrawTimelineIcon(drawList, min, max, icon, foreground);

    if (tooltip && hovered)
        ImGui::SetTooltip("%s", tooltip);
    return enabled && pressed;
}

void TimelineToolbarSeparator()
{
    ImGui::SameLine(0.f, 5.f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(pos.x + 1.f, pos.y + 4.f),
        ImVec2(pos.x + 1.f, pos.y + 23.f),
        ImGui::GetColorU32(ImGuiCol_Border),
        1.f);
    ImGui::Dummy(ImVec2(3.f, 27.f));
    ImGui::SameLine(0.f, 5.f);
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
        if (frame == currentFrame && !m_settings.EnableAnimations)
            return;
        m_settings.EnableAnimations = false;
        m_sceneEditor.evaluateAnimationsAt(static_cast<float>(frame) * frameSeconds);
        currentFrame = frame;
    };

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.f, 4.f));
    if (TimelineIconButton(
            "##JumpStart",
            TimelineIcon::JumpStart,
            false,
            true,
            "Jump to start"))
        setFrame(m_editorUI.StartFrame);
    ImGui::SameLine(0.f, 2.f);
    if (TimelineIconButton(
            "##PreviousFrame",
            TimelineIcon::PreviousFrame,
            false,
            true,
            "Previous frame"))
        setFrame(currentFrame - 1);
    ImGui::SameLine(0.f, 2.f);
    if (TimelineIconButton(
            "##PlayPause",
            m_settings.EnableAnimations ? TimelineIcon::Pause : TimelineIcon::Play,
            m_settings.EnableAnimations,
            m_settings.RealtimeMode,
            m_settings.RealtimeMode
                ? (m_settings.EnableAnimations ? "Pause animation (Space)" : "Play animation (Space)")
                : "Animation playback is unavailable in reference mode"))
        m_settings.EnableAnimations = !m_settings.EnableAnimations;
    ImGui::SameLine(0.f, 2.f);
    if (TimelineIconButton(
            "##NextFrame",
            TimelineIcon::NextFrame,
            false,
            true,
            "Next frame"))
        setFrame(currentFrame + 1);
    ImGui::SameLine(0.f, 2.f);
    if (TimelineIconButton(
            "##JumpEnd",
            TimelineIcon::JumpEnd,
            false,
            true,
            "Jump to end"))
        setFrame(m_editorUI.EndFrame);

    const ecs::Entity selected = m_editorUI.SelectedEntity;
    const bool hasSelection = ecs::isValid(selected);
    const float keyTime = currentFrame * frameSeconds;
    const bool hasTransformKey =
        hasSelection && m_sceneEditor.hasTransformKeyframe(selected, keyTime);
    const bool canVisibility =
        hasSelection && m_sceneEditor.canAnimateVisibility(selected);
    const bool hasVisibilityKey =
        canVisibility && m_sceneEditor.hasVisibilityKeyframe(selected, keyTime);

    TimelineToolbarSeparator();
    if (TimelineIconButton(
            "##InsertKey",
            TimelineIcon::InsertKey,
            hasTransformKey,
            hasSelection,
            hasTransformKey
                ? "Update Location / Rotation / Scale keyframe (I)"
                : "Insert Location, Rotation and Scale keyframe (I)"))
    {
        m_settings.EnableAnimations = false;
        m_sceneEditor.insertTransformKeyframe(selected, keyTime);
    }
    ImGui::SameLine(0.f, 2.f);
    if (TimelineIconButton(
            "##DeleteKey",
            TimelineIcon::DeleteKey,
            false,
            hasTransformKey,
            "Delete Location / Rotation / Scale keyframe"))
        m_sceneEditor.deleteTransformKeyframe(selected, keyTime);

    TimelineToolbarSeparator();
    if (TimelineIconButton(
            "##InsertVisibilityKey",
            TimelineIcon::InsertKey,
            hasVisibilityKey,
            canVisibility,
            !canVisibility
                ? "Select a Mesh or 3DGS to keyframe visibility"
                : (hasVisibilityKey
                    ? "Update visibility keyframe (from Hierarchy eye / Inspector Visible) (Shift+I)"
                    : "Insert visibility keyframe (current Visible state) (Shift+I)")))
    {
        m_settings.EnableAnimations = false;
        m_sceneEditor.insertVisibilityKeyframe(selected, keyTime);
    }
    ImGui::SameLine(0.f, 2.f);
    if (TimelineIconButton(
            "##DeleteVisibilityKey",
            TimelineIcon::DeleteKey,
            false,
            hasVisibilityKey,
            "Delete visibility keyframe"))
        m_sceneEditor.deleteVisibilityKeyframe(selected, keyTime);

    TimelineToolbarSeparator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Frame");
    ImGui::SameLine(0.f, 5.f);
    ImGui::SetNextItemWidth(72.f);
    int editedFrame = currentFrame;
    if (ImGui::DragInt("##CurrentFrame", &editedFrame, 0.25f, m_editorUI.StartFrame, m_editorUI.EndFrame))
        setFrame(editedFrame);

    ImGui::SameLine(0.f, 12.f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Range");
    ImGui::SameLine(0.f, 5.f);
    ImGui::SetNextItemWidth(58.f);
    ImGui::DragInt("##StartFrame", &m_editorUI.StartFrame, 1.f, 0, m_editorUI.EndFrame - 1);
    ImGui::SameLine(0.f, 3.f);
    ImGui::TextDisabled("-");
    ImGui::SameLine(0.f, 3.f);
    ImGui::SetNextItemWidth(58.f);
    ImGui::DragInt(
        "##EndFrame", &m_editorUI.EndFrame, 1.f, m_editorUI.StartFrame + 1, 1000000);

    ImGui::SameLine(0.f, 12.f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("FPS");
    ImGui::SameLine(0.f, 5.f);
    ImGui::SetNextItemWidth(52.f);
    if (ImGui::DragInt("##TimelineFPS", &m_editorUI.FramesPerSecond, 1.f, 1, 240))
        m_editorUI.FramesPerSecond = std::clamp(m_editorUI.FramesPerSecond, 1, 240);
    ImGui::PopStyleVar();

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

    const std::vector<float> transformTimes = m_sceneEditor.keyframeTimes();
    const std::vector<float> selectedTransformTimes =
        hasSelection ? m_sceneEditor.keyframeTimes(selected) : std::vector<float>{};
    const std::vector<float> visibilityTimes = m_sceneEditor.visibilityKeyframeTimes();
    const std::vector<float> selectedVisibilityTimes =
        hasSelection ? m_sceneEditor.visibilityKeyframeTimes(selected) : std::vector<float>{};

    const float transformKeyY = canvasPos.y + canvasSize.y * 0.52f;
    const float visibilityKeyY = canvasPos.y + canvasSize.y * 0.78f;
    for (float time : transformTimes)
    {
        const float frame = time * fps;
        if (frame < startFrame || frame > endFrame)
            continue;
        const float x = frameToX(frame);
        const bool selectedKey = ContainsTime(selectedTransformTimes, time, 1e-4f);
        DrawDiamond(
            drawList,
            ImVec2(x, transformKeyY),
            selectedKey ? 5.f : 3.5f,
            selectedKey
                ? ImGui::GetColorU32(GetEditorColors().Accent)
                : ImGui::GetColorU32(ImGuiCol_TextDisabled));
    }
    for (float time : visibilityTimes)
    {
        const float frame = time * fps;
        if (frame < startFrame || frame > endFrame)
            continue;
        const float x = frameToX(frame);
        const bool selectedKey = ContainsTime(selectedVisibilityTimes, time, 1e-4f);
        DrawDiamond(
            drawList,
            ImVec2(x, visibilityKeyY),
            selectedKey ? 5.f : 3.5f,
            selectedKey
                ? IM_COL32(230, 160, 70, 255)
                : IM_COL32(140, 110, 70, 220));
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

    if (transformTimes.empty() && visibilityTimes.empty())
    {
        drawList->AddText(
            ImVec2(canvasPos.x + 12.f, canvasMax.y - ImGui::GetTextLineHeight() - 8.f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            "Select an entity, set a frame, then Insert Transform or Visibility key.");
    }
    else
    {
        drawList->AddText(
            ImVec2(canvasPos.x + 8.f, transformKeyY - 14.f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            "TRS");
        drawList->AddText(
            ImVec2(canvasPos.x + 8.f, visibilityKeyY - 14.f),
            IM_COL32(180, 140, 80, 220),
            "Vis");
    }

    ImGui::End();
}

} // namespace caustica::editor
