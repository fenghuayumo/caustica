#include "SceneEditor.h"

#include "SceneContentEditor.h"
#include "common/LocalConfig.h"
#include "common/CaptureScriptManager.h"
#include "common/TransformGizmo.h"
#include "ui/RenderSettingsConsole.h"

#include <render/core/PathTracerSettings.h>
#include <render/passes/debug/ZoomTool.h>
#include <engine/App.h>
#include <engine/AppResources.h>
#include "EditorAccess.h"
#include <engine/SceneQuery.h>
#include <engine/CameraApi.h>
#include <engine/SceneLifecycle.h>
#include <engine/EngineApp.h>
#include <engine/RenderSessionApi.h>
#include <engine/EnqueueRenderCommand.h>
#include <engine/ScenePlugins.h>
#include <core/path_utils.h>
#include <core/json.h>
#include <core/log.h>
#include <platform/file_dialog.h>
#include <scene/SceneSerializer.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneObjects.h>
#include <scene/scene_utils.h>
#include <scene/View.h>
#include <EditorUI.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>

#include "game/GameScene.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#if CAUSTICA_WITH_PYTHON
#include "PythonScripting.h"
#endif

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

namespace caustica::editor
{

namespace
{

scene::AnimationChannelData* FindAnimationChannel(
    scene::SceneEntityWorld& entityWorld,
    ecs::Entity target,
    AnimationAttribute attribute,
    scene::AnimationComponent** owner = nullptr)
{
    scene::AnimationChannelData* result = nullptr;
    entityWorld.world().each<scene::AnimationComponent>(
        [&](ecs::Entity, scene::AnimationComponent& animation) {
            if (!animation.editorAuthored)
                return;
            if (result)
                return;
            for (auto& channel : animation.channels)
            {
                if (channel.targetEntity == target && channel.attribute == attribute)
                {
                    result = &channel;
                    if (owner)
                        *owner = &animation;
                    return;
                }
            }
        });
    return result;
}

scene::AnimationComponent& FindOrCreateEditorAnimation(
    scene::SceneEntityWorld& entityWorld,
    ecs::Entity& cachedEntity)
{
    if (ecs::isValid(cachedEntity) && entityWorld.world().isAlive(cachedEntity))
    {
        if (auto* animation =
                entityWorld.world().tryGet<scene::AnimationComponent>(cachedEntity))
            return *animation;
    }

    scene::AnimationComponent* existing = nullptr;
    entityWorld.world().each<scene::AnimationComponent>(
        [&](ecs::Entity entity, scene::AnimationComponent& animation) {
            if (!existing && animation.editorAuthored)
            {
                cachedEntity = entity;
                existing = &animation;
            }
        });
    if (existing)
        return *existing;

    cachedEntity = entityWorld.createEntity("Editor Keyframes", entityWorld.root());
    scene::AnimationComponent animation;
    animation.editorAuthored = true;
    entityWorld.setAnimation(cachedEntity, std::move(animation));
    return *entityWorld.world().get<scene::AnimationComponent>(cachedEntity);
}

void EnsureAnimationChannel(
    scene::SceneEntityWorld& entityWorld,
    ecs::Entity target,
    AnimationAttribute attribute,
    ecs::Entity& editorAnimationEntity)
{
    if (FindAnimationChannel(entityWorld, target, attribute))
        return;

    auto sampler = std::make_shared<animation::Sampler>();
    if (attribute == AnimationAttribute::Rotation)
        sampler->setInterpolationMode(animation::InterpolationMode::Slerp);
    else if (attribute == AnimationAttribute::Visibility)
        sampler->setInterpolationMode(animation::InterpolationMode::Step);
    else
        sampler->setInterpolationMode(animation::InterpolationMode::Linear);

    scene::AnimationChannelData channel;
    channel.sampler = std::move(sampler);
    channel.targetEntity = target;
    channel.attribute = attribute;

    auto& animation =
        FindOrCreateEditorAnimation(entityWorld, editorAnimationEntity);
    scene::addAnimationChannel(animation, std::move(channel));
}

bool EntitySupportsVisibility(const scene::SceneEntityWorld& entityWorld, ecs::Entity entity)
{
    return entityWorld.world().tryGet<scene::MeshInstanceComponent>(entity) != nullptr
        || entityWorld.world().tryGet<scene::GaussianSplatComponent>(entity) != nullptr;
}

bool GetEntityVisibility(const scene::SceneEntityWorld& entityWorld, ecs::Entity entity)
{
    if (const auto* mesh = entityWorld.world().tryGet<scene::MeshInstanceComponent>(entity))
        return mesh->enabled;
    if (const auto* splat = entityWorld.world().tryGet<scene::GaussianSplatComponent>(entity))
        return splat->splat.enabled;
    return true;
}

animation::Keyframe MakeVisibilityKeyframe(float time, bool visible)
{
    animation::Keyframe keyframe;
    keyframe.time = time;
    keyframe.value = dm::float4(visible ? 1.f : 0.f, 0.f, 0.f, 0.f);
    return keyframe;
}

std::vector<float> CollectKeyframeTimes(
    scene::SceneEntityWorld& entityWorld,
    ecs::Entity entity,
    AnimationAttribute attributeFilter,
    bool filterByAttribute)
{
    std::vector<float> result;
    entityWorld.world().each<scene::AnimationComponent>(
        [&](ecs::Entity, scene::AnimationComponent& animation) {
            if (!animation.editorAuthored)
                return;
            for (const auto& channel : animation.channels)
            {
                if (ecs::isValid(entity) && channel.targetEntity != entity)
                    continue;
                if (filterByAttribute && channel.attribute != attributeFilter)
                    continue;
                if (!channel.sampler)
                    continue;
                for (const auto& keyframe : channel.sampler->getKeyframes())
                    result.push_back(keyframe.time);
            }
        });

    std::sort(result.begin(), result.end());
    result.erase(
        std::unique(result.begin(), result.end(), [](float a, float b) {
            return std::fabs(a - b) <= 1e-4f;
        }),
        result.end());
    return result;
}

void EnsureUniqueSampler(scene::AnimationChannelData& channel)
{
    if (channel.sampler && channel.sampler.use_count() > 1)
        channel.sampler = std::make_shared<animation::Sampler>(*channel.sampler);
}

animation::Keyframe MakeKeyframe(float time, const dm::double3& value)
{
    animation::Keyframe keyframe;
    keyframe.time = time;
    keyframe.value = dm::float4(
        static_cast<float>(value.x),
        static_cast<float>(value.y),
        static_cast<float>(value.z),
        0.f);
    return keyframe;
}

animation::Keyframe MakeKeyframe(float time, const dm::dquat& value)
{
    animation::Keyframe keyframe;
    keyframe.time = time;
    keyframe.value = dm::float4(
        static_cast<float>(value.x),
        static_cast<float>(value.y),
        static_cast<float>(value.z),
        static_cast<float>(value.w));
    return keyframe;
}

void RecalculateAnimationsForTarget(
    scene::SceneEntityWorld& entityWorld,
    ecs::Entity target)
{
    entityWorld.world().each<scene::AnimationComponent>(
        [&](ecs::Entity, scene::AnimationComponent& animation) {
            const bool targetsEntity =
                std::any_of(
                    animation.channels.begin(),
                    animation.channels.end(),
                    [target](const scene::AnimationChannelData& channel) {
                        return channel.targetEntity == target;
                    });
            if (targetsEntity)
                scene::recalculateAnimationDuration(animation);
        });
}

} // namespace

SceneEditor::SceneEditor()
    : m_contentEditor(*this)
{
    m_inputRouter.bind(*this);
}

void SceneEditor::bindEngine(EngineApp& engine)
{
    m_engine = &engine;
    m_app = &engine.app();
    m_editorUiData = std::make_unique<EditorUIData>(engine.renderAppState());
    m_selectionState.editor = &m_editorUiData->editor;
    m_editorCameraState.viewState = &engine.viewState();
    m_captureScriptManager = std::make_unique<CaptureScriptManager>(*this, engine.renderAppState(), engine.commandLine());
    m_captureScriptState.manager = m_captureScriptManager.get();
}

SceneViewState& SceneEditor::viewState()
{
    return m_engine->viewState();
}

const SceneViewState& SceneEditor::viewState() const
{
    return m_engine->viewState();
}

render::RenderAppState& SceneEditor::renderAppState()
{
    return m_engine->renderAppState();
}

const render::RenderAppState& SceneEditor::renderAppState() const
{
    return m_engine->renderAppState();
}

PathTracerSettings& SceneEditor::pathTracerSettings()
{
    return m_engine->renderAppState().settings;
}

const PathTracerSettings& SceneEditor::pathTracerSettings() const
{
    return m_engine->renderAppState().settings;
}

render::RenderRuntimeState& SceneEditor::renderRuntimeState()
{
    return m_engine->renderAppState().runtime;
}

const render::RenderRuntimeState& SceneEditor::renderRuntimeState() const
{
    return m_engine->renderAppState().runtime;
}

CommandLineOptions& SceneEditor::cmdLine()
{
    return m_engine->commandLine();
}

const CommandLineOptions& SceneEditor::cmdLine() const
{
    return m_engine->commandLine();
}

render::AppDiagnostics& SceneEditor::diagnostics()
{
    return m_engine->diagnostics();
}

const render::AppDiagnostics& SceneEditor::diagnostics() const
{
    return m_engine->diagnostics();
}

SceneEditor::~SceneEditor()
{
#if CAUSTICA_WITH_PYTHON
    m_pythonScripting.reset();
#endif
}

void SceneEditor::setConsole(std::unique_ptr<RenderSettingsConsoleBinding> console)
{
    m_console = std::move(console);
}


ecs::Entity SceneEditor::pickGaussianSplatAtPixel(math::uint2 displayPixel) const
{
    if (!m_app)
        return ecs::NullEntity;

    auto* entityWorld = caustica::entityWorld(*m_app);
    const auto& view = caustica::currentView(*m_app);
    if (!entityWorld || !view)
        return ecs::NullEntity;

    const uint2 disp = caustica::displaySize(*m_app);
    if (disp.x == 0 || disp.y == 0)
        return ecs::NullEntity;

    // Picking.Position is viewport-local display pixels (see EditorInputRouter).
    const float2 mousePos = float2(float(displayPixel.x), float(displayPixel.y));
    const float2 displaySizeF = float2(float(disp.x), float(disp.y));
    const float4x4 viewProj = view->getViewProjectionMatrix();

    constexpr float2 kInvalidPos = float2(FLT_MAX, FLT_MAX);
    auto projectToScreen = [&](const float3& worldPos) -> float2
    {
        float4 projv = float4(worldPos, 1.f) * viewProj;
        if (std::fabs(projv.w) < 1e-8f)
            return kInvalidPos;
        projv /= projv.w;
        if (projv.z < 0.f)
            return kInvalidPos;
        projv.xy() = projv.xy() * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
        projv.xy() *= displaySizeF;
        if (projv.x < 0.f || projv.x > displaySizeF.x || projv.y < 0.f || projv.y > displaySizeF.y)
            return kInvalidPos;
        return projv.xy();
    };

    ecs::Entity bestEntity = ecs::NullEntity;
    float bestDistance = FLT_MAX;

    for (const auto& object : caustica::gaussianSplatObjectBounds(*m_app))
    {
        if (!ecs::isValid(object.entity))
            continue;
        const auto* splatComp = entityWorld->world().tryGet<scene::GaussianSplatComponent>(object.entity);
        if (!splatComp || !splatComp->splat.enabled)
            continue;

        auto* boundsComp = entityWorld->world().tryGet<scene::BoundsComponent>(object.entity);
        box3 bbox = boundsComp ? boundsComp->globalBounds : box3::empty();
        if (bbox.isempty())
        {
            auto* global = entityWorld->world().tryGet<scene::GlobalTransformComponent>(object.entity);
            const box3 local = object.localBounds;
            if (global && !local.isempty())
                bbox = local * global->transformFloat;
        }
        if (bbox.isempty())
            continue;

        const float3 center = bbox.center();
        const float2 screenCenter = projectToScreen(center);
        if (screenCenter.x == FLT_MAX)
            continue;

        float screenRadius = 0.f;
        for (int corner = 0; corner < 8; ++corner)
        {
            const float2 screenCorner = projectToScreen(bbox.getCorner(corner));
            if (screenCorner.x == FLT_MAX)
                continue;
            screenRadius = std::max(screenRadius, length(screenCenter - screenCorner));
        }
        if (screenRadius <= 0.f)
            continue;

        screenRadius += 10.f;
        if (length(mousePos - screenCenter) > screenRadius)
            continue;

        const float range = length(center - view->getViewOrigin());
        if (range < bestDistance)
        {
            bestDistance = range;
            bestEntity = object.entity;
        }
    }

    return bestEntity;
}

void SceneEditor::bindCameraControllerSideEffects()
{
    if (m_app)
        caustica::bindCameraControllerSideEffects(*m_app);
#if CAUSTICA_WITH_PYTHON
    if (!m_pythonScripting && m_engine)
        m_pythonScripting = std::make_unique<PythonScripting>(*m_engine);
#endif
    m_inputRouter.bind(*this);
}

void SceneEditor::onBeforeInitialSceneLoad()
{
    m_game = std::make_unique<::GameScene>(*this, m_editorCmdLine);
    viewState().progressLoading.Set(95);
}

void SceneEditor::consumeCompletedMaterialPickFeedback()
{
    const uint64_t completedRequestId =
        m_completedMaterialPickRequestId.load(std::memory_order_acquire);
    if (completedRequestId > m_consumedMaterialPickRequestId)
    {
        m_consumedMaterialPickRequestId = completedRequestId;
        auto& picking = renderRuntimeState().Picking;
        // SceneAfterWorldRender may already have cleared the one-shot bool.
        // Request identity, which remains monotonic, is the authoritative match.
        if (picking.MaterialRequestId == completedRequestId)
        {
            const int materialGpuId =
                m_completedMaterialPickGpuId.load(std::memory_order_relaxed);
            editorUIState().SelectedMaterial = m_app
                ? caustica::findMaterial(*m_app, materialGpuId)
                : nullptr;
            picking.completeMaterialPick(completedRequestId);
        }
    }
}

void SceneEditor::consumeCompletedInstancePickFeedback()
{
    const uint64_t completedRequestId =
        m_completedInstancePickRequestId.load(std::memory_order_acquire);
    if (completedRequestId <= m_consumedInstancePickRequestId)
        return;

    m_consumedInstancePickRequestId = completedRequestId;
    auto& picking = renderRuntimeState().Picking;
    // SceneAfterWorldRender may already have cleared the one-shot bool.
    if (picking.InstanceRequestId != completedRequestId)
        return;

    const int instanceIndex = m_completedInstancePickIndex.load(std::memory_order_relaxed);
    const uint64_t packedPosition =
        m_completedInstancePickPosition.load(std::memory_order_relaxed);
    const math::uint2 pickPixel{
        uint32_t(packedPosition >> 32u),
        uint32_t(packedPosition & 0xffffffffu)};

    ecs::Entity picked = m_app
        ? caustica::findEntityByInstanceIndex(*m_app, instanceIndex)
        : ecs::NullEntity;
    bool pickedGaussian = false;
    if (picked == ecs::NullEntity)
    {
        picked = pickGaussianSplatAtPixel(pickPixel);
        pickedGaussian = (picked != ecs::NullEntity);
    }
    editorUIState().SelectedEntity = picked;
    editorUIState().SelectedGaussianSplat = pickedGaussian;
    picking.completeInstancePick(completedRequestId);
}

void SceneEditor::prepareEditorFrame()
{
    pathTracerSettings().DebugExploreDeltaTree = editorUIState().ShowDeltaTree;
}

void SceneEditor::onSceneUnloading()
{
    m_pendingEditAction = PendingEditAction::None;
    m_undoStack.clear();
    ResetTransformGizmoInteraction();
    m_editorAnimationEntity = ecs::NullEntity;
    editorUIState().TogglableNodes = nullptr;
    editorUIState().SelectedMaterial = nullptr;
    editorUIState().SelectedEntity = caustica::ecs::NullEntity;
    editorUIState().PendingDeleteEntity = caustica::ecs::NullEntity;
    editorUIState().InspectorRotationEntity = caustica::ecs::NullEntity;
    editorUIState().InspectorRotationEulerValid = false;
    editorUIState().SelectedGaussianSplat = false;
    m_editorState.sceneDocument = Json::Value();
    m_editorState.sceneDocumentPath.clear();
    m_editorState.sceneDocumentValid = false;
    m_editorState.loadedSceneName.clear();

    if (m_game != nullptr)
        m_game->sceneUnloading();
}

void SceneEditor::requestUndo()
{
    m_pendingEditAction = PendingEditAction::Undo;
}

void SceneEditor::requestRedo()
{
    m_pendingEditAction = PendingEditAction::Redo;
}

void SceneEditor::requestOpenSceneFromDialog()
{
    m_pendingEditAction = PendingEditAction::OpenScene;
}

void SceneEditor::requestSaveScene()
{
    m_pendingEditAction = PendingEditAction::SaveScene;
}

void SceneEditor::requestSaveSceneAsFromDialog()
{
    m_pendingEditAction = PendingEditAction::SaveSceneAs;
}

void SceneEditor::processPendingEditActions()
{
    const PendingEditAction action = m_pendingEditAction;
    m_pendingEditAction = PendingEditAction::None;

    if (action == PendingEditAction::None || !m_app)
        return;

    // File dialogs / scene switches must run after ImGui::Render (this system is
    // ordered after EditorUIAnimate). Running them inside buildUI freezes the
    // update thread mid-frame and makes Open Scene look like a hang/crash.
    if (action == PendingEditAction::OpenScene)
    {
        openSceneFromDialog();
        return;
    }
    if (action == PendingEditAction::SaveScene)
    {
        saveScene();
        return;
    }
    if (action == PendingEditAction::SaveSceneAs)
    {
        saveSceneAsFromDialog();
        return;
    }

    // Transform undo reverses a change that may still be referenced by one of the
    // two pipelined render frames. Drain the render thread before publishing the
    // reverse transform. GPU waitForIdle for undo runs on the render thread.
    // THREADING: Logic↔RT wait — ADR 0002 S5 (editor undo/redo; not frame loop).
    m_app->waitForRenderThreadIdle();
    GpuDevice* gpuDevice = m_app->getGpuDevice();
    if (!gpuDevice || gpuDevice->isShuttingDown())
        return;

    bool waitOk = true;
    caustica::EnqueueRenderCommandAndWait(*m_app, [gpuDevice, &waitOk]() {
        if (caustica::rhi::Device* rhi = gpuDevice->getDevice())
            waitOk = rhi->waitForIdle();
    });
    if (!waitOk)
    {
        gpuDevice->setShuttingDown(true);
        return;
    }

    if (action == PendingEditAction::Undo)
        undo();
    else if (action == PendingEditAction::Redo)
        redo();
}

bool SceneEditor::undo()
{
    // Mid-drag Ctrl+Z cancels the in-progress gizmo edit instead of popping the stack.
    if (CancelTransformGizmoEdit(*this))
        return true;

    if (!m_undoStack.undo())
        return false;

    ResetTransformGizmoInteraction();
    return true;
}

bool SceneEditor::redo()
{
    if (IsTransformGizmoEditing())
        return false;

    if (!m_undoStack.redo())
        return false;

    ResetTransformGizmoInteraction();
    return true;
}

void SceneEditor::commitTransformEdit(
    ecs::Entity entity,
    const LocalTransformSnapshot& before,
    const LocalTransformSnapshot& after)
{
    if (auto command = makeTransformUndoCommand(*this, entity, before, after))
        m_undoStack.push(std::move(command));
}

void SceneEditor::onSceneLoadedEarly()
{
    if (m_game == nullptr || !m_app)
        return;

    const std::filesystem::path assetsRoot = mediaRootForScene(caustica::currentScenePath(*m_app));
    m_game->sceneLoaded(caustica::currentScenePath(*m_app), assetsRoot);
}

void SceneEditor::onSceneLoadedBeforeGpuPrep()
{
    if (editorUIState().TogglableNodes != nullptr || !m_app)
        return;

    if (auto* ew = caustica::entityWorld(*m_app))
    {
        editorUIState().TogglableNodes = std::make_shared<std::vector<TogglableNode>>();
        UpdateTogglableNodes(*editorUIState().TogglableNodes, *ew, ew->root());
    }
}

void SceneEditor::onSceneLoadedAfterCollectTextures()
{
    LocalConfig::PostSceneLoad(*this, renderAppState(), editorUIState());
}

void SceneEditor::onSceneLoadedComplete()
{
#if CAUSTICA_WITH_PYTHON
    if (m_pythonScripting
        && (!m_editorCmdLine.pythonScript.empty() || !m_editorCmdLine.pythonExpr.empty()))
    {
        if (m_pythonScripting->Initialize())
        {
            if (!m_editorCmdLine.pythonScript.empty())
                m_pythonScripting->QueueScriptFile(m_editorCmdLine.pythonScript);
            if (!m_editorCmdLine.pythonExpr.empty())
                m_pythonScripting->QueueScriptString(m_editorCmdLine.pythonExpr, "<--pythonExpr>");
        }
    }
#endif
}

void SceneEditor::onSceneLoadedFromLoader()
{
    onSceneLoadedEarly();
    onSceneLoadedBeforeGpuPrep();
    if (m_app)
        caustica::onSceneLoaded(*m_app);
    onSceneLoadedAfterCollectTextures();
    onSceneLoadedComplete();
}

void SceneEditor::syncLoadedSceneSystems()
{
    if (!m_app || !caustica::isSceneLoaded(*m_app))
        return;

    const std::string loadedSceneName = caustica::currentSceneName(*m_app);
    if (loadedSceneName.empty() || loadedSceneName == m_editorState.loadedSceneName)
        return;

    m_editorState.loadedSceneName = loadedSceneName;

    m_editorState.sceneDocument = Json::Value();
    m_editorState.sceneDocumentValid = false;
    m_editorState.sceneDocumentPath.clear();

    const std::filesystem::path scenePath = caustica::currentScenePath(*m_app);
    if (scenePath.empty() || caustica::isInlineScenePath(scenePath))
        return;

    Json::Value document;
    if (caustica::json::loadFromFile(scenePath, document))
    {
        m_editorState.sceneDocument = std::move(document);
        m_editorState.sceneDocumentPath = scenePath;
        m_editorState.sceneDocumentValid = true;
    }
    else
    {
        caustica::warning("Could not cache scene JSON for save: '%s'", scenePath.generic_string().c_str());
    }
}

namespace
{

void SaveSceneMaterials(App& app)
{
    caustica::saveAllMaterials(app);
}

const char* AnimationModeName(animation::InterpolationMode mode)
{
    switch (mode)
    {
    case animation::InterpolationMode::Step:             return "step";
    case animation::InterpolationMode::Linear:           return "linear";
    case animation::InterpolationMode::Slerp:            return "slerp";
    case animation::InterpolationMode::CatmullRomSpline: return "catmull-rom";
    case animation::InterpolationMode::HermiteSpline:    return "hermite";
    default:                                             return "step";
    }
}

const char* AnimationAttributeName(const scene::AnimationChannelData& channel)
{
    switch (channel.attribute)
    {
    case AnimationAttribute::Translation: return "translation";
    case AnimationAttribute::Rotation:    return "rotation";
    case AnimationAttribute::Scaling:     return "scaling";
    case AnimationAttribute::Visibility:  return "visibility";
    case AnimationAttribute::LeafProperty:
        return channel.leafPropertyName.empty() ? nullptr : channel.leafPropertyName.c_str();
    case AnimationAttribute::Undefined:
    default:
        return nullptr;
    }
}

void WriteAnimationValue(
    Json::Value& destination,
    const dm::float4& value,
    AnimationAttribute attribute)
{
    if (attribute == AnimationAttribute::Visibility)
    {
        destination = value.x;
        return;
    }

    destination = Json::Value(Json::arrayValue);
    destination.append(value.x);
    destination.append(value.y);
    destination.append(value.z);
    if (attribute == AnimationAttribute::Rotation
        || attribute == AnimationAttribute::LeafProperty)
    {
        destination.append(value.w);
    }
}

void PatchEditorAnimations(
    Json::Value& document,
    scene::SceneEntityWorld& entityWorld)
{
    // Preserve scene-authored animation JSON verbatim. Imported animations are
    // owned by their source assets and must not be baked into the scene on save.
    Json::Value animations(Json::arrayValue);
    const Json::Value& existingAnimations = document["animations"];
    if (existingAnimations.isArray())
    {
        for (const Json::Value& animationNode : existingAnimations)
        {
            if (!animationNode["editorAuthored"].asBool())
                animations.append(animationNode);
        }
    }

    entityWorld.world().each<scene::AnimationComponent>(
        [&](ecs::Entity animationEntity, scene::AnimationComponent& animationComponent) {
            if (!animationComponent.editorAuthored)
                return;

            Json::Value animationNode(Json::objectValue);
            std::string name = entityWorld.getEntityName(animationEntity);
            animationNode["name"] = name.empty() ? "Editor Keyframes" : name;
            animationNode["editorAuthored"] = true;
            Json::Value channels(Json::arrayValue);

            for (const auto& channel : animationComponent.channels)
            {
                if (!channel.sampler || channel.sampler->getKeyframes().empty())
                    continue;

                const char* attributeName = AnimationAttributeName(channel);
                if (!attributeName)
                    continue;

                std::string target;
                if (channel.targetMaterial)
                {
                    target = "material:" + channel.targetMaterial->name;
                }
                else if (ecs::isValid(channel.targetEntity))
                {
                    target = entityWorld.getEntityPath(channel.targetEntity).generic_string();
                }
                if (target.empty())
                    continue;

                Json::Value channelNode(Json::objectValue);
                channelNode["target"] = target;
                channelNode["attribute"] = attributeName;
                channelNode["mode"] = AnimationModeName(channel.sampler->getMode());

                Json::Value keyframes(Json::arrayValue);
                for (const auto& keyframe : channel.sampler->getKeyframes())
                {
                    Json::Value keyframeNode(Json::objectValue);
                    keyframeNode["time"] = keyframe.time;
                    WriteAnimationValue(
                        keyframeNode["value"], keyframe.value, channel.attribute);
                    if (channel.sampler->getMode()
                        == animation::InterpolationMode::HermiteSpline)
                    {
                        WriteAnimationValue(
                            keyframeNode["inTangent"], keyframe.inTangent, channel.attribute);
                        WriteAnimationValue(
                            keyframeNode["outTangent"], keyframe.outTangent, channel.attribute);
                    }
                    keyframes.append(std::move(keyframeNode));
                }
                channelNode["data"] = std::move(keyframes);
                channels.append(std::move(channelNode));
            }

            if (!channels.empty())
            {
                animationNode["channels"] = std::move(channels);
                animations.append(std::move(animationNode));
            }
        });

    if (animations.empty())
        document.removeMember("animations");
    else
        document["animations"] = std::move(animations);
}

SceneSettings BuildSceneLookSettings(
    const PathTracerSettings& cfg,
    const std::string& envOverride,
    scene::SceneEntityWorld& world)
{
    SceneSettings look;

    EnvironmentLookSettings env;
    env.tintColor = cfg.EnvironmentMapParams.TintColor;
    env.intensity = cfg.EnvironmentMapParams.Intensity;
    env.rotationXYZ = cfg.EnvironmentMapParams.RotationXYZ;
    env.visibleToCamera = cfg.EnvironmentMapParams.VisibleToCamera;
    env.enabled = cfg.EnvironmentMapParams.enabled;
    env.overrideSource = envOverride;
    look.environment = std::move(env);

    GaussianSplatLookSettings splat;
    splat.footprintScale = cfg.GaussianSplatScale;
    splat.alphaScale = cfg.GaussianSplatAlphaScale;
    splat.brightness = cfg.GaussianSplatBrightness;
    splat.tintColor = cfg.GaussianSplatTintColor;
    splat.applyToneMapping = cfg.GaussianSplatApplyToneMapping;
    splat.asEmitter = cfg.GaussianSplatAsEmitter;
    splat.emissionIntensity = cfg.GaussianSplatEmissionIntensity;
    splat.alphaCullThreshold = cfg.GaussianSplatAlphaCullThreshold;
    splat.shadowStrength = cfg.GaussianSplatShadowStrength;
    look.gaussianSplat = std::move(splat);

    auto addHidden = [&](ecs::Entity entity, bool enabled)
    {
        if (enabled)
            return;
        const std::string path = world.getEntityPath(entity).generic_string();
        if (!path.empty())
            look.hiddenEntities.push_back(path);
    };
    world.world().each<scene::MeshInstanceComponent>(
        [&](ecs::Entity entity, scene::MeshInstanceComponent& mesh)
        {
            addHidden(entity, mesh.enabled);
        });
    world.world().each<scene::GaussianSplatComponent>(
        [&](ecs::Entity entity, scene::GaussianSplatComponent& splatComp)
        {
            addHidden(entity, splatComp.splat.enabled);
        });
    world.world().each<scene::DirectionalLightComponent>(
        [&](ecs::Entity entity, scene::DirectionalLightComponent& light)
        {
            addHidden(entity, light.enabled);
        });
    world.world().each<scene::PointLightComponent>(
        [&](ecs::Entity entity, scene::PointLightComponent& light)
        {
            addHidden(entity, light.enabled);
        });
    world.world().each<scene::SpotLightComponent>(
        [&](ecs::Entity entity, scene::SpotLightComponent& light)
        {
            addHidden(entity, light.enabled);
        });
    world.world().each<scene::RectLightComponent>(
        [&](ecs::Entity entity, scene::RectLightComponent& light)
        {
            addHidden(entity, light.enabled);
        });
    world.world().each<scene::EnvironmentLightComponent>(
        [&](ecs::Entity entity, scene::EnvironmentLightComponent& light)
        {
            addHidden(entity, light.enabled);
        });
    return look;
}

bool SaveSceneDocumentToPath(
    App& app,
    EditorState& editorState,
    const std::filesystem::path& path)
{
    if (!editorState.sceneDocumentValid)
        return false;

    scene::SceneEntityWorld* ew = caustica::entityWorld(app);
    if (!ew || !ecs::isValid(ew->root()))
        return false;

    ew->rebuildPathsFromRoot();
    caustica::scene::syncAuthoredEntitiesToDocument(editorState.sceneDocument, *ew);
    if (editorState.sceneDocument.isMember("entities"))
        caustica::scene::patchEntityTransforms(editorState.sceneDocument["entities"], *ew);
    caustica::scene::patchEntityOverrides(editorState.sceneDocument, *ew);
    PatchEditorAnimations(editorState.sceneDocument, *ew);

    if (const PathTracerSettings* cfg = caustica::settings(app))
    {
        const SceneSettings look = BuildSceneLookSettings(
            *cfg, caustica::envMapOverrideSource(app), *ew);
        look.writeLook(editorState.sceneDocument["settings"]);
    }

    if (!caustica::json::saveToFile(path, editorState.sceneDocument))
        return false;

    editorState.sceneDocumentPath = path;
    SaveSceneMaterials(app);
    return true;
}

} // namespace

bool SceneEditor::canSaveScene() const
{
    return m_app
        && caustica::isSceneLoaded(*m_app)
        && m_editorState.sceneDocumentValid
        && !m_editorState.sceneDocumentPath.empty()
        && !caustica::isInlineScenePath(m_editorState.sceneDocumentPath);
}

bool SceneEditor::openSceneFromDialog()
{
    if (!m_app)
        return false;
    if (caustica::isSceneSwitchBusy(*m_app))
    {
        caustica::warning("Open Scene: ignored, scene switch / structure edit busy");
        return false;
    }

    caustica::sceneSwitchTrace("Open Scene: showing file dialog");
    std::string picked;
    const std::filesystem::path currentPath = caustica::currentScenePath(*m_app);
    if (!currentPath.empty() && !caustica::isInlineScenePath(currentPath))
        picked = currentPath.string();

    std::string initialDir;
    const std::filesystem::path packRoot = caustica::getAssetPackRoot();
    if (std::filesystem::is_directory(packRoot / caustica::c_ScenesSubFolder))
        initialDir = (packRoot / caustica::c_ScenesSubFolder).string();
    else if (std::filesystem::is_directory(packRoot))
        initialDir = packRoot.string();

    if (!caustica::FileDialog(
            true,
            "Scene files (*.scene.json;*.json)\0*.scene.json;*.json\0All files\0*.*\0",
            picked,
            initialDir.empty() ? nullptr : initialDir.c_str()))
    {
        caustica::sceneSwitchTrace("Open Scene: cancelled");
        return false;
    }

    caustica::sceneSwitchTrace("Open Scene: picked '%s'", picked.c_str());
    // forceReload: dialog pick must always reload even if the path string matches.
    caustica::setCurrentScene(*m_app, picked, true);
    return true;
}

bool SceneEditor::saveScene()
{
    if (!canSaveScene())
        return saveSceneAsFromDialog();

    if (!SaveSceneDocumentToPath(*m_app, m_editorState, m_editorState.sceneDocumentPath))
    {
        caustica::error("Failed to save scene '%s'", m_editorState.sceneDocumentPath.generic_string().c_str());
        return false;
    }

    caustica::info("Saved scene '%s'", m_editorState.sceneDocumentPath.generic_string().c_str());
    return true;
}

bool SceneEditor::saveSceneAsFromDialog()
{
    if (!m_app || !m_editorState.sceneDocumentValid || !caustica::isSceneLoaded(*m_app))
        return false;

    std::string picked = m_editorState.sceneDocumentPath.generic_string();
    if (!caustica::FileDialog(
            false,
            "Scene files (*.scene.json)\0*.scene.json\0JSON files (*.json)\0*.json\0All files\0*.*\0",
            picked))
        return false;

    std::filesystem::path path(picked);
    if (path.extension().empty())
        path += ".scene.json";

    if (!SaveSceneDocumentToPath(*m_app, m_editorState, path))
    {
        caustica::error("Failed to save scene '%s'", path.generic_string().c_str());
        return false;
    }

    const std::string sceneName = path.filename().generic_string();
    caustica::retargetCurrentScene(*m_app, sceneName, path);

    m_editorState.loadedSceneName = sceneName;
    caustica::info("Saved scene as '%s'", path.generic_string().c_str());
    return true;
}

void SceneEditor::onAnimateBegin(float& fElapsedTimeSeconds)
{
    // Consume render feedback before EditorUIAnimate so the material panel
    // reflects the completed click in this logic/UI frame.
    consumeCompletedMaterialPickFeedback();
    consumeCompletedInstancePickFeedback();
    m_captureScriptManager->preAnim(fElapsedTimeSeconds);

#if CAUSTICA_WITH_PYTHON
    if (m_pythonScripting && m_app && caustica::isSceneLoaded(*m_app))
        m_pythonScripting->ProcessPendingScripts();
#endif
}

void SceneEditor::onAnimateGameTick(float fElapsedTimeSeconds, bool enableAnimations)
{
    if (m_game)
        m_game->Tick(fElapsedTimeSeconds, enableAnimations);
}

void SceneEditor::onAnimateUpdateSceneTime(float /*fElapsedTimeSeconds*/, bool enableAnimations, bool /*enableAnimationUpdate*/)
{
    if (enableAnimations && m_game && m_game->IsInitialized())
        viewState().sceneTime = m_game->gameTime();
}

void SceneEditor::onAnimateGameCamera(float fElapsedTimeSeconds)
{
    if (m_game && m_app)
        m_game->TickCamera(fElapsedTimeSeconds, caustica::currentCamera(*m_app));
}

void SceneEditor::onAnimateEnd(float /*fElapsedTimeSeconds*/)
{
    m_captureScriptManager->PostAnim();
}

void SceneEditor::updateWindowTitle()
{
    if (m_app)
        caustica::updateWindowTitle(*m_app);
}

void SceneEditor::setSceneTime(double sceneTime)
{
    if (m_game && m_game->IsInitialized())
        m_game->SetGameTime(sceneTime);
    viewState().sceneTime = sceneTime;
}

double SceneEditor::sceneTime() const
{
    if (m_game && m_game->IsInitialized())
        return m_game->gameTime();
    return viewState().sceneTime;
}

void SceneEditor::setTimelineTime(double timelineTime)
{
    viewState().keyframeTime = timelineTime;
}

double SceneEditor::timelineTime() const
{
    return viewState().keyframeTime;
}

bool SceneEditor::insertTransformKeyframe(ecs::Entity entity, float timeSeconds)
{
    if (!m_app || !ecs::isValid(entity))
        return false;

    auto* entityWorld = caustica::entityWorld(*m_app);
    if (!entityWorld || !entityWorld->world().isAlive(entity))
        return false;

    const auto* transform =
        entityWorld->world().tryGet<scene::LocalTransformComponent>(entity);
    if (!transform)
        return false;

    EnsureAnimationChannel(
        *entityWorld,
        entity,
        AnimationAttribute::Translation,
        m_editorAnimationEntity);
    EnsureAnimationChannel(
        *entityWorld,
        entity,
        AnimationAttribute::Rotation,
        m_editorAnimationEntity);
    EnsureAnimationChannel(
        *entityWorld,
        entity,
        AnimationAttribute::Scaling,
        m_editorAnimationEntity);

    // Adding channels can reallocate the owning vector, so acquire stable pointers
    // only after all three channels exist.
    auto* translation =
        FindAnimationChannel(*entityWorld, entity, AnimationAttribute::Translation);
    auto* rotation =
        FindAnimationChannel(*entityWorld, entity, AnimationAttribute::Rotation);
    auto* scaling =
        FindAnimationChannel(*entityWorld, entity, AnimationAttribute::Scaling);
    if (!translation || !rotation || !scaling)
        return false;

    EnsureUniqueSampler(*translation);
    EnsureUniqueSampler(*rotation);
    EnsureUniqueSampler(*scaling);

    translation->sampler->upsertKeyframe(MakeKeyframe(timeSeconds, transform->translation));
    rotation->sampler->upsertKeyframe(MakeKeyframe(timeSeconds, transform->rotation));
    scaling->sampler->upsertKeyframe(MakeKeyframe(timeSeconds, transform->scaling));

    RecalculateAnimationsForTarget(*entityWorld, entity);
    pathTracerSettings().ResetAccumulation = true;
    return true;
}

bool SceneEditor::deleteTransformKeyframe(ecs::Entity entity, float timeSeconds)
{
    if (!m_app || !ecs::isValid(entity))
        return false;

    auto* entityWorld = caustica::entityWorld(*m_app);
    if (!entityWorld || !entityWorld->world().isAlive(entity))
        return false;

    bool removed = false;
    for (AnimationAttribute attribute : {
             AnimationAttribute::Translation,
             AnimationAttribute::Rotation,
             AnimationAttribute::Scaling })
    {
        if (auto* channel = FindAnimationChannel(*entityWorld, entity, attribute);
            channel && channel->sampler)
        {
            EnsureUniqueSampler(*channel);
            removed = channel->sampler->removeKeyframe(timeSeconds) || removed;
        }
    }

    if (removed)
    {
        RecalculateAnimationsForTarget(*entityWorld, entity);
        pathTracerSettings().ResetAccumulation = true;
    }
    return removed;
}

bool SceneEditor::hasTransformKeyframe(ecs::Entity entity, float timeSeconds) const
{
    if (!m_app || !ecs::isValid(entity))
        return false;

    auto* entityWorld = caustica::entityWorld(*m_app);
    if (!entityWorld || !entityWorld->world().isAlive(entity))
        return false;

    for (AnimationAttribute attribute : {
             AnimationAttribute::Translation,
             AnimationAttribute::Rotation,
             AnimationAttribute::Scaling })
    {
        const auto* channel = FindAnimationChannel(*entityWorld, entity, attribute);
        if (channel && channel->sampler && channel->sampler->hasKeyframe(timeSeconds))
            return true;
    }
    return false;
}

bool SceneEditor::canAnimateVisibility(ecs::Entity entity) const
{
    if (!m_app || !ecs::isValid(entity))
        return false;
    auto* entityWorld = caustica::entityWorld(*m_app);
    if (!entityWorld || !entityWorld->world().isAlive(entity))
        return false;
    return EntitySupportsVisibility(*entityWorld, entity);
}

bool SceneEditor::insertVisibilityKeyframe(ecs::Entity entity, float timeSeconds)
{
    if (!m_app || !ecs::isValid(entity))
        return false;

    auto* entityWorld = caustica::entityWorld(*m_app);
    if (!entityWorld || !entityWorld->world().isAlive(entity))
        return false;
    if (!EntitySupportsVisibility(*entityWorld, entity))
        return false;

    EnsureAnimationChannel(
        *entityWorld,
        entity,
        AnimationAttribute::Visibility,
        m_editorAnimationEntity);

    auto* channel =
        FindAnimationChannel(*entityWorld, entity, AnimationAttribute::Visibility);
    if (!channel || !channel->sampler)
        return false;

    EnsureUniqueSampler(*channel);
    // Visibility is a discrete state even when an existing scene track was
    // authored with a different interpolation mode.
    channel->sampler->setInterpolationMode(animation::InterpolationMode::Step);
    channel->sampler->upsertKeyframe(
        MakeVisibilityKeyframe(timeSeconds, GetEntityVisibility(*entityWorld, entity)));

    RecalculateAnimationsForTarget(*entityWorld, entity);
    pathTracerSettings().ResetAccumulation = true;
    if (entityWorld->world().tryGet<scene::GaussianSplatComponent>(entity))
        renderRuntimeState().Invalidation.AccelerationStructRebuildRequested = true;
    return true;
}

bool SceneEditor::deleteVisibilityKeyframe(ecs::Entity entity, float timeSeconds)
{
    if (!m_app || !ecs::isValid(entity))
        return false;

    auto* entityWorld = caustica::entityWorld(*m_app);
    if (!entityWorld || !entityWorld->world().isAlive(entity))
        return false;

    auto* channel =
        FindAnimationChannel(*entityWorld, entity, AnimationAttribute::Visibility);
    if (!channel || !channel->sampler)
        return false;

    EnsureUniqueSampler(*channel);
    if (!channel->sampler->removeKeyframe(timeSeconds))
        return false;

    RecalculateAnimationsForTarget(*entityWorld, entity);
    pathTracerSettings().ResetAccumulation = true;
    return true;
}

bool SceneEditor::hasVisibilityKeyframe(ecs::Entity entity, float timeSeconds) const
{
    if (!m_app || !ecs::isValid(entity))
        return false;

    auto* entityWorld = caustica::entityWorld(*m_app);
    if (!entityWorld || !entityWorld->world().isAlive(entity))
        return false;

    const auto* channel =
        FindAnimationChannel(*entityWorld, entity, AnimationAttribute::Visibility);
    return channel && channel->sampler && channel->sampler->hasKeyframe(timeSeconds);
}

std::vector<float> SceneEditor::keyframeTimes(ecs::Entity entity) const
{
    if (!m_app)
        return {};
    auto* entityWorld = caustica::entityWorld(*m_app);
    if (!entityWorld)
        return {};

    // Transform diamonds: exclude visibility (drawn on its own row).
    std::vector<float> result;
    entityWorld->world().each<scene::AnimationComponent>(
        [&](ecs::Entity, scene::AnimationComponent& animation) {
            if (!animation.editorAuthored)
                return;
            for (const auto& channel : animation.channels)
            {
                if (ecs::isValid(entity) && channel.targetEntity != entity)
                    continue;
                if (channel.attribute == AnimationAttribute::Visibility)
                    continue;
                if (!channel.sampler)
                    continue;
                for (const auto& keyframe : channel.sampler->getKeyframes())
                    result.push_back(keyframe.time);
            }
        });
    std::sort(result.begin(), result.end());
    result.erase(
        std::unique(result.begin(), result.end(), [](float a, float b) {
            return std::fabs(a - b) <= 1e-4f;
        }),
        result.end());
    return result;
}

std::vector<float> SceneEditor::visibilityKeyframeTimes(ecs::Entity entity) const
{
    if (!m_app)
        return {};
    auto* entityWorld = caustica::entityWorld(*m_app);
    if (!entityWorld)
        return {};
    return CollectKeyframeTimes(
        *entityWorld, entity, AnimationAttribute::Visibility, true);
}

float SceneEditor::animationDuration() const
{
    float duration = 0.f;
    if (!m_app)
        return duration;

    auto* entityWorld = caustica::entityWorld(*m_app);
    if (!entityWorld)
        return duration;

    entityWorld->world().each<scene::AnimationComponent>(
        [&](ecs::Entity, scene::AnimationComponent& animation) {
            if (!animation.editorAuthored)
                return;
            duration = std::max(duration, scene::getAnimationDuration(animation));
        });
    return duration;
}

void SceneEditor::evaluateAnimationsAt(float timeSeconds, AnimationEvaluateMode mode)
{
    setTimelineTime(std::max(0.f, timeSeconds));
    bool touchedGaussianVisibility = false;
    if (m_app)
    {
        if (auto* entityWorld = caustica::entityWorld(*m_app))
        {
            entityWorld->world().each<scene::AnimationComponent>(
                [&](ecs::Entity, scene::AnimationComponent& animation) {
                    if (animation.editorAuthored)
                        (void)scene::applyAnimation(
                            animation, std::max(0.f, timeSeconds), *entityWorld);
                });

            if (mode == AnimationEvaluateMode::ContinuousScrub)
            {
                // Same as timeline/skeletal playback: capture previous transforms so
                // MVs/TAA/NRD see a smooth step instead of wiping history every drag tick.
                entityWorld->refreshHierarchy(scene::PreviousTransformPolicy::CaptureCurrent);
            }
            else
            {
                // Discontinuous seek: keep previous==current so filters do not treat the
                // jump as high-speed motion, then drop temporal history once.
                entityWorld->refreshHierarchy(scene::PreviousTransformPolicy::PreserveExisting);
                entityWorld->syncPreviousTransformsFromCurrent();
                entityWorld->resetSkinnedMeshMotionHistory();
            }

            entityWorld->world().each<scene::AnimationComponent>(
                [&](ecs::Entity, scene::AnimationComponent& animation) {
                    if (!animation.editorAuthored)
                        return;
                    for (const auto& channel : animation.channels)
                    {
                        if (channel.attribute != AnimationAttribute::Visibility)
                            continue;
                        if (!ecs::isValid(channel.targetEntity))
                            continue;
                        if (entityWorld->world().tryGet<scene::GaussianSplatComponent>(
                                channel.targetEntity))
                            touchedGaussianVisibility = true;
                    }
                });
        }
    }

    if (mode == AnimationEvaluateMode::DiscontinuousSeek)
    {
        // Accumulation alone is not enough  - realtime temporal history (TAA/NRD/DLSS)
        // must also drop on large seeks, otherwise the jump ghosts for many frames.
        pathTracerSettings().ResetAccumulation = true;
        pathTracerSettings().ResetRealtimeCaches = true;
    }

    if (touchedGaussianVisibility)
        renderRuntimeState().Invalidation.AccelerationStructRebuildRequested = true;
}

void SceneEditor::handleDroppedFiles()
{
    m_contentEditor.handleDroppedFiles(editorUIState().PendingDroppedFiles);
}

bool SceneEditor::loadMeshFile(const std::filesystem::path& filePath)
{
    return m_contentEditor.loadMeshFile(filePath);
}

bool SceneEditor::loadGltfMeshFile(const std::filesystem::path& filePath)
{
    return m_contentEditor.loadGltfMeshFile(filePath);
}

bool SceneEditor::loadObjMeshFile(const std::filesystem::path& filePath)
{
    return m_contentEditor.loadObjMeshFile(filePath);
}

bool SceneEditor::deleteEntity(caustica::ecs::Entity entity)
{
    return m_contentEditor.deleteEntity(entity);
}

void SceneEditor::processPendingSceneDeletes()
{
    if (editorUIState().PendingDeleteEntity == caustica::ecs::NullEntity || !m_app)
        return;

    const caustica::ecs::Entity entity = editorUIState().PendingDeleteEntity;
    editorUIState().PendingDeleteEntity = caustica::ecs::NullEntity;

    auto* ew = caustica::entityWorld(*m_app);
    if (!ew || !ew->world().isAlive(entity))
        return;

    deleteEntity(entity);
}

void SceneEditor::processPendingSceneCreates()
{
    if (editorUIState().PendingCreate == EditorSelectionState::PendingSceneCreate::None || !m_app)
        return;

    if (caustica::isSceneStructureBusy(*m_app) || !caustica::isSceneLoaded(*m_app))
        return;

    const auto kind = editorUIState().PendingCreate;
    editorUIState().PendingCreate = EditorSelectionState::PendingSceneCreate::None;

    switch (kind)
    {
    case EditorSelectionState::PendingSceneCreate::Plane:
        (void)createBuiltinMesh(BuiltinPrimitiveKind::Plane);
        break;
    case EditorSelectionState::PendingSceneCreate::Cube:
        (void)createBuiltinMesh(BuiltinPrimitiveKind::Cube);
        break;
    case EditorSelectionState::PendingSceneCreate::Sphere:
        (void)createBuiltinMesh(BuiltinPrimitiveKind::Sphere);
        break;
    case EditorSelectionState::PendingSceneCreate::Cylinder:
        (void)createBuiltinMesh(BuiltinPrimitiveKind::Cylinder);
        break;
    case EditorSelectionState::PendingSceneCreate::DirectionalLight:
        (void)createLight(EditorLightKind::Directional);
        break;
    case EditorSelectionState::PendingSceneCreate::PointLight:
        (void)createLight(EditorLightKind::Point);
        break;
    case EditorSelectionState::PendingSceneCreate::SpotLight:
        (void)createLight(EditorLightKind::Spot);
        break;
    case EditorSelectionState::PendingSceneCreate::RectLight:
        (void)createLight(EditorLightKind::Rect);
        break;
    case EditorSelectionState::PendingSceneCreate::EnvironmentLight:
        (void)createLight(EditorLightKind::Environment);
        break;
    case EditorSelectionState::PendingSceneCreate::None:
        break;
    }
}

caustica::ecs::Entity SceneEditor::createBuiltinMesh(BuiltinPrimitiveKind kind)
{
    return m_contentEditor.createBuiltinMesh(kind);
}

caustica::ecs::Entity SceneEditor::createLight(EditorLightKind kind)
{
    return m_contentEditor.createLight(kind);
}

void SceneEditor::requestCreateBuiltinMesh(BuiltinPrimitiveKind kind)
{
    using Kind = EditorSelectionState::PendingSceneCreate;
    switch (kind)
    {
    case BuiltinPrimitiveKind::Plane: editorUIState().PendingCreate = Kind::Plane; break;
    case BuiltinPrimitiveKind::Cube: editorUIState().PendingCreate = Kind::Cube; break;
    case BuiltinPrimitiveKind::Sphere: editorUIState().PendingCreate = Kind::Sphere; break;
    case BuiltinPrimitiveKind::Cylinder: editorUIState().PendingCreate = Kind::Cylinder; break;
    }
}

void SceneEditor::requestCreateLight(EditorLightKind kind)
{
    using Kind = EditorSelectionState::PendingSceneCreate;
    switch (kind)
    {
    case EditorLightKind::Directional: editorUIState().PendingCreate = Kind::DirectionalLight; break;
    case EditorLightKind::Point: editorUIState().PendingCreate = Kind::PointLight; break;
    case EditorLightKind::Spot: editorUIState().PendingCreate = Kind::SpotLight; break;
    case EditorLightKind::Rect: editorUIState().PendingCreate = Kind::RectLight; break;
    case EditorLightKind::Environment: editorUIState().PendingCreate = Kind::EnvironmentLight; break;
    }
}

void SceneEditor::requestFullRebuild()
{
    m_contentEditor.requestFullRebuild();
}

std::vector<float3> SceneEditor::getMeshVertices(caustica::ecs::Entity entity) const
{
    return m_contentEditor.getMeshVertices(entity);
}

std::vector<float3> SceneEditor::getMeshVerticesWorld(caustica::ecs::Entity entity)
{
    return m_contentEditor.getMeshVerticesWorld(entity);
}

void SceneEditor::setMeshVerticesWorld(caustica::ecs::Entity entity,
    const std::vector<float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    m_contentEditor.setMeshVerticesWorld(entity, vertices, recomputeNormals, rebuildAccelerationStructure);
}

void SceneEditor::setMeshVertices(caustica::ecs::Entity entity,
    const std::vector<float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    m_contentEditor.setMeshVertices(entity, vertices, recomputeNormals, rebuildAccelerationStructure);
}

ZoomTool* SceneEditor::getOrCreateZoomTool()
{
    if (m_zoomTool == nullptr && m_app)
        m_zoomTool = caustica::createZoomTool(*m_app);
    return m_zoomTool.get();
}

bool SceneEditor::showDeltaTree() const
{
    return editorUIState().ShowDeltaTree;
}

void SceneEditor::resolvePickFeedback(const DebugFeedbackStruct& feedback, const caustica::render::RenderPickState& renderedPick)
{
    if (!m_app)
        return;

    if (!renderedPick.InstanceRequested)
        return;

    // Publish POD results for the logic thread. SelectedEntity is UI state and
    // must not be mutated from the dedicated render thread.
    m_completedInstancePickIndex.store(
        int(feedback.pickedInstanceIndex),
        std::memory_order_relaxed);
    const uint64_t packedPosition = (uint64_t(renderedPick.Position.x) << 32u)
        | uint64_t(renderedPick.Position.y);
    m_completedInstancePickPosition.store(packedPosition, std::memory_order_relaxed);
    m_completedInstancePickRequestId.store(
        renderedPick.InstanceRequestId,
        std::memory_order_release);
}

void SceneEditor::afterWorldRender(GpuDevice& gpuDevice)
{
    if (!m_app)
        return;

    const auto& renderedPick = caustica::lastRenderedPicking(*m_app);
    if (renderedPick.MaterialRequested)
    {
        // Publish data before the release-store of its request identity. The
        // logic thread consumes it in prepareEditorFrame and owns UI mutation.
        m_completedMaterialPickGpuId.store(
            int(caustica::feedbackData(*m_app).pickedMaterialID),
            std::memory_order_relaxed);
        m_completedMaterialPickRequestId.store(
            renderedPick.MaterialRequestId,
            std::memory_order_release);
    }
    if (pathTracerSettings().ContinuousDebugFeedback || renderedPick.hasActivePickRequest())
        resolvePickFeedback(caustica::feedbackData(*m_app), renderedPick);

    if (renderedPick.InstanceRequested)
        renderRuntimeState().Picking.completeInstancePick(renderedPick.InstanceRequestId);

    auto saveFramebuffer = [this](const char* fileName) -> bool {
        if (!m_app)
            return false;
        GpuDevice* gpuDevice = m_app->getGpuDevice();
        if (!gpuDevice)
            return false;
        return caustica::saveCurrentFramebuffer(*m_app, *gpuDevice, fileName);
    };
    captureScriptPostRender(saveFramebuffer);

    if (consumeExperimentalPhotoScreenshot())
    {
        caustica::rhi::Framebuffer* framebuffer = gpuDevice.getCurrentFramebuffer(true);
        if (framebuffer)
            caustica::takeDenoisedScreenshot(*m_app, framebuffer->getDesc().colorAttachments[0].texture);
    }
}

bool SceneEditor::consumeExperimentalPhotoScreenshot()
{
    if (!editorUIState().ExperimentalPhotoModeScreenshot)
        return false;
    editorUIState().ExperimentalPhotoModeScreenshot = false;
    return true;
}

void SceneEditor::captureScriptPostRender(std::function<bool(const char* fileName)> saveTexture)
{
    if (m_captureScriptManager)
        m_captureScriptManager->postRender(saveTexture);
}

void SceneEditor::onEvent(caustica::Event& event)
{
    m_inputRouter.onEvent(event);
}

} // namespace caustica::editor
