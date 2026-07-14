#pragma once

#include <animation/KeyframeAnimation.h>
#include <ecs/Entity.h>
#include <ecs/World.h>
#include <scene/SceneEcs.h>

#include <string>

namespace caustica::scene
{

[[nodiscard]] SceneContentFlags getAnimationContentFlags();

[[nodiscard]] bool isAnimationChannelValid(const AnimationChannelData& channel);
[[nodiscard]] bool isAnimationValid(const AnimationComponent& component);
[[nodiscard]] float getAnimationDuration(const AnimationComponent& component);

void addAnimationChannel(AnimationComponent& component, AnimationChannelData channel);
void recalculateAnimationDuration(AnimationComponent& component);

[[nodiscard]] bool applyAnimationChannel(
    const AnimationChannelData& channel, float time, SceneEntityWorld& world);
[[nodiscard]] bool applyAnimation(AnimationComponent& component, float time, SceneEntityWorld& world);

[[nodiscard]] const AnimationComponent* tryGetAnimation(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] AnimationComponent* tryGetAnimation(ecs::World& world, ecs::Entity entity);

} // namespace caustica::scene
