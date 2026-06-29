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

[[nodiscard]] SceneContentFlags GetMeshContentFlags(const MeshInfo& mesh);
[[nodiscard]] dm::box3 GetMeshLocalBounds(const MeshInfo& mesh);
[[nodiscard]] bool SetMeshProperty(MeshInfo& mesh, const std::string& propName, const dm::float4& value);

void InitializeMeshInstanceComponent(MeshInstanceComponent& component, const std::shared_ptr<MeshInfo>& mesh);

[[nodiscard]] const MeshInstanceComponent* TryGetMeshInstance(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] std::shared_ptr<MeshInfo> GetMeshAsset(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] bool HasSkinnedMesh(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] const SkinnedMeshComponent* TryGetSkinnedMesh(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] SkinnedMeshComponent* TryGetSkinnedMesh(ecs::World& world, ecs::Entity entity);
[[nodiscard]] const SkinnedMeshGpuComponent* TryGetSkinnedMeshGpu(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] SkinnedMeshGpuComponent* TryGetSkinnedMeshGpu(ecs::World& world, ecs::Entity entity);

[[nodiscard]] std::shared_ptr<MeshInfo> CreateSkinnedMeshFromPrototype(
    SceneTypeFactory& factory, const std::shared_ptr<MeshInfo>& prototypeMesh);

} // namespace caustica::scene
