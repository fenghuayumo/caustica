#include "common/TransformGizmo.h"

#include "SceneEditor.h"
#include "ui/EditorUIInternal.h"

#include <ImGuizmo.h>
#include <imgui.h>
#include <engine/GpuRenderSubsystem.h>
#include <math/affine.h>
#include <math/quat.h>
#include <scene/SceneEcs.h>
#include <scene/View.h>

#include <algorithm>
#include <cstring>

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
};

GizmoDragState g_drag;

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
    entityWorld.refreshHierarchy(caustica::scene::PreviousTransformPolicy::PreserveExisting);
}

void HandleTransformGizmoShortcuts(EditorUIState& editorUI)
{
    if (ImGui::GetIO().WantCaptureKeyboard || ImGui::IsAnyItemActive())
        return;

    if (ImGui::IsKeyPressed(ImGuiKey_W, false))
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::TRANSLATE);
    if (ImGui::IsKeyPressed(ImGuiKey_E, false))
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::ROTATE);
    if (ImGui::IsKeyPressed(ImGuiKey_T, false))
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::SCALE);
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
    auto* gpuRender = ctx.sceneEditor.gpuRender();
    if (gpuRender && view.isReverseDepth())
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float aspect = (io.DisplaySize.y > 0.f) ? (io.DisplaySize.x / io.DisplaySize.y) : 1.f;
        const float fov = gpuRender->camera().verticalFOV();
        const float zNear = std::max(gpuRender->camera().zNear(), 0.01f);
        const float zFar = std::max(zNear * 10000.f, 1000.f);
        Float4x4ToImGuizmoMatrix(dm::perspProjD3DStyle(fov, aspect, zNear, zFar), outMatrix);
        return;
    }

    Float4x4ToImGuizmoMatrix(view.getProjectionMatrix(false), outMatrix);
}

bool IsEditingInspectorUi()
{
    // DragFloat / radio buttons in Inspector must win over the viewport gizmo.
    return ImGui::IsAnyItemActive();
}

} // namespace

void caustica::editor::BuildTransformGizmoToolbar(EditorUIState& editorUI)
{
    const auto operation = static_cast<ImGuizmo::OPERATION>(editorUI.GizmoOperation);
    const auto mode = static_cast<ImGuizmo::MODE>(editorUI.GizmoMode);

    if (ImGui::RadioButton("Move", operation == ImGuizmo::TRANSLATE))
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::TRANSLATE);
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate", operation == ImGuizmo::ROTATE))
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::ROTATE);
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale", operation == ImGuizmo::SCALE))
        editorUI.GizmoOperation = static_cast<int>(ImGuizmo::SCALE);

    if (operation != ImGuizmo::SCALE)
    {
        if (ImGui::RadioButton("Local", mode == ImGuizmo::LOCAL))
            editorUI.GizmoMode = static_cast<int>(ImGuizmo::LOCAL);
        ImGui::SameLine();
        if (ImGui::RadioButton("World", mode == ImGuizmo::WORLD))
            editorUI.GizmoMode = static_cast<int>(ImGuizmo::WORLD);
    }

    ImGui::Checkbox("Snap", &editorUI.GizmoSnapEnabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(W/E/T)");
}

bool caustica::editor::DrawTransformGizmo(const TransformGizmoContext& ctx)
{
    ctx.editorUI.GizmoCapturingInput = false;

    if (!ctx.editorUI.ShowTransformGizmo || !ctx.editorUI.ShowUI)
    {
        g_drag = {};
        return false;
    }

    HandleTransformGizmoShortcuts(ctx.editorUI);

    auto* entityWorld = ctx.sceneEditor.entityWorld();
    const ecs::Entity entity = ctx.editorUI.SelectedEntity;
    if (!entityWorld || entity == ecs::NullEntity)
    {
        g_drag = {};
        return false;
    }

    auto* localTransform = entityWorld->world().tryGet<caustica::scene::LocalTransformComponent>(entity);
    auto* globalTransform = entityWorld->world().tryGet<caustica::scene::GlobalTransformComponent>(entity);
    if (!localTransform || !globalTransform)
    {
        g_drag = {};
        return false;
    }

    const auto& view = ctx.sceneEditor.currentView();
    if (!view)
    {
        g_drag = {};
        return false;
    }

    const bool editingUi = IsEditingInspectorUi();

    ImGuizmo::BeginFrame();
    {
        // Default ImGuizmo line weights are a bit thin in this editor; scale to 1.5x.
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
    // While Inspector DragFloats are active, keep drawing but block gizmo input/writeback.
    ImGuizmo::Enable(ctx.editorUI.GizmoEnabled && !editingUi);
    ImGuizmo::SetID(static_cast<int>(entt::to_integral(entity)));

    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetRect(0.f, 0.f, io.DisplaySize.x, io.DisplaySize.y);
    ImGuizmo::SetOrthographic(view->isOrthographicProjection());

    float viewMatrix[16];
    float projectionMatrix[16];
    Affine3ToImGuizmoMatrix(view->getViewMatrix(), viewMatrix);
    BuildGizmoProjectionMatrix(ctx, *view, projectionMatrix);

    // Sync from ECS only when not actively dragging the gizmo. During a drag, keep the
    // working matrix so decompose/recompose round-trips cannot jitter the object.
    if (!ImGuizmo::IsUsing() || !g_drag.active || g_drag.entity != entity || editingUi)
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

    const bool usingGizmo = ImGuizmo::IsUsing();
    g_drag.active = usingGizmo && !editingUi;

    // Only capture input while actively dragging (not merely hovering) so Inspector UI stays usable.
    ctx.editorUI.GizmoCapturingInput = usingGizmo;
    if (usingGizmo)
        io.WantCaptureMouse = true;

    if (!manipulated || editingUi)
        return false;

    ApplyWorldMatrixToLocalTransform(*entityWorld, entity, g_drag.matrix);

    ctx.editorUI.InspectorRotationEntity = entity;
    ctx.editorUI.InspectorRotationEulerValid = false;
    ctx.settings.ResetAccumulation = true;
    return true;
}
