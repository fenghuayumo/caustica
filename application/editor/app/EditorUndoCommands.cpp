#include "EditorUndoCommands.h"

#include "SceneEditor.h"
#include "EditorAccess.h"

#include <engine/App.h>
#include <scene/SceneEcs.h>

#include <cmath>

namespace caustica::editor
{

namespace
{

constexpr double kTransformEpsilon = 1e-9;

bool nearlyEqual(double a, double b)
{
    return std::abs(a - b) <= kTransformEpsilon;
}

bool nearlyEqual(const dm::double3& a, const dm::double3& b)
{
    return nearlyEqual(a.x, b.x) && nearlyEqual(a.y, b.y) && nearlyEqual(a.z, b.z);
}

bool nearlyEqual(const dm::dquat& a, const dm::dquat& b)
{
    // q and -q represent the same rotation.
    return std::abs(std::abs(dm::dot(a, b)) - 1.0) <= 1e-8;
}

} // namespace

LocalTransformSnapshot captureLocalTransform(const caustica::scene::LocalTransformComponent& local)
{
    LocalTransformSnapshot snapshot;
    snapshot.translation = local.translation;
    snapshot.rotation = local.rotation;
    snapshot.scaling = local.scaling;
    return snapshot;
}

LocalTransformSnapshot captureLocalTransform(
    const caustica::scene::SceneEntityWorld& entityWorld,
    ecs::Entity entity)
{
    if (const auto* local = entityWorld.world().tryGet<caustica::scene::LocalTransformComponent>(entity))
        return captureLocalTransform(*local);
    return {};
}

bool transformsEqual(const LocalTransformSnapshot& a, const LocalTransformSnapshot& b)
{
    return nearlyEqual(a.translation, b.translation)
        && nearlyEqual(a.rotation, b.rotation)
        && nearlyEqual(a.scaling, b.scaling);
}

bool applyLocalTransform(SceneEditor& sceneEditor, ecs::Entity entity, const LocalTransformSnapshot& snapshot)
{
    App* app = sceneEditor.app();
    if (!app || !ecs::isValid(entity))
        return false;

    auto* entityWorld = caustica::entityWorld(*app);
    if (!entityWorld || !entityWorld->world().isAlive(entity))
        return false;

    const auto finite3 = [](const dm::double3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    const auto finiteQ = [](const dm::dquat& q) {
        return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
    };
    if (!finite3(snapshot.translation) || !finiteQ(snapshot.rotation) || !finite3(snapshot.scaling))
        return false;

    entityWorld->setLocalTransform(
        entity,
        &snapshot.translation,
        &snapshot.rotation,
        &snapshot.scaling);
    entityWorld->refreshHierarchy(caustica::scene::PreviousTransformPolicy::PreserveExisting);

    auto& editorUI = sceneEditor.editorUIState();
    editorUI.InspectorRotationEntity = entity;
    editorUI.InspectorRotationEulerValid = false;
    sceneEditor.pathTracerSettings().ResetAccumulation = true;
    return true;
}

TransformUndoCommand::TransformUndoCommand(
    SceneEditor& sceneEditor,
    ecs::Entity entity,
    std::filesystem::path entityPath,
    LocalTransformSnapshot before,
    LocalTransformSnapshot after)
    : m_sceneEditor(&sceneEditor)
    , m_entity(entity)
    , m_entityPath(std::move(entityPath))
    , m_before(before)
    , m_after(after)
{
}

ecs::Entity TransformUndoCommand::resolveEntity() const
{
    if (!m_sceneEditor || !m_sceneEditor->app())
        return ecs::NullEntity;

    auto* entityWorld = caustica::entityWorld(*m_sceneEditor->app());
    if (!entityWorld)
        return ecs::NullEntity;

    if (ecs::isValid(m_entity) && entityWorld->world().isAlive(m_entity))
    {
        if (m_entityPath.empty() || entityWorld->getEntityPath(m_entity) == m_entityPath)
            return m_entity;
    }

    if (!m_entityPath.empty())
        return entityWorld->entityForPath(m_entityPath);

    return ecs::NullEntity;
}

void TransformUndoCommand::apply(const LocalTransformSnapshot& snapshot) const
{
    if (!m_sceneEditor)
        return;
    applyLocalTransform(*m_sceneEditor, resolveEntity(), snapshot);
}

void TransformUndoCommand::undo()
{
    apply(m_before);
}

void TransformUndoCommand::redo()
{
    apply(m_after);
}

std::unique_ptr<TransformUndoCommand> makeTransformUndoCommand(
    SceneEditor& sceneEditor,
    ecs::Entity entity,
    const LocalTransformSnapshot& before,
    const LocalTransformSnapshot& after)
{
    if (!ecs::isValid(entity) || transformsEqual(before, after))
        return nullptr;

    std::filesystem::path entityPath;
    if (App* app = sceneEditor.app())
    {
        if (auto* entityWorld = caustica::entityWorld(*app))
            entityPath = entityWorld->getEntityPath(entity);
    }

    return std::make_unique<TransformUndoCommand>(
        sceneEditor,
        entity,
        std::move(entityPath),
        before,
        after);
}

} // namespace caustica::editor
