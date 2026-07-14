#include <scene/SceneAnimationAccess.h>

#include <scene/SceneLightAccess.h>
#include <scene/SceneEcs.h>
#include <core/log.h>

namespace caustica::scene
{

SceneContentFlags getAnimationContentFlags()
{
    return SceneContentFlags::Animations;
}

bool isAnimationChannelValid(const AnimationChannelData& channel)
{
    return ecs::isValid(channel.targetEntity) || channel.targetMaterial != nullptr;
}

bool isAnimationValid(const AnimationComponent& component)
{
    if (component.channels.empty())
        return false;

    for (const auto& channel : component.channels)
    {
        if (!isAnimationChannelValid(channel))
            return false;
    }
    return true;
}

float getAnimationDuration(const AnimationComponent& component)
{
    return component.duration;
}

void addAnimationChannel(AnimationComponent& component, AnimationChannelData channel)
{
    if (channel.sampler)
        component.duration = std::max(component.duration, channel.sampler->getEndTime());
    component.channels.push_back(std::move(channel));
}

void recalculateAnimationDuration(AnimationComponent& component)
{
    component.duration = 0.f;
    for (const auto& channel : component.channels)
    {
        if (channel.sampler)
            component.duration = std::max(component.duration, channel.sampler->getEndTime());
    }
}

namespace
{
void MarkSkinnedMeshDirtyForTransformChannel(
    SceneEntityWorld& world,
    const AnimationChannelData& channel)
{
    if (channel.attribute != AnimationAttribute::Translation
        && channel.attribute != AnimationAttribute::Rotation
        && channel.attribute != AnimationAttribute::Scaling)
    {
        return;
    }

    world.markSkinnedMeshDirtyForJoint(channel.targetEntity);
}
} // namespace

bool applyAnimationChannel(const AnimationChannelData& channel, float time, SceneEntityWorld& world)
{
    if (channel.attribute != AnimationAttribute::LeafProperty && !ecs::isValid(channel.targetEntity))
        return false;

    if (channel.attribute == AnimationAttribute::LeafProperty
        && !channel.targetMaterial && !ecs::isValid(channel.targetEntity))
    {
        return false;
    }

    if (!channel.sampler)
        return false;

    const auto valueOption = channel.sampler->evaluate(time, true);
    if (!valueOption.has_value())
        return false;

    const auto value = valueOption.value();

    switch (channel.attribute)
    {
    case AnimationAttribute::Scaling:
        world.setScaling(channel.targetEntity, dm::double3(value.xyz()));
        MarkSkinnedMeshDirtyForTransformChannel(world, channel);
        break;

    case AnimationAttribute::Rotation: {
        dm::dquat quat = dm::dquat::fromXYZW(dm::double4(value));
        const double len = length(quat);
        if (len == 0.0)
        {
            caustica::warning("Rotation quaternion interpolated to zero, ignoring.");
        }
        else
        {
            quat /= len;
            world.setRotation(channel.targetEntity, quat);
            MarkSkinnedMeshDirtyForTransformChannel(world, channel);
        }
        break;
    }

    case AnimationAttribute::Translation:
        world.setTranslation(channel.targetEntity, dm::double3(value.xyz()));
        MarkSkinnedMeshDirtyForTransformChannel(world, channel);
        break;

    case AnimationAttribute::LeafProperty: {
        if (channel.targetMaterial)
        {
            if (!channel.targetMaterial->setProperty(channel.leafPropertyName, value))
            {
                caustica::warning("Cannot set property '%s' on material '%s': the material doesn't support this property.",
                    channel.leafPropertyName.c_str(), channel.targetMaterial->name.c_str());
            }
        }
        else if (ecs::isValid(channel.targetEntity))
        {
            auto* meshComp = world.world().get<MeshInstanceComponent>(channel.targetEntity);
            if (meshComp && meshComp->mesh)
            {
                if (!setMeshProperty(*meshComp->mesh, channel.leafPropertyName, value))
                {
                    caustica::warning("Cannot set property '%s' on mesh instance: the instance doesn't support this property.",
                        channel.leafPropertyName.c_str());
                }
            }
            else if (auto* lightComp = world.world().get<LightComponent>(channel.targetEntity))
            {
                if (!setLightProperty(*lightComp, channel.leafPropertyName, value))
                {
                    caustica::warning("Cannot set property '%s' on light: the light doesn't support this property.",
                        channel.leafPropertyName.c_str());
                }
            }
            else
            {
                caustica::warning("Cannot set property '%s': entity has no supported component with this property.",
                    channel.leafPropertyName.c_str());
            }
        }
        break;
    }

    case AnimationAttribute::Undefined:
    default:
        caustica::warning("Unsupported animation target (%d), ignoring.", uint32_t(channel.attribute));
        return false;
    }

    return true;
}

bool applyAnimation(AnimationComponent& component, float time, SceneEntityWorld& world)
{
    bool success = true;
    for (const auto& channel : component.channels)
        success = applyAnimationChannel(channel, time, world) && success;

    if (success)
        world.markTransformDirty();

    return success;
}

const AnimationComponent* tryGetAnimation(const ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<AnimationComponent>(entity);
}

AnimationComponent* tryGetAnimation(ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<AnimationComponent>(entity);
}

} // namespace caustica::scene
