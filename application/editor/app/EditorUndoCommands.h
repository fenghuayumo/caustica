#pragma once

#include "EditorUndoStack.h"

#include <ecs/Entity.h>
#include <math/math.h>
#include <math/quat.h>

#include <filesystem>
#include <memory>

namespace caustica::scene
{
class SceneEntityWorld;
struct LocalTransformComponent;
} // namespace caustica::scene

namespace caustica::editor
{

class SceneEditor;

struct LocalTransformSnapshot
{
    dm::double3 translation = 0.0;
    dm::dquat rotation = dm::dquat::identity();
    dm::double3 scaling = 1.0;
};

[[nodiscard]] LocalTransformSnapshot captureLocalTransform(const caustica::scene::LocalTransformComponent& local);
[[nodiscard]] LocalTransformSnapshot captureLocalTransform(
    const caustica::scene::SceneEntityWorld& entityWorld,
    ecs::Entity entity);
[[nodiscard]] bool transformsEqual(const LocalTransformSnapshot& a, const LocalTransformSnapshot& b);
bool applyLocalTransform(SceneEditor& sceneEditor, ecs::Entity entity, const LocalTransformSnapshot& snapshot);

class TransformUndoCommand final : public IEditorUndoCommand
{
public:
    TransformUndoCommand(
        SceneEditor& sceneEditor,
        ecs::Entity entity,
        std::filesystem::path entityPath,
        LocalTransformSnapshot before,
        LocalTransformSnapshot after);

    void undo() override;
    void redo() override;
    [[nodiscard]] const char* label() const override { return "Transform"; }

private:
    void apply(const LocalTransformSnapshot& snapshot) const;
    [[nodiscard]] ecs::Entity resolveEntity() const;

    SceneEditor* m_sceneEditor = nullptr;
    ecs::Entity m_entity = ecs::NullEntity;
    std::filesystem::path m_entityPath;
    LocalTransformSnapshot m_before;
    LocalTransformSnapshot m_after;
};

std::unique_ptr<TransformUndoCommand> makeTransformUndoCommand(
    SceneEditor& sceneEditor,
    ecs::Entity entity,
    const LocalTransformSnapshot& before,
    const LocalTransformSnapshot& after);

} // namespace caustica::editor
