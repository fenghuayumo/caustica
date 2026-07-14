#pragma once

#include <ecs/Entity.h>
#include <ecs/World.h>
#include <scene/SceneContent.h>
#include <scene/SceneEcs.h>
#include <scene/SceneObjects.h>

#include <memory>
#include <string>

namespace caustica::scene
{

[[nodiscard]] SceneContentFlags getMeshContentFlags(const MeshInfo& mesh);
[[nodiscard]] dm::box3 getMeshLocalBounds(const MeshInfo& mesh);
[[nodiscard]] bool setMeshProperty(MeshInfo& mesh, const std::string& propName, const dm::float4& value);

void initializeMeshInstanceComponent(MeshInstanceComponent& component, const std::shared_ptr<MeshInfo>& mesh);

[[nodiscard]] const MeshInstanceComponent* tryGetMeshInstance(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] std::shared_ptr<MeshInfo> getMeshAsset(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] bool hasSkinnedMesh(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] const SkinnedMeshComponent* tryGetSkinnedMesh(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] SkinnedMeshComponent* tryGetSkinnedMesh(ecs::World& world, ecs::Entity entity);

[[nodiscard]] std::shared_ptr<MeshInfo> createSkinnedMeshFromPrototype(
    SceneTypeFactory& factory, const std::shared_ptr<MeshInfo>& prototypeMesh);

} // namespace caustica::scene
