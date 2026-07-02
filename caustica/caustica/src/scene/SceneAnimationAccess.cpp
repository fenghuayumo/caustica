#include <scene/SceneAnimationAccess.h>

#include <scene/SceneLightAccess.h>
#include <scene/SceneMeshAccess.h>
#include <core/log.h>

namespace caustica::scene
{

SceneContentFlags GetAnimationContentFlags()
{
    return SceneContentFlags::Animations;
}

bool IsAnimationChannelValid(const AnimationChannelData& channel)
{
    return ecs::isValid(channel.targetEntity) || channel.targetMaterial != nullptr;
}

bool IsAnimationValid(const AnimationComponent& component)
{
    if (component.channels.empty())
        return false;

    for (const auto& channel : component.channels)
    {
        if (!IsAnimationChannelValid(channel))
            return false;
    }
    return true;
}

float GetAnimationDuration(const AnimationComponent& component)
{
    return component.duration;
}

void AddAnimationChannel(AnimationComponent& component, AnimationChannelData channel)
{
    if (channel.sampler)
        component.duration = std::max(component.duration, channel.sampler->GetEndTime());
    component.channels.push_back(std::move(channel));
}

void RecalculateAnimationDuration(AnimationComponent& component)
{
    component.duration = 0.f;
    for (const auto& channel : component.channels)
    {
        if (channel.sampler)
            component.duration = std::max(component.duration, channel.sampler->GetEndTime());
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

bool ApplyAnimationChannel(const AnimationChannelData& channel, float time, SceneEntityWorld& world)
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

    const auto valueOption = channel.sampler->Evaluate(time, true);
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
            if (!channel.targetMaterial->SetProperty(channel.leafPropertyName, value))
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
                if (!SetMeshProperty(*meshComp->mesh, channel.leafPropertyName, value))
                {
                    caustica::warning("Cannot set property '%s' on mesh instance: the instance doesn't support this property.",
                        channel.leafPropertyName.c_str());
                }
            }
            else if (auto* lightComp = world.world().get<LightComponent>(channel.targetEntity))
            {
                if (!SetLightProperty(*lightComp, channel.leafPropertyName, value))
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

bool ApplyAnimation(AnimationComponent& component, float time, SceneEntityWorld& world)
{
    bool success = true;
    for (const auto& channel : component.channels)
        success = ApplyAnimationChannel(channel, time, world) && success;

    if (success)
        world.markTransformDirty();

    return success;
}

void InitializeAnimationComponent(AnimationComponent& component, const SceneAnimation& animation)
{
    component.channels.clear();
    component.duration = animation.GetDuration();
    component.channels.reserve(animation.GetChannels().size());

    for (const auto& channel : animation.GetChannels())
    {
        AnimationChannelData data;
        data.sampler = channel->GetSampler();
        data.targetEntity = channel->GetTargetEntity();
        data.targetMaterial = channel->GetTargetMaterial();
        data.attribute = channel->GetAttribute();
        data.leafPropertyName = channel->GetLeafPropertyName();
        component.channels.push_back(std::move(data));
    }
}

void InitializeAnimationComponent(AnimationComponent& component, const std::shared_ptr<SceneAnimation>& animation)
{
    if (animation)
        InitializeAnimationComponent(component, *animation);
}

const AnimationComponent* TryGetAnimation(const ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<AnimationComponent>(entity);
}

AnimationComponent* TryGetAnimation(ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<AnimationComponent>(entity);
}

} // namespace caustica::scene
