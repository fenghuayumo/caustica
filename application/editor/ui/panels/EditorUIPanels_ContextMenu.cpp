#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "common/EditorIcons.h"
#include "common/IconsMaterialSymbols.h"

#include <engine/SceneQuery.h>
#include <imgui.h>

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{
namespace
{

constexpr float kCreateCardW = 68.f;
constexpr float kCreateCardH = 62.f;
constexpr float kCreateCardGap = 6.f;

ImU32 ColorWithAlpha(ImU32 col, int alpha)
{
    return (col & 0x00FFFFFFu) | (ImU32(alpha) << 24);
}

bool CreateCard(
    const char* id,
    EditorGlyphIcon icon,
    const char* label,
    ImU32 accent)
{
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(kCreateCardW, kCreateCardH);
    const bool pressed = ImGui::InvisibleButton("##card", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const ImU32 fill = active
        ? ColorWithAlpha(accent, 48)
        : hovered ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 10);
    dl->AddRectFilled(p0, p1, fill, 8.f);
    dl->AddRect(
        p0,
        p1,
        hovered ? accent : IM_COL32(255, 255, 255, 22),
        8.f,
        0,
        hovered ? 1.35f : 1.0f);

    const float iconSize = 28.f;
    const ImVec2 iconMin(p0.x + (size.x - iconSize) * 0.5f, p0.y + 8.f);
    DrawEditorGlyphIcon(
        dl,
        iconMin,
        ImVec2(iconMin.x + iconSize, iconMin.y + iconSize),
        icon,
        hovered ? accent : ImGui::GetColorU32(ImGuiCol_Text));

    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(
        ImVec2(p0.x + (size.x - ts.x) * 0.5f, p1.y - ts.y - 7.f),
        ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled),
        label);

    ImGui::PopID();
    return pressed;
}

bool MenuRow(const char* id, const char* iconUtf8, const char* label, const char* shortcut, bool enabled)
{
    ImGui::PushID(id);
    if (!enabled)
        ImGui::BeginDisabled();

    const float height = ImGui::GetFrameHeight();
    const bool pressed = ImGui::Selectable("##row", false, 0, ImVec2(0.f, height));
    const ImVec2 r0 = ImGui::GetItemRectMin();
    const ImVec2 r1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float iconX = r0.x + 8.f;
    if (iconUtf8 && iconUtf8[0] != '\0')
    {
        const ImVec2 ts = ImGui::CalcTextSize(iconUtf8);
        dl->AddText(
            ImVec2(iconX, r0.y + (height - ts.y) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_Text),
            iconUtf8);
    }

    dl->AddText(
        ImVec2(r0.x + 28.f, r0.y + (height - ImGui::GetTextLineHeight()) * 0.5f),
        ImGui::GetColorU32(ImGuiCol_Text),
        label);

    if (shortcut && shortcut[0] != '\0')
    {
        const ImVec2 ts = ImGui::CalcTextSize(shortcut);
        dl->AddText(
            ImVec2(r1.x - ts.x - 8.f, r0.y + (height - ts.y) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            shortcut);
    }

    if (!enabled)
        ImGui::EndDisabled();
    ImGui::PopID();
    return pressed && enabled;
}

void SectionLabel(const char* label)
{
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, GetEditorColors().TextMuted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.f, 2.f));
}

} // namespace

void TryOpenSceneCreatePopupOnRightClick(const char* popupId, bool areaHovered)
{
    if (!areaHovered || ImGui::IsPopupOpen(popupId))
        return;

    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)
        && !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 5.f)
        && io.MouseDownDurationPrev[ImGuiMouseButton_Right] >= 0.f
        && io.MouseDownDurationPrev[ImGuiMouseButton_Right] < 0.35f)
    {
        ImGui::OpenPopup(popupId);
    }
}

void BuildSceneCreatePopup(SceneEditor& sceneEditor, EditorUIData& ui, const char* popupId)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kCreateCardGap, 6.f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.10f, 0.11f, 0.13f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.10f));

    if (!ImGui::BeginPopup(popupId, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
    {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        return;
    }

    const bool sceneReady = sceneEditor.app()
        && caustica::isSceneLoaded(*sceneEditor.app())
        && !caustica::isSceneStructureBusy(*sceneEditor.app());

    ImGui::PushStyleColor(ImGuiCol_Text, GetEditorColors().TextMuted);
    ImGui::TextUnformatted("Add to scene");
    ImGui::PopStyleColor();

    SectionLabel("Mesh");
    ImGui::BeginDisabled(!sceneReady);
    if (CreateCard("cube", EditorGlyphIcon::Cube, "Cube", IM_COL32(92, 168, 255, 255)))
    {
        sceneEditor.requestCreateBuiltinMesh(BuiltinPrimitiveKind::Cube);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.f, kCreateCardGap);
    if (CreateCard("sphere", EditorGlyphIcon::Sphere, "Sphere", IM_COL32(240, 140, 88, 255)))
    {
        sceneEditor.requestCreateBuiltinMesh(BuiltinPrimitiveKind::Sphere);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.f, kCreateCardGap);
    if (CreateCard("plane", EditorGlyphIcon::Plane, "Plane", IM_COL32(186, 186, 168, 255)))
    {
        sceneEditor.requestCreateBuiltinMesh(BuiltinPrimitiveKind::Plane);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.f, kCreateCardGap);
    if (CreateCard("cyl", EditorGlyphIcon::Cylinder, "Cylinder", IM_COL32(88, 196, 140, 255)))
    {
        sceneEditor.requestCreateBuiltinMesh(BuiltinPrimitiveKind::Cylinder);
        ImGui::CloseCurrentPopup();
    }

    SectionLabel("Light");
    if (CreateCard("dir", EditorGlyphIcon::DirectionalLight, "Sun", IM_COL32(255, 206, 92, 255)))
    {
        sceneEditor.requestCreateLight(EditorLightKind::Directional);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.f, kCreateCardGap);
    if (CreateCard("point", EditorGlyphIcon::PointLight, "Point", IM_COL32(255, 176, 82, 255)))
    {
        sceneEditor.requestCreateLight(EditorLightKind::Point);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.f, kCreateCardGap);
    if (CreateCard("spot", EditorGlyphIcon::SpotLight, "Spot", IM_COL32(255, 158, 72, 255)))
    {
        sceneEditor.requestCreateLight(EditorLightKind::Spot);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.f, kCreateCardGap);
    if (CreateCard("rect", EditorGlyphIcon::RectLight, "Area", IM_COL32(186, 158, 255, 255)))
    {
        sceneEditor.requestCreateLight(EditorLightKind::Rect);
        ImGui::CloseCurrentPopup();
    }
    if (CreateCard("env", EditorGlyphIcon::EnvironmentLight, "Sky", IM_COL32(110, 176, 255, 255)))
    {
        sceneEditor.requestCreateLight(EditorLightKind::Environment);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    auto* ew = sceneEditor.app() ? caustica::entityWorld(*sceneEditor.app()) : nullptr;
    const ecs::Entity selected = ui.editor.SelectedEntity;
    const bool canDelete = ew
        && selected != ecs::NullEntity
        && selected != ew->root()
        && ew->world().isAlive(selected)
        && ui.editor.PendingDeleteEntity == ecs::NullEntity;

    if (MenuRow("delete", ICON_MS_DELETE, "Delete", "Del", canDelete))
    {
        ui.editor.PendingDeleteEntity = selected;
        ui.editor.SelectedEntity = ecs::NullEntity;
        ui.editor.SelectedMaterial = nullptr;
        ui.editor.InspectorRotationEntity = ecs::NullEntity;
        ui.editor.InspectorRotationEulerValid = false;
        ui.editor.SelectedGaussianSplat = false;
        ImGui::CloseCurrentPopup();
    }

    const bool canSave = sceneEditor.canSaveScene();
    const bool canSaveAs = sceneEditor.app()
        && caustica::isSceneLoaded(*sceneEditor.app())
        && sceneEditor.editorState().sceneDocumentValid;
    if (MenuRow("save", ICON_MS_SAVE, "Save Scene", "Ctrl+S", canSave || canSaveAs))
    {
        sceneEditor.requestSaveScene();
        ImGui::CloseCurrentPopup();
    }
    if (MenuRow("saveas", ICON_MS_SAVE, "Save Scene As...", nullptr, canSaveAs))
    {
        sceneEditor.requestSaveSceneAsFromDialog();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

} // namespace caustica::editor
