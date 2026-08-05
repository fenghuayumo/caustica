#include "common/TransformGizmo.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
#include "EditorUndoCommands.h"
#include <engine/SceneQuery.h>
#include <engine/CameraApi.h>
#include "ui/EditorUIInternal.h"

#include <ImGuizmo.h>
#include "common/imoguizmo.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <engine/SessionCamera.h>
#include <math/affine.h>
#include <math/quat.h>
#include <scene/SceneEcs.h>
#include <scene/View.h>

#include <algorithm>
#include <cmath>

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

    ImGuizmo::BeginFrame();
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
