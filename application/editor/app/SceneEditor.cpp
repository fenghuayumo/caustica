#include "SceneEditor.h"

#include "SceneContentEditor.h"
#include "common/LocalConfig.h"
#include "common/CaptureScriptManager.h"
#include "common/TransformGizmo.h"
#include "ui/RenderSettingsConsole.h"

#include <render/WorldRenderer.h>
#include <render/core/TextureUtils.h>
#include <render/passes/debug/ZoomTool.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <assets/loader/ShaderFactory.h>
#include <engine/App.h>
#include <engine/GpuSharedCaches.h>
#include <engine/AppResources.h>
#include "EditorAccess.h"
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/CameraApi.h>
#include <engine/SceneLifecycle.h>
#include <engine/SceneQuery.h>
#include <engine/RenderSessionApi.h>
#include <engine/EnqueueRenderCommand.h>
#include <engine/SceneSession.h>
#include <core/path_utils.h>
#include <core/json.h>
#include <core/log.h>
#include <platform/file_dialog.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneManager.h>
#include <scene/scene_utils.h>
#include <scene/View.h>
#include <EditorUI.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>

#include "game/GameScene.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#if CAUSTICA_WITH_PYTHON
#include "Python/PythonScripting.h"
#endif

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

extern const char* g_windowTitle;

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
    : m_renderAppState(m_editorUiData.render)
    , m_settings(m_editorUiData.render.settings)
    , m_renderState(m_editorUiData.render.runtime)
    , m_editor(m_editorUiData.editor)
    , m_selectionState(m_editorUiData.editor)
    , m_editorCameraState(m_viewState)
    , m_inputRouter()
    , m_contentEditor(*this)
{
    m_viewState.progressLoading.start("Starting up...");
    m_viewState.progressLoading.Set(50);
    m_inputRouter.bind(*this);
    m_captureScriptManager = std::make_unique<CaptureScriptManager>(*this, m_renderAppState, m_cmdLine);
    m_captureScriptState.manager = m_captureScriptManager.get();
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

    auto* pathTracing = caustica::worldRenderer(*m_app);
    auto* entityWorld = caustica::entityWorld(*m_app);
    const auto& view = caustica::currentView(*m_app);
    if (!pathTracing || !entityWorld || !view)
        return ecs::NullEntity;

    const uint2 disp = caustica::displaySize(*m_app);
    if (disp.x == 0 || disp.y == 0)
        return ecs::NullEntity;

    // Picking.Position is already display/window space (see EditorInputRouter).
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

    for (const auto& object : pathTracing->gaussianSplatPasses().objects())
    {
        if (!ecs::isValid(object.entity) || !object.pass)
            continue;
        const auto* splatComp = entityWorld->world().tryGet<scene::GaussianSplatComponent>(object.entity);
        if (!splatComp || !splatComp->splat.enabled)
            continue;

        auto* boundsComp = entityWorld->world().tryGet<scene::BoundsComponent>(object.entity);
        box3 bbox = boundsComp ? boundsComp->globalBounds : box3::empty();
        if (bbox.isempty())
        {
            auto* global = entityWorld->world().tryGet<scene::GlobalTransformComponent>(object.entity);
            const box3 local = object.pass->getLocalBounds();
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
    if (!m_pythonScripting)
        m_pythonScripting = std::make_unique<PythonScripting>(*this);
#endif
    m_inputRouter.bind(*this);
}

void SceneEditor::onBeforeInitialSceneLoad()
{
    m_sampleGame = std::make_unique<::GameScene>(*this, m_cmdLine);
    m_viewState.progressLoading.Set(95);
}

void SceneEditor::prepareEditorFrame()
{
    m_settings.DebugExploreDeltaTree = m_editor.ShowDeltaTree;
}

void SceneEditor::onSceneUnloading()
{
    m_pendingEditAction = PendingEditAction::None;
    m_undoStack.clear();
    ResetTransformGizmoInteraction();
    m_editorAnimationEntity = ecs::NullEntity;
    m_editor.TogglableNodes = nullptr;
    m_editor.SelectedMaterial = nullptr;
    m_editor.SelectedEntity = caustica::ecs::NullEntity;
    m_editor.PendingDeleteEntity = caustica::ecs::NullEntity;
    m_editor.InspectorRotationEntity = caustica::ecs::NullEntity;
    m_editor.InspectorRotationEulerValid = false;
    m_editor.SelectedGaussianSplat = false;
    m_editorState.sceneDocument = Json::Value();
    m_editorState.sceneDocumentPath.clear();
    m_editorState.sceneDocumentValid = false;
    m_editorState.loadedSceneName.clear();

    if (m_sampleGame != nullptr)
        m_sampleGame->sceneUnloading();
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
    if (m_sampleGame == nullptr || !m_app)
        return;

    auto scene = caustica::activeScene(*m_app);
    if (!scene)
        return;

    const std::filesystem::path assetsRoot = getLocalPath(c_AssetsFolder);
    m_sampleGame->sceneLoaded(
        scene,
        caustica::currentScenePath(*m_app),
        assetsRoot);
}

void SceneEditor::onSceneLoadedBeforeGpuPrep()
{
    if (m_editor.TogglableNodes != nullptr || !m_app)
        return;

    if (auto* ew = caustica::entityWorld(*m_app))
    {
        m_editor.TogglableNodes = std::make_shared<std::vector<TogglableNode>>();
        UpdateTogglableNodes(*m_editor.TogglableNodes, *ew, ew->root());
    }
}

void SceneEditor::onSceneLoadedAfterCollectTextures()
{
    LocalConfig::PostSceneLoad(*this, m_renderAppState, m_editor);
}

void SceneEditor::onSceneLoadedComplete()
{
#if CAUSTICA_WITH_PYTHON
    if (m_pythonScripting
        && (!m_cmdLine.pythonScript.empty() || !m_cmdLine.pythonExpr.empty()))
    {
        if (m_pythonScripting->Initialize())
        {
            if (!m_cmdLine.pythonScript.empty())
                m_pythonScripting->QueueScriptFile(m_cmdLine.pythonScript);
            if (!m_cmdLine.pythonExpr.empty())
                m_pythonScripting->QueueScriptString(m_cmdLine.pythonExpr, "<--pythonExpr>");
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

ecs::Entity FindChildEntityByName(scene::SceneEntityWorld& ew, ecs::Entity parent, const std::string& name)
{
    if (!ecs::isValid(parent) || name.empty())
        return ecs::NullEntity;

    for (ecs::Entity child : ew.getEntityChildren(parent))
    {
        if (ew.getEntityName(child) == name)
            return child;
    }
    return ecs::NullEntity;
}

void WriteLocalTransformToJson(Json::Value& node, const scene::LocalTransformComponent& local)
{
    {
        Json::Value translation(Json::arrayValue);
        caustica::json::write(translation, local.translation);
        node["translation"] = translation;
    }
    {
        Json::Value rotation(Json::arrayValue);
        const dm::double4 xyzw(
            local.rotation.x, local.rotation.y, local.rotation.z, local.rotation.w);
        caustica::json::write(rotation, xyzw);
        node["rotation"] = rotation;
        node.removeMember("euler");
    }
    {
        Json::Value scaling(Json::arrayValue);
        caustica::json::write(scaling, local.scaling);
        node["scaling"] = scaling;
    }
}

void PatchSceneGraphTransforms(
    Json::Value& nodes,
    scene::SceneEntityWorld& ew,
    ecs::Entity parent)
{
    if (!nodes.isArray())
        return;

    for (Json::Value& node : nodes)
    {
        if (!node.isObject() || !node["name"].isString())
            continue;

        const std::string name = node["name"].asString();
        const ecs::Entity entity = FindChildEntityByName(ew, parent, name);
        if (!ecs::isValid(entity))
            continue;

        if (const auto* local = ew.world().tryGet<scene::LocalTransformComponent>(entity);
            local && local->hasLocalTransform)
        {
            WriteLocalTransformToJson(node, *local);
        }

        if (node.isMember("children"))
            PatchSceneGraphTransforms(node["children"], ew, entity);
    }
}

void SaveSceneMaterials(App& app)
{
    auto* wr = app.tryResource<render::WorldRenderer>();
    if (!wr)
        return;
    if (auto materials = wr->lightingPasses().materials())
        materials->saveAll();
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

    if (editorState.sceneDocument.isMember("graph"))
        PatchSceneGraphTransforms(editorState.sceneDocument["graph"], *ew, ew->root());
    PatchEditorAnimations(editorState.sceneDocument, *ew);

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
    if (!caustica::FileDialog(
            true,
            "Scene files (*.scene.json;*.json)\0*.scene.json;*.json\0All files\0*.*\0",
            picked))
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
    if (auto* session = m_app->tryResource<SceneSession>(); session && session->manager)
        session->manager->retargetCurrentScene(sceneName, path);

    if (auto scene = caustica::activeScene(*m_app))
        caustica::commitActiveScene(*m_app, std::move(scene), sceneName, path);

    m_editorState.loadedSceneName = sceneName;
    caustica::info("Saved scene as '%s'", path.generic_string().c_str());
    return true;
}

void SceneEditor::onAnimateBegin(float& fElapsedTimeSeconds)
{
    m_captureScriptManager->preAnim(fElapsedTimeSeconds);

#if CAUSTICA_WITH_PYTHON
    if (m_pythonScripting && m_app && caustica::isSceneLoaded(*m_app))
        m_pythonScripting->ProcessPendingScripts();
#endif
}

void SceneEditor::onAnimateGameTick(float fElapsedTimeSeconds, bool enableAnimations)
{
    if (m_sampleGame)
        m_sampleGame->Tick(fElapsedTimeSeconds, enableAnimations);
}

void SceneEditor::onAnimateUpdateSceneTime(float /*fElapsedTimeSeconds*/, bool enableAnimations, bool /*enableAnimationUpdate*/)
{
    if (enableAnimations && m_sampleGame && m_sampleGame->IsInitialized())
        m_viewState.sceneTime = m_sampleGame->gameTime();
}

void SceneEditor::onAnimateGameCamera(float fElapsedTimeSeconds)
{
    auto* cam = m_app ? caustica::cameraController(*m_app) : nullptr;
    if (m_sampleGame && cam)
        m_sampleGame->TickCamera(fElapsedTimeSeconds, cam->camera());
}

void SceneEditor::onAnimateEnd(float /*fElapsedTimeSeconds*/)
{
    m_captureScriptManager->PostAnim();
}

void SceneEditor::updateWindowTitle()
{
    if (!m_app)
        return;

    auto* device = m_app->getGpuDevice();
    auto scene = caustica::activeScene(*m_app);
    if (!device || !scene)
        return;

    std::string extraInfo = ", " + caustica::fpsInfo(*m_app) + ", " + caustica::currentSceneName(*m_app)
        + ", " + caustica::resolutionInfo(*m_app) + ", (L: " + std::to_string(scene->getLightEntities().size())
        + ", MAT: " + std::to_string(scene->getMaterials().size())
        + ", MESH: " + std::to_string(scene->getMeshes().size())
        + ", I: " + std::to_string(scene->getMeshInstances().size())
        + ", SI: " + std::to_string(scene->getSkinnedMeshInstances().size())
#if ENABLE_DEBUG_VIZUALISATIONS
        + ", ENABLE_DEBUG_VIZUALISATIONS: 1"
#endif
        + ")";

    device->setInformativeWindowTitle(g_windowTitle, false, extraInfo.c_str());
}

void SceneEditor::setSceneTime(double sceneTime)
{
    if (m_sampleGame && m_sampleGame->IsInitialized())
        m_sampleGame->SetGameTime(sceneTime);
    m_viewState.sceneTime = sceneTime;
}

double SceneEditor::sceneTime() const
{
    if (m_sampleGame && m_sampleGame->IsInitialized())
        return m_sampleGame->gameTime();
    return m_viewState.sceneTime;
}

void SceneEditor::setTimelineTime(double timelineTime)
{
    m_viewState.keyframeTime = timelineTime;
}

double SceneEditor::timelineTime() const
{
    return m_viewState.keyframeTime;
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
    m_settings.ResetAccumulation = true;
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
        m_settings.ResetAccumulation = true;
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
    m_settings.ResetAccumulation = true;
    if (entityWorld->world().tryGet<scene::GaussianSplatComponent>(entity))
        m_renderState.Invalidation.AccelerationStructRebuildRequested = true;
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
    m_settings.ResetAccumulation = true;
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
        m_settings.ResetAccumulation = true;
        m_settings.ResetRealtimeCaches = true;
    }

    if (touchedGaussianVisibility)
        m_renderState.Invalidation.AccelerationStructRebuildRequested = true;
}

void SceneEditor::handleDroppedFiles()
{
    m_contentEditor.handleDroppedFiles(m_editor.PendingDroppedFiles);
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
    if (m_editor.PendingDeleteEntity == caustica::ecs::NullEntity || !m_app)
        return;

    const caustica::ecs::Entity entity = m_editor.PendingDeleteEntity;
    m_editor.PendingDeleteEntity = caustica::ecs::NullEntity;

    auto* ew = caustica::entityWorld(*m_app);
    if (!ew || !ew->world().isAlive(entity))
        return;

    deleteEntity(entity);
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
    auto* infra = m_app ? caustica::gpuSharedCaches(*m_app) : nullptr;
    auto* device = m_app ? m_app->getGpuDevice() : nullptr;
    if (m_zoomTool == nullptr && infra && infra->shaderFactory && device)
        m_zoomTool = std::make_unique<ZoomTool>(device->getDevice(), infra->shaderFactory);
    return m_zoomTool.get();
}

bool SceneEditor::showDeltaTree() const
{
    return m_editor.ShowDeltaTree;
}

void SceneEditor::resolvePickFeedback(const DebugFeedbackStruct& feedback, const caustica::render::RenderPickState& renderedPick)
{
    if (!m_app)
        return;

    if (renderedPick.MaterialRequested)
        m_editor.SelectedMaterial = caustica::findMaterial(*m_app, int(feedback.pickedMaterialID));
    if (renderedPick.InstanceRequested)
    {
        ecs::Entity picked = caustica::findEntityByInstanceIndex(*m_app, int(feedback.pickedInstanceIndex));
        bool pickedGaussian = false;
        if (picked == ecs::NullEntity)
        {
            picked = pickGaussianSplatAtPixel(renderedPick.Position);
            pickedGaussian = (picked != ecs::NullEntity);
        }
        m_editor.SelectedEntity = picked;
        m_editor.SelectedGaussianSplat = pickedGaussian;
    }
}

void SceneEditor::afterWorldRender(GpuDevice& gpuDevice)
{
    auto* wr = m_app ? caustica::worldRenderer(*m_app) : nullptr;
    if (!wr)
        return;

    const auto& renderedPick = wr->getLastRenderedPicking();
    if (m_settings.ContinuousDebugFeedback || renderedPick.hasActivePickRequest())
        resolvePickFeedback(wr->getFeedbackData(), renderedPick);

    if (renderedPick.MaterialRequested)
        m_renderState.Picking.MaterialRequested = false;
    if (renderedPick.InstanceRequested)
        m_renderState.Picking.InstanceRequested = false;

    auto saveFramebuffer = [this, &gpuDevice](const char* fileName) -> bool {
        caustica::rhi::Framebuffer* framebuffer = gpuDevice.getCurrentFramebuffer(true);
        auto* infra = m_app ? caustica::gpuSharedCaches(*m_app) : nullptr;
        if (!framebuffer || !infra || !infra->renderDevice)
            return false;
        caustica::rhi::Texture* texture = framebuffer->getDesc().colorAttachments[0].texture;
        auto* renderDevice = infra->renderDevice.get();
        return saveTextureToFile(
            gpuDevice.getDevice(), *renderDevice, texture, caustica::rhi::ResourceStates::Common, fileName);
    };
    captureScriptPostRender(saveFramebuffer);

    if (consumeExperimentalPhotoScreenshot())
    {
        caustica::rhi::Framebuffer* framebuffer = gpuDevice.getCurrentFramebuffer(true);
        if (framebuffer)
            wr->denoisedScreenshot(framebuffer->getDesc().colorAttachments[0].texture);
    }
}

bool SceneEditor::consumeExperimentalPhotoScreenshot()
{
    if (!m_editor.ExperimentalPhotoModeScreenshot)
        return false;
    m_editor.ExperimentalPhotoModeScreenshot = false;
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
