#pragma once

#include <animation/KeyframeAnimation.h>
#include <ecs/Entity.h>
#include <ecs/World.h>
#include <scene/SceneAnimation.h>
#include <scene/SceneEcs.h>

#include <memory>
#include <string>

namespace caustica::scene
{

[[nodiscard]] SceneContentFlags GetAnimationContentFlags();

[[nodiscard]] bool IsAnimationChannelValid(const AnimationChannelData& channel);
[[nodiscard]] bool IsAnimationValid(const AnimationComponent& component);
[[nodiscard]] float GetAnimationDuration(const AnimationComponent& component);

void AddAnimationChannel(AnimationComponent& component, AnimationChannelData channel);
void RecalculateAnimationDuration(AnimationComponent& component);

[[nodiscard]] bool ApplyAnimationChannel(
    const AnimationChannelData& channel, float time, SceneEntityWorld& world);
[[nodiscard]] bool ApplyAnimation(AnimationComponent& component, float time, SceneEntityWorld& world);

void InitializeAnimationComponent(AnimationComponent& component, const SceneAnimation& animation);
void InitializeAnimationComponent(AnimationComponent& component, const std::shared_ptr<SceneAnimation>& animation);

[[nodiscard]] const AnimationComponent* TryGetAnimation(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] AnimationComponent* TryGetAnimation(ecs::World& world, ecs::Entity entity);

} // namespace caustica::scene
