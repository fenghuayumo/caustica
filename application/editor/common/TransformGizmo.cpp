#include "common/TransformGizmo.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
#include "EditorUndoCommands.h"
#include <engine/SceneQuery.h>
#include <engine/CameraApi.h>
#include "ui/EditorUIInternal.h"

#include <ImGuizmo.h>
#include "common/imoguizmo.hpp"
#include "common/EditorIcons.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <math/affine.h>
#include <math/quat.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
#include <scene/View.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

using namespace caustica;
using namespace caustica::editor;
using namespace caustica::math;

namespace
{

// Working matrix while the gizmo is being dragged. Reloading from ECS every frame
// after decompose/recompose causes visible jitter.
struct GizmoDragState
{
    ecs::Entity entity = ecs::NullEntity;
    float matrix[16] = {};
    bool active = false;
    // After CancelTransformGizmoEdit, ignore writeback until ImGuizmo releases.
    bool suppressUntilRelease = false;
};

struct GizmoUndoState
{
    bool tracking = false;
    bool changed = false;
    ecs::Entity entity = ecs::NullEntity;
    LocalTransformSnapshot before;
};

GizmoDragState g_drag;
GizmoUndoState g_undo;

void ResetGizmoUndoState()
{
    g_undo = {};
}

void ResetGizmoDragState()
{
    g_drag = {};
}

// Match the layout that worked before DockSpace: caustica/ImGuizmo both consume
// this buffer as the same row-major affine dump (see commit 5240cfa9).
void Affine3ToImGuizmoMatrix(const dm::affine3& affine, float matrix[16])
{
    matrix[0] = affine.m_linear.m00;
    matrix[1] = affine.m_linear.m01;
    matrix[2] = affine.m_linear.m02;
    matrix[3] = 0.f;
    matrix[4] = affine.m_linear.m10;
    matrix[5] = affine.m_linear.m11;
    matrix[6] = affine.m_linear.m12;
    matrix[7] = 0.f;
    matrix[8] = affine.m_linear.m20;
    matrix[9] = affine.m_linear.m21;
    matrix[10] = affine.m_linear.m22;
    matrix[11] = 0.f;
    matrix[12] = affine.m_translation.x;
    matrix[13] = affine.m_translation.y;
    matrix[14] = affine.m_translation.z;
    matrix[15] = 1.f;
}

void Float4x4ToImGuizmoMatrix(const dm::float4x4& source, float matrix[16])
{
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            matrix[row * 4 + col] = source[row][col];
}

// Same row-major product ImGuizmo uses for view * projection.
void MultiplyImGuizmoMatrix(const float a[16], const float b[16], float r[16])
{
    float tmp[16];
    tmp[0] = a[0] * b[0] + a[1] * b[4] + a[2] * b[8] + a[3] * b[12];
    tmp[1] = a[0] * b[1] + a[1] * b[5] + a[2] * b[9] + a[3] * b[13];
    tmp[2] = a[0] * b[2] + a[1] * b[6] + a[2] * b[10] + a[3] * b[14];
    tmp[3] = a[0] * b[3] + a[1] * b[7] + a[2] * b[11] + a[3] * b[15];
    tmp[4] = a[4] * b[0] + a[5] * b[4] + a[6] * b[8] + a[7] * b[12];
    tmp[5] = a[4] * b[1] + a[5] * b[5] + a[6] * b[9] + a[7] * b[13];
    tmp[6] = a[4] * b[2] + a[5] * b[6] + a[6] * b[10] + a[7] * b[14];
    tmp[7] = a[4] * b[3] + a[5] * b[7] + a[6] * b[11] + a[7] * b[15];
    tmp[8] = a[8] * b[0] + a[9] * b[4] + a[10] * b[8] + a[11] * b[12];
    tmp[9] = a[8] * b[1] + a[9] * b[5] + a[10] * b[9] + a[11] * b[13];
    tmp[10] = a[8] * b[2] + a[9] * b[6] + a[10] * b[10] + a[11] * b[14];
    tmp[11] = a[8] * b[3] + a[9] * b[7] + a[10] * b[11] + a[11] * b[15];
    tmp[12] = a[12] * b[0] + a[13] * b[4] + a[14] * b[8] + a[15] * b[12];
    tmp[13] = a[12] * b[1] + a[13] * b[5] + a[14] * b[9] + a[15] * b[13];
    tmp[14] = a[12] * b[2] + a[13] * b[6] + a[14] * b[10] + a[15] * b[14];
    tmp[15] = a[12] * b[3] + a[13] * b[7] + a[14] * b[11] + a[15] * b[15];
    std::memcpy(r, tmp, sizeof(tmp));
}

bool ProjectWorldWithGizmo(const dm::float3& world, const float viewProj[16], const EditorViewportState& vp, ImVec2& out)
{
    // ImGuizmo::worldToPos: row-vector TransformPoint, then Y-flip into the viewport rect.
    const float x = world.x;
    const float y = world.y;
    const float z = world.z;
    const float clipX = x * viewProj[0] + y * viewProj[4] + z * viewProj[8] + viewProj[12];
    const float clipY = x * viewProj[1] + y * viewProj[5] + z * viewProj[9] + viewProj[13];
    const float clipW = x * viewProj[3] + y * viewProj[7] + z * viewProj[11] + viewProj[15];
    if (clipW <= 1e-4f)
        return false;
    const float invW = 0.5f / clipW;
    const float ndcX = clipX * invW + 0.5f;
    const float ndcY = 1.f - (clipY * invW + 0.5f);
    // Near-plane / off-axis points explode to huge NDC and dash-fill the viewport.
    if (ndcX < -2.f || ndcX > 3.f || ndcY < -2.f || ndcY > 3.f)
        return false;
    out.x = vp.PosX + ndcX * vp.SizeX;
    out.y = vp.PosY + ndcY * vp.SizeY;
    return true;
}

bool ClipSegmentToRect(ImVec2& a, ImVec2& b, ImVec2 min, ImVec2 max)
{
    auto outCode = [&](const ImVec2& p) -> int
    {
        int code = 0;
        if (p.x < min.x) code |= 1;
        if (p.x > max.x) code |= 2;
        if (p.y < min.y) code |= 4;
        if (p.y > max.y) code |= 8;
        return code;
    };

    int codeA = outCode(a);
    int codeB = outCode(b);
    for (;;)
    {
        if ((codeA | codeB) == 0)
            return true;
        if ((codeA & codeB) != 0)
            return false;
        const int code = codeA ? codeA : codeB;
        ImVec2 p;
        if (code & 8)
        {
            p.x = a.x + (b.x - a.x) * (max.y - a.y) / (b.y - a.y);
            p.y = max.y;
        }
        else if (code & 4)
        {
            p.x = a.x + (b.x - a.x) * (min.y - a.y) / (b.y - a.y);
            p.y = min.y;
        }
        else if (code & 2)
        {
            p.y = a.y + (b.y - a.y) * (max.x - a.x) / (b.x - a.x);
            p.x = max.x;
        }
        else
        {
            p.y = a.y + (b.y - a.y) * (min.x - a.x) / (b.x - a.x);
            p.x = min.x;
        }
        if (code == codeA)
        {
            a = p;
            codeA = outCode(a);
        }
        else
        {
            b = p;
            codeB = outCode(b);
        }
    }
}

void BasisFromAxis(const dm::float3& axis, dm::float3& tangent, dm::float3& bitangent)
{
    const dm::float3 n = dm::normalize(axis);
    tangent = dm::normalize(dm::orthogonal(n));
    bitangent = dm::normalize(dm::cross(n, tangent));
}

void DashedScreenLine(ImDrawList* drawList, ImVec2 a, ImVec2 b, ImU32 col, float thickness, const EditorViewportState& vp)
{
    if (!ClipSegmentToRect(a, b, ImVec2(vp.PosX, vp.PosY), ImVec2(vp.PosX + vp.SizeX, vp.PosY + vp.SizeY)))
        return;
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.f || len > 4000.f)
        return;
    const float dash = 7.f;
    const float gap = 4.f;
    const ImVec2 dir(dx / len, dy / len);
    for (float t = 0.f; t < len; t += dash + gap)
    {
        const float t1 = std::min(t + dash, len);
        drawList->AddLine(
            ImVec2(a.x + dir.x * t, a.y + dir.y * t),
            ImVec2(a.x + dir.x * t1, a.y + dir.y * t1),
            col,
            thickness);
    }
}

void DashedWorldLine(
    ImDrawList* drawList,
    const float viewProj[16],
    const EditorViewportState& vp,
    const dm::float3& a,
    const dm::float3& b,
    ImU32 col,
    float thickness)
{
    ImVec2 sa, sb;
    if (ProjectWorldWithGizmo(a, viewProj, vp, sa) && ProjectWorldWithGizmo(b, viewProj, vp, sb))
        DashedScreenLine(drawList, sa, sb, col, thickness, vp);
}

void DashedWorldCircle(
    ImDrawList* drawList,
    const float viewProj[16],
    const EditorViewportState& vp,
    const dm::float3& center,
    const dm::float3& axisX,
    const dm::float3& axisY,
    float radius,
    ImU32 col,
    float thickness,
    int segments = 32)
{
    ImVec2 prev{};
    bool prevOk = false;
    for (int i = 0; i <= segments; ++i)
    {
        const float a = (dm::PI_f * 2.f * float(i)) / float(segments);
        const dm::float3 world = center + axisX * (std::cos(a) * radius) + axisY * (std::sin(a) * radius);
        ImVec2 screen;
        const bool ok = ProjectWorldWithGizmo(world, viewProj, vp, screen);
        if (ok && prevOk)
            DashedScreenLine(drawList, prev, screen, col, thickness, vp);
        prev = screen;
        prevOk = ok;
    }
}

void DrawViewportLightIcon(ImDrawList* drawList, ImVec2 center, EditorGlyphIcon kind, ImU32 col, float size)
{
    const float half = size * 0.5f;
    drawList->AddCircleFilled(center, half + 3.f, IM_COL32(10, 12, 16, 170), 20);
    DrawEditorGlyphIcon(
        drawList,
        ImVec2(center.x - half, center.y - half),
        ImVec2(center.x + half, center.y + half),
        kind,
        col);
}

dm::affine3 ImGuizmoMatrixToAffine3(const float matrix[16])
{
    dm::affine3 result = dm::affine3::identity();
    result.m_linear.m00 = matrix[0];
    result.m_linear.m01 = matrix[1];
    result.m_linear.m02 = matrix[2];
    result.m_linear.m10 = matrix[4];
    result.m_linear.m11 = matrix[5];
    result.m_linear.m12 = matrix[6];
    result.m_linear.m20 = matrix[8];
    result.m_linear.m21 = matrix[9];
    result.m_linear.m22 = matrix[10];
    result.m_translation.x = matrix[12];
    result.m_translation.y = matrix[13];
    result.m_translation.z = matrix[14];
    return result;
}

dm::daffine3 GetParentGlobalTransform(const caustica::scene::SceneEntityWorld& entityWorld, ecs::Entity entity)
{
    const auto* parentComp = entityWorld.world().tryGet<caustica::scene::ParentComponent>(entity);
    if (!parentComp || !ecs::isValid(parentComp->parent))
        return dm::daffine3::identity();

    const auto* parentGlobal = entityWorld.world().tryGet<caustica::scene::GlobalTransformComponent>(parentComp->parent);
    return parentGlobal ? parentGlobal->transform : dm::daffine3::identity();
}

void ApplyWorldMatrixToLocalTransform(
    caustica::scene::SceneEntityWorld& entityWorld,
    ecs::Entity entity,
    const float worldMatrix[16])
{
    const dm::daffine3 parentWorld = GetParentGlobalTransform(entityWorld, entity);
    const dm::daffine3 newWorld(dm::affine3(ImGuizmoMatrixToAffine3(worldMatrix)));
    const dm::daffine3 newLocal = newWorld * dm::daffine3(inverse(parentWorld));

    dm::double3 translation;
    dm::dquat rotation;
    dm::double3 scaling;
    dm::decomposeAffine(newLocal, &translation, &rotation, &scaling);
    entityWorld.setLocalTransform(entity, &translation, &rotation, &scaling);
    // CaptureCurrent: per-frame motion vectors. PreserveExisting (drag-start previous)
    // makes temporal denoise / accumulation flicker while dragging.
    entityWorld.refreshHierarchy(caustica::scene::PreviousTransformPolicy::CaptureCurrent);
}

void HandleTransformGizmoShortcuts(EditorUIState& editorUI)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard || ImGui::IsAnyItemActive())
        return;
    // Avoid stealing Ctrl/Alt/Super chords (e.g. Ctrl+R shader reload).
    if (io.KeyCtrl || io.KeyAlt || io.KeySuper)
        return;
    // RMB + WASD/QE fly mode owns Q/S — don't switch tools while flying.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
        return;

    if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
        editorUI.GizmoEnabled = false;
    if (ImGui::IsKeyPressed(ImGuiKey_T, false))
    {
        editorUI.GizmoEnabled = true;
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::TRANSLATE);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false))
    {
        editorUI.GizmoEnabled = true;
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::ROTATE);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S, false))
    {
        editorUI.GizmoEnabled = true;
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::SCALE);
    }
}

void HandleLightHelperShortcut(EditorUIState& editorUI)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard || ImGui::IsAnyItemActive())
        return;
    if (io.KeyCtrl || io.KeyAlt || io.KeySuper)
        return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_G, false))
        editorUI.ShowLightHelpers = !editorUI.ShowLightHelpers;
}

const float* GetSnapValues(const EditorUIState& editorUI)
{
    if (!editorUI.GizmoSnapEnabled)
        return nullptr;

    switch (static_cast<ImGuizmo::OPERATION>(editorUI.GizmoOperation))
    {
    case ImGuizmo::TRANSLATE:
        return editorUI.GizmoSnapTranslation;
    case ImGuizmo::ROTATE:
        return &editorUI.GizmoSnapRotation;
    case ImGuizmo::SCALE:
        return &editorUI.GizmoSnapScale;
    default:
        return nullptr;
    }
}

void BuildGizmoProjectionMatrix(const TransformGizmoContext& ctx, const PlanarView& view, float outMatrix[16])
{
    auto* camera = caustica::editor::editorCamera(ctx.sceneEditor);
    if (camera && view.isReverseDepth())
    {
        const auto& vp = ctx.editorUI.Viewport;
        float aspect = 1.f;
        if (vp.RectValid && vp.SizeY > 1.f)
            aspect = vp.SizeX / vp.SizeY;
        else
        {
            const ImGuiIO& io = ImGui::GetIO();
            aspect = (io.DisplaySize.y > 0.f) ? (io.DisplaySize.x / io.DisplaySize.y) : 1.f;
        }
        const float fov = camera->verticalFOV();
        const float zNear = std::max(camera->zNear(), 0.01f);
        const float zFar = std::max(zNear * 10000.f, 1000.f);
        Float4x4ToImGuizmoMatrix(dm::perspProjD3DStyle(fov, aspect, zNear, zFar), outMatrix);
        return;
    }

    Float4x4ToImGuizmoMatrix(view.getProjectionMatrix(false), outMatrix);
}

bool IsEditingInspectorUi()
{
    // Only block gizmo *writeback* when a side-panel widget is active.
    // Viewport canvas / gizmo toolbar also set ActiveId — those must not gray-out
    // the gizmo (ImGuizmo::Enable(false) draws the inactive gray style).
    if (!ImGui::IsAnyItemActive())
        return false;

    ImGuiContext* g = ImGui::GetCurrentContext();
    ImGuiWindow* win = g ? g->ActiveIdWindow : nullptr;
    if (!win || !win->Name)
        return false;

    // "Viewport", "Viewport/##GizmoToolbarOverlay", etc.
    if (std::strncmp(win->Name, "Viewport", 8) == 0)
        return false;
    if (std::strstr(win->Name, "GizmoToolbar") != nullptr)
        return false;

    return true;
}

} // namespace

void caustica::editor::DrawInfiniteGrid(const TransformGizmoContext& ctx)
{
    if (!ctx.editorUI.ShowUI || !ctx.editorUI.ShowInfiniteGrid)
        return;

    App* app = ctx.sceneEditor.app();
    if (!app)
        return;

    const auto& view = caustica::currentView(*app);
    if (!view || view->isOrthographicProjection())
        return;

    const auto& vp = ctx.editorUI.Viewport;
    if (!vp.RectValid || vp.SizeX <= 1.f || vp.SizeY <= 1.f)
        return;

    // Draw on the Viewport window list (not the foreground list). ImGuizmo::DrawGrid
    // does not clip; foreground drawing was painting over Timeline / side panels.
    ImGuiWindow* viewportWindow = ImGui::FindWindowByName("Viewport");
    if (!viewportWindow || !viewportWindow->Active || viewportWindow->Hidden)
        return;

    // Do not call BeginFrame() here: it creates a fullscreen NoInputs "gizmo"
    // window that steals ImGui hover from Viewport and makes ImGuizmo::IsOver()
    // true across the canvas (blocking left-click pick when nothing is selected).
    ImDrawList* drawList = viewportWindow->DrawList;
    ImGuizmo::SetDrawlist(drawList);
    ImGuizmo::SetAlternativeWindow(viewportWindow);
    ImGuizmo::SetRect(vp.PosX, vp.PosY, vp.SizeX, vp.SizeY);
    ImGuizmo::SetOrthographic(false);

    float viewMatrix[16];
    float projectionMatrix[16];
    Affine3ToImGuizmoMatrix(view->getViewMatrix(), viewMatrix);
    BuildGizmoProjectionMatrix(ctx, *view, projectionMatrix);

    float identity[16] = {
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f,
    };

    const ImVec2 clipMin(vp.PosX, vp.PosY);
    const ImVec2 clipMax(vp.PosX + vp.SizeX, vp.PosY + vp.SizeY);
    drawList->PushClipRect(clipMin, clipMax, true);
    // Large finite grid on Y=0 (ImGuizmo DrawGrid); feels infinite in typical editor framing.
    ImGuizmo::DrawGrid(viewMatrix, projectionMatrix, identity, 120.f);
    drawList->PopClipRect();
}

void caustica::editor::DrawLightHelpers(const TransformGizmoContext& ctx)
{
    if (!ctx.editorUI.ShowUI)
        return;

    HandleLightHelperShortcut(ctx.editorUI);
    if (!ctx.editorUI.ShowLightHelpers)
        return;

    App* app = ctx.sceneEditor.app();
    auto* ew = app ? caustica::entityWorld(*app) : nullptr;
    const auto& view = app ? caustica::currentView(*app) : nullptr;
    if (!ew || !view)
        return;

    const auto& vp = ctx.editorUI.Viewport;
    if (!vp.RectValid || vp.SizeX <= 1.f || vp.SizeY <= 1.f)
        return;

    ImGuiWindow* viewportWindow = ImGui::FindWindowByName("Viewport");
    if (!viewportWindow || !viewportWindow->Active || viewportWindow->Hidden)
        return;

    ImDrawList* drawList = viewportWindow->DrawList;
    const ImVec2 clipMin(vp.PosX, vp.PosY);
    const ImVec2 clipMax(vp.PosX + vp.SizeX, vp.PosY + vp.SizeY);
    drawList->PushClipRect(clipMin, clipMax, true);

    // Match DrawInfiniteGrid / the transform gizmo. Reverse-Z viewProj plus
    // clip.z < 0 rejects visible corners and shifts the overlay off the mesh.
    float viewMatrix[16];
    float projectionMatrix[16];
    float viewProj[16];
    Affine3ToImGuizmoMatrix(view->getViewMatrix(), viewMatrix);
    BuildGizmoProjectionMatrix(ctx, *view, projectionMatrix);
    MultiplyImGuizmoMatrix(viewMatrix, projectionMatrix, viewProj);

    auto line = [&](const dm::float3& a, const dm::float3& b, ImU32 col, float thickness)
    {
        DashedWorldLine(drawList, viewProj, vp, a, b, col, thickness);
    };
    auto circle = [&](const dm::float3& center, const dm::float3& axisX, const dm::float3& axisY,
        float radius, ImU32 col, float thickness)
    {
        DashedWorldCircle(drawList, viewProj, vp, center, axisX, axisY, radius, col, thickness);
    };
    auto wireSphere = [&](const dm::float3& center, float radius, ImU32 col, float thickness)
    {
        circle(center, dm::float3(1.f, 0.f, 0.f), dm::float3(0.f, 1.f, 0.f), radius, col, thickness);
        circle(center, dm::float3(1.f, 0.f, 0.f), dm::float3(0.f, 0.f, 1.f), radius, col, thickness);
        circle(center, dm::float3(0.f, 1.f, 0.f), dm::float3(0.f, 0.f, 1.f), radius, col, thickness);
    };
    auto cone = [&](const dm::float3& origin, const dm::float3& dir, float length, float halfAngleDeg,
        ImU32 col, float thickness)
    {
        const float clamped = std::clamp(halfAngleDeg, 1.f, 80.f);
        const float radius = std::tan(dm::radians(clamped)) * length;
        dm::float3 tangent, bitangent;
        BasisFromAxis(dir, tangent, bitangent);
        const dm::float3 end = origin + dir * length;
        circle(end, tangent, bitangent, radius, col, thickness);
        constexpr int kRays = 4;
        for (int i = 0; i < kRays; ++i)
        {
            const float a = (dm::PI_f * 2.f * float(i)) / float(kRays);
            const dm::float3 rim = end + tangent * (std::cos(a) * radius) + bitangent * (std::sin(a) * radius);
            line(origin, rim, col, thickness);
        }
    };

    struct PendingIcon
    {
        ImVec2 screen;
        EditorGlyphIcon kind;
        ImU32 col;
        float size;
    };
    std::vector<PendingIcon> icons;

    auto queueIcon = [&](const dm::float3& origin, EditorGlyphIcon kind, ImU32 col, bool selected)
    {
        ImVec2 screen;
        if (!ProjectWorldWithGizmo(origin, viewProj, vp, screen))
            return;
        if (screen.x < vp.PosX || screen.y < vp.PosY
            || screen.x > vp.PosX + vp.SizeX || screen.y > vp.PosY + vp.SizeY)
            return;
        icons.push_back({ screen, kind, col, selected ? 26.f : 22.f });
    };

    auto style = [&](ecs::Entity entity, ImU32 idle) -> std::pair<ImU32, float>
    {
        const bool selected = ctx.editorUI.SelectedEntity == entity;
        return {
            selected ? IM_COL32(255, 230, 120, 255) : idle,
            selected ? 2.2f : 1.35f
        };
    };

    ew->world().each<scene::DirectionalLightComponent, scene::GlobalTransformComponent>(
        [&](ecs::Entity entity, scene::DirectionalLightComponent& light, scene::GlobalTransformComponent& global)
        {
            if (!light.enabled)
                return;
            const auto [col, thickness] = style(entity, IM_COL32(255, 206, 92, 210));
            const dm::float3 origin = global.transformFloat.m_translation;
            dm::float3 dir = dm::float3(scene::getLightDirection(global.transform));
            if (dm::length(dir) < 1e-5f)
                dir = dm::float3(0.f, -1.f, 0.f);
            dir = dm::normalize(dir);
            dm::float3 tangent, bitangent;
            BasisFromAxis(dir, tangent, bitangent);
            const bool selected = ctx.editorUI.SelectedEntity == entity;
            if (selected)
            {
                circle(origin, tangent, bitangent, 0.22f, col, thickness);
                constexpr int kRays = 8;
                for (int i = 0; i < kRays; ++i)
                {
                    const float a = (dm::PI_f * 2.f * float(i)) / float(kRays);
                    const dm::float3 radial = tangent * std::cos(a) + bitangent * std::sin(a);
                    line(origin + radial * 0.22f, origin + radial * 0.42f, col, thickness);
                }
                line(origin, origin + dir * 0.7f, col, thickness);
            }
            queueIcon(origin, EditorGlyphIcon::DirectionalLight, col, selected);
        });

    ew->world().each<scene::PointLightComponent, scene::GlobalTransformComponent>(
        [&](ecs::Entity entity, scene::PointLightComponent& light, scene::GlobalTransformComponent& global)
        {
            if (!light.enabled)
                return;
            const bool selected = ctx.editorUI.SelectedEntity == entity;
            const auto [col, thickness] = style(entity, IM_COL32(255, 176, 82, 210));
            const dm::float3 origin = global.transformFloat.m_translation;
            const float coreRadius = std::max(light.radius, 0.18f);
            if (selected)
            {
                wireSphere(origin, coreRadius, col, thickness);
                if (light.range > coreRadius + 0.05f)
                    wireSphere(origin, std::min(light.range, 8.f), IM_COL32(255, 230, 120, 140), 1.15f);
            }
            queueIcon(origin, EditorGlyphIcon::PointLight, col, selected);
        });

    ew->world().each<scene::SpotLightComponent, scene::GlobalTransformComponent>(
        [&](ecs::Entity entity, scene::SpotLightComponent& light, scene::GlobalTransformComponent& global)
        {
            if (!light.enabled)
                return;
            const bool selected = ctx.editorUI.SelectedEntity == entity;
            const auto [col, thickness] = style(entity, IM_COL32(255, 158, 72, 210));
            const dm::float3 origin = global.transformFloat.m_translation;
            dm::float3 dir = dm::float3(scene::getLightDirection(global.transform));
            if (dm::length(dir) < 1e-5f)
                dir = dm::float3(0.f, -1.f, 0.f);
            dir = dm::normalize(dir);
            const float fullLen = light.range > 1e-3f ? light.range : 2.5f;
            const float coneLen = std::min(selected ? fullLen : 1.6f, 6.f);
            if (selected)
            {
                cone(origin, dir, coneLen, light.outerAngle, col, thickness);
                if (light.innerAngle > 1.f && light.innerAngle < light.outerAngle - 0.5f)
                    cone(origin, dir, coneLen, light.innerAngle, IM_COL32(255, 230, 120, 150), 1.15f);
            }
            queueIcon(origin, EditorGlyphIcon::SpotLight, col, selected);
        });

    ew->world().each<scene::RectLightComponent, scene::GlobalTransformComponent>(
        [&](ecs::Entity entity, scene::RectLightComponent& light, scene::GlobalTransformComponent& global)
        {
            if (!light.enabled)
                return;
            const bool selected = ctx.editorUI.SelectedEntity == entity;
            const auto [col, thickness] = style(entity, IM_COL32(186, 158, 255, 210));

            dm::float3 local[4] = {
                { -0.5f, -0.5f, 0.0f },
                { -0.5f,  0.5f, 0.0f },
                {  0.5f,  0.5f, 0.0f },
                {  0.5f, -0.5f, 0.0f },
            };
            if (const auto* meshComp = ew->world().tryGet<scene::MeshInstanceComponent>(entity))
            {
                const auto& mesh = meshComp->mesh;
                if (mesh && mesh->buffers && mesh->buffers->positionData.size() >= 4)
                {
                    for (int i = 0; i < 4; ++i)
                        local[i] = mesh->buffers->positionData[static_cast<size_t>(i)];
                }
            }

            dm::float3 world[4];
            for (int i = 0; i < 4; ++i)
                world[i] = global.transformFloat.transformPoint(local[i]);
            line(world[0], world[1], col, thickness);
            line(world[1], world[2], col, thickness);
            line(world[2], world[3], col, thickness);
            line(world[3], world[0], col, thickness);

            const dm::float3 origin = global.transformFloat.m_translation;
            const dm::float3 tip = global.transformFloat.transformPoint(dm::float3(0.f, 0.f, -0.35f));
            line(origin, tip, col, thickness);
            queueIcon(origin, EditorGlyphIcon::RectLight, col, selected);
        });

    ew->world().each<scene::EnvironmentLightComponent, scene::GlobalTransformComponent>(
        [&](ecs::Entity entity, scene::EnvironmentLightComponent& light, scene::GlobalTransformComponent& global)
        {
            if (!light.enabled)
                return;
            const bool selected = ctx.editorUI.SelectedEntity == entity;
            const ImU32 col = selected ? IM_COL32(255, 230, 120, 255) : IM_COL32(110, 176, 255, 210);
            queueIcon(global.transformFloat.m_translation, EditorGlyphIcon::EnvironmentLight, col, selected);
        });

    for (const PendingIcon& icon : icons)
        DrawViewportLightIcon(drawList, icon.screen, icon.kind, icon.col, icon.size);

    drawList->PopClipRect();
}

bool caustica::editor::IsTransformGizmoEditing()
{
    return g_undo.tracking || g_drag.active || g_drag.suppressUntilRelease || ImGuizmo::IsUsing();
}

void caustica::editor::ResetTransformGizmoInteraction()
{
    ResetGizmoUndoState();
    ResetGizmoDragState();
}

bool caustica::editor::CancelTransformGizmoEdit(SceneEditor& sceneEditor)
{
    if (!g_undo.tracking && !g_drag.active && !g_drag.suppressUntilRelease && !ImGuizmo::IsUsing())
        return false;

    if (g_undo.tracking && ecs::isValid(g_undo.entity))
        applyLocalTransform(sceneEditor, g_undo.entity, g_undo.before);

    ResetGizmoUndoState();
    g_drag.active = false;
    g_drag.suppressUntilRelease = ImGuizmo::IsUsing();
    if (!g_drag.suppressUntilRelease)
        ResetGizmoDragState();
    return true;
}

bool caustica::editor::DrawTransformGizmo(const TransformGizmoContext& ctx)
{
    ctx.editorUI.GizmoCapturingInput = false;

    if (!ctx.editorUI.ShowTransformGizmo || !ctx.editorUI.ShowUI)
    {
        if (g_undo.tracking && g_undo.changed)
        {
            if (App* app = ctx.sceneEditor.app())
            {
                if (auto* ew = caustica::entityWorld(*app))
                {
                    ctx.sceneEditor.commitTransformEdit(
                        g_undo.entity,
                        g_undo.before,
                        captureLocalTransform(*ew, g_undo.entity));
                }
            }
        }
        ResetGizmoUndoState();
        ResetGizmoDragState();
        return false;
    }

    HandleTransformGizmoShortcuts(ctx.editorUI);

    auto* entityWorld = caustica::entityWorld(*ctx.sceneEditor.app());
    const ecs::Entity entity = ctx.editorUI.SelectedEntity;
    if (!entityWorld || entity == ecs::NullEntity)
    {
        ResetGizmoUndoState();
        ResetGizmoDragState();
        return false;
    }

    auto* localTransform = entityWorld->world().tryGet<caustica::scene::LocalTransformComponent>(entity);
    auto* globalTransform = entityWorld->world().tryGet<caustica::scene::GlobalTransformComponent>(entity);
    if (!localTransform || !globalTransform)
    {
        ResetGizmoUndoState();
        ResetGizmoDragState();
        return false;
    }

    const auto& view = caustica::currentView(*ctx.sceneEditor.app());
    if (!view)
    {
        ResetGizmoUndoState();
        ResetGizmoDragState();
        return false;
    }

    // Select tool: do not draw the inactive gray gizmo at all.
    if (!ctx.editorUI.GizmoEnabled)
    {
        if (g_undo.tracking && g_undo.changed)
        {
            ctx.sceneEditor.commitTransformEdit(
                g_undo.entity,
                g_undo.before,
                captureLocalTransform(*entityWorld, g_undo.entity));
        }
        ResetGizmoUndoState();
        return false;
    }

    const bool editingUi = IsEditingInspectorUi();
    const bool wasTracking = g_undo.tracking;

    ImGuizmo::BeginFrame();
    {
        constexpr float kGizmoStyleScale = 1.5f;
        ImGuizmo::Style& style = ImGuizmo::GetStyle();
        style.TranslationLineThickness = 3.0f * kGizmoStyleScale;
        style.TranslationLineArrowSize = 6.0f * kGizmoStyleScale;
        style.RotationLineThickness = 2.0f * kGizmoStyleScale;
        style.RotationOuterLineThickness = 3.0f * kGizmoStyleScale;
        style.ScaleLineThickness = 3.0f * kGizmoStyleScale;
        style.ScaleLineCircleSize = 6.0f * kGizmoStyleScale;
        style.HatchedAxisLineThickness = 6.0f * kGizmoStyleScale;
        style.CenterCircleSize = 6.0f * kGizmoStyleScale;
    }

    // DockSpace covers BeginFrame's default fullscreen window. Draw on the
    // foreground list (above every docked panel / viewport image) and map hover
    // to the Viewport window for gizmo picking.
    ImGuiWindow* viewportWindow = ImGui::FindWindowByName("Viewport");
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    if (viewportWindow)
        ImGuizmo::SetAlternativeWindow(viewportWindow);

    // Keep Enable(true) whenever a transform tool is active so axes stay RGB.
    // Inspector edits only block writeback below — they must not switch ImGuizmo
    // into its gray INACTIVE draw style.
    ImGuizmo::Enable(true);
    ImGuizmo::SetID(static_cast<int>(entt::to_integral(entity)));

    ImGuiIO& io = ImGui::GetIO();
    const auto& vp = ctx.editorUI.Viewport;
    if (vp.RectValid && vp.SizeX > 1.f && vp.SizeY > 1.f)
        ImGuizmo::SetRect(vp.PosX, vp.PosY, vp.SizeX, vp.SizeY);
    else
        ImGuizmo::SetRect(0.f, 0.f, io.DisplaySize.x, io.DisplaySize.y);
    ImGuizmo::SetOrthographic(view->isOrthographicProjection());

    float viewMatrix[16];
    float projectionMatrix[16];
    Affine3ToImGuizmoMatrix(view->getViewMatrix(), viewMatrix);
    BuildGizmoProjectionMatrix(ctx, *view, projectionMatrix);

    if (g_drag.suppressUntilRelease)
    {
        // Keep the gizmo visual locked to ECS (post-cancel / post-undo) until the
        // mouse button that started the drag is released.
        Affine3ToImGuizmoMatrix(globalTransform->transformFloat, g_drag.matrix);
        g_drag.entity = entity;
        g_drag.active = false;
    }

    // Sync from ECS only when not actively dragging the gizmo. During a drag, keep the
    // working matrix so decompose/recompose round-trips cannot jitter the object.
    if (!g_drag.suppressUntilRelease
        && (!ImGuizmo::IsUsing() || !g_drag.active || g_drag.entity != entity || editingUi))
    {
        Affine3ToImGuizmoMatrix(globalTransform->transformFloat, g_drag.matrix);
        g_drag.entity = entity;
        g_drag.active = false;
    }

    const auto operation = static_cast<ImGuizmo::OPERATION>(ctx.editorUI.GizmoOperation);
    const auto mode = static_cast<ImGuizmo::MODE>(ctx.editorUI.GizmoMode);
    const bool manipulated = ImGuizmo::Manipulate(
        viewMatrix,
        projectionMatrix,
        operation,
        mode,
        g_drag.matrix,
        nullptr,
        GetSnapValues(ctx.editorUI));

    const bool usingGizmo = ImGuizmo::IsUsing() && !editingUi && !g_drag.suppressUntilRelease;
    g_drag.active = usingGizmo;

    if (g_drag.suppressUntilRelease && !ImGuizmo::IsUsing())
    {
        g_drag.suppressUntilRelease = false;
        ResetGizmoDragState();
    }

    // Capture while dragging, or while hovering a handle so the first click does not
    // fall through to camera orbit / instance picking.
    const bool overGizmo = ImGuizmo::IsOver();
    ctx.editorUI.GizmoCapturingInput =
        (usingGizmo || overGizmo || g_drag.suppressUntilRelease) && !editingUi;
    if (ctx.editorUI.GizmoCapturingInput)
        io.WantCaptureMouse = true;

    if (usingGizmo && !wasTracking)
    {
        g_undo.tracking = true;
        g_undo.changed = false;
        g_undo.entity = entity;
        g_undo.before = captureLocalTransform(*localTransform);
    }

    if (manipulated && !editingUi && !g_drag.suppressUntilRelease)
    {
        ApplyWorldMatrixToLocalTransform(*entityWorld, entity, g_drag.matrix);
        if (g_undo.tracking && g_undo.entity == entity)
            g_undo.changed = true;

        ctx.editorUI.InspectorRotationEntity = entity;
        ctx.editorUI.InspectorRotationEulerValid = false;
        // PT must drop history while the pose changes. CaptureCurrent keeps motion
        // vectors on the per-frame delta so temporal denoise does not thrash.
        ctx.settings.ResetAccumulation = true;
        if (caustica::scene::hasAnyLightComponent(entityWorld->world(), entity))
            ctx.settings.ResetRealtimeCaches = true;
    }

    if (!usingGizmo && wasTracking && !g_drag.suppressUntilRelease)
    {
        if (g_undo.changed)
        {
            ctx.sceneEditor.commitTransformEdit(
                g_undo.entity,
                g_undo.before,
                captureLocalTransform(*entityWorld, g_undo.entity));
        }
        ResetGizmoUndoState();
    }
    else if (g_undo.tracking && g_undo.entity != entity)
    {
        // Selection changed mid-drag — drop the incomplete edit session.
        ResetGizmoUndoState();
    }

    return manipulated && !editingUi && !g_drag.suppressUntilRelease;
}

namespace
{

// Dedicated square projection for the orientation widget (not the scene camera).
void BuildImOGuizmoProjectionMatrix(float outMatrix[16])
{
    // Caustica view space is LHS / D3D-style; keep the widget projection matching.
    Float4x4ToImGuizmoMatrix(dm::perspProjD3DStyle(dm::PI_f * 0.5f, 1.f, 0.1f, 1000.f), outMatrix);
}

bool ApplyImOGuizmoViewMatrix(CameraController& camera, const float viewMatrix[16])
{
    const dm::affine3 viewAffine = ImGuizmoMatrixToAffine3(viewMatrix);
    const dm::affine3 invView = inverse(viewAffine);

    const dm::float3 pos = invView.m_translation;
    const dm::float3 dir = normalize(dm::float3(
        viewAffine.m_linear.m02,
        viewAffine.m_linear.m12,
        viewAffine.m_linear.m22));
    const dm::float3 up = normalize(dm::float3(
        viewAffine.m_linear.m01,
        viewAffine.m_linear.m11,
        viewAffine.m_linear.m21));

    if (!std::isfinite(pos.x) || !std::isfinite(dir.x) || !std::isfinite(up.x))
        return false;
    if (length(dir) < 1e-6f || length(up) < 1e-6f)
        return false;

    camera.camera().lookTo(pos, dir, up);
    camera.markCameraChanged();
    return true;
}

} // namespace

void caustica::editor::DrawViewOrientationGizmo(const TransformGizmoContext& ctx)
{
    if (!ctx.editorUI.ShowUI || !ctx.editorUI.ShowViewOrientationGizmo)
        return;

    const auto& vp = ctx.editorUI.Viewport;
    if (!vp.RectValid || vp.SizeX <= 1.f || vp.SizeY <= 1.f)
        return;

    App* app = ctx.sceneEditor.app();
    auto* camera = caustica::editor::editorCamera(ctx.sceneEditor);
    if (!app || !camera)
        return;

    const auto& view = caustica::currentView(*app);
    if (!view)
        return;

    constexpr float kSize = 96.f;
    constexpr float kPad = 10.f;
    const float gizmoX = vp.PosX + vp.SizeX - kSize - kPad;
    const float gizmoY = vp.PosY + kPad;

    float viewMatrix[16];
    float projectionMatrix[16];
    Affine3ToImGuizmoMatrix(view->getViewMatrix(), viewMatrix);
    BuildImOGuizmoProjectionMatrix(projectionMatrix);

    // Pivot distance enables click-to-axis / orbit. Prefer distance to origin so
    // framing stays stable; fall back when the camera sits near the origin.
    float pivotDistance = length(camera->camera().getPosition());
    if (pivotDistance < 0.25f)
        pivotDistance = 1.f;

    ImOGuizmo::SetRect(gizmoX, gizmoY, kSize);
    ImOGuizmo::SetDrawList(ImGui::GetForegroundDrawList());

    // Caustica camera basis is Y-up with +Z look (LHS); match ImOGuizmo's ZYX.
    const bool viewChanged = ImOGuizmo::DrawGizmo(
        viewMatrix,
        projectionMatrix,
        pivotDistance,
        ImOGuizmo::CoordinateSystem::ZYX);

    if (viewChanged)
    {
        if (ApplyImOGuizmoViewMatrix(*camera, viewMatrix))
            ctx.settings.ResetAccumulation = true;
    }

    // Steal viewport picks / camera orbit while the cursor is over the widget
    // (or while a drag started on it is still held).
    const float hSize = kSize * 0.5f;
    const ImVec2 center(gizmoX + hSize, gizmoY + hSize);
    const float hoverRadius = hSize * ImOGuizmo::config.hoverCircleRadiusScale;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float dx = mouse.x - center.x;
    const float dy = mouse.y - center.y;
    const bool hovered = (dx * dx + dy * dy) <= (hoverRadius * hoverRadius);

    static bool s_viewGizmoCapturing = false;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        if (hovered)
            s_viewGizmoCapturing = true;
    }
    else
    {
        s_viewGizmoCapturing = false;
    }

    if (hovered || s_viewGizmoCapturing || viewChanged)
    {
        ctx.editorUI.GizmoCapturingInput = true;
        ImGui::GetIO().WantCaptureMouse = true;
    }
}
