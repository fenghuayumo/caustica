#pragma once

#include <ecs/Entity.h>
#include <ecs/World.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace caustica::scene
{

// FNV-1a. Never returns 0 (reserved for miss / unlabeled background).
[[nodiscard]] uint32_t hashStableLabel(std::string_view text);

// Stable per-entity instance id for sensor AOVs.
// Prefers SemanticLabelComponent::instanceId when non-zero, then hashes
// SceneAuthoringId, then the entity path. 0 is never returned for a live entity.
[[nodiscard]] uint32_t resolveInstanceId(const ecs::World& world, ecs::Entity entity);

// Semantic class id. 0 = unlabeled. Prefers SemanticLabelComponent::semanticId,
// otherwise hashes SemanticLabelComponent::semanticLabel when set.
[[nodiscard]] uint32_t resolveSemanticId(const ecs::World& world, ecs::Entity entity);

[[nodiscard]] std::string resolveSemanticLabel(const ecs::World& world, ecs::Entity entity);

// Create or update SemanticLabelComponent. instanceId/semanticId 0 keep auto-resolve.
bool setSemanticLabel(
    ecs::World& world,
    ecs::Entity entity,
    uint32_t instanceId,
    uint32_t semanticId,
    std::string semanticLabel = {});

} // namespace caustica::scene
