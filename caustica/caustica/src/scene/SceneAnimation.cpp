#include <scene/SceneAnimation.h>
#include <scene/SceneEcs.h>
#include <scene/SceneMeshAccess.h>
#include <core/log.h>

using namespace caustica;
using namespace caustica::scene;

// =============================================================================
// SceneAnimationChannel
// =============================================================================

bool SceneAnimationChannel::IsValid() const
{
    return ecs::isValid(m_TargetEntity) || !m_TargetMaterial.expired();
}

bool SceneAnimationChannel::Apply(float time, scene::SceneEntityWorld& world) const
{
    auto material = m_TargetMaterial.lock();

    if (m_Attribute != AnimationAttribute::LeafProperty && !ecs::isValid(m_TargetEntity))
        return false;

    if (m_Attribute == AnimationAttribute::LeafProperty && !material && !ecs::isValid(m_TargetEntity))
        return false;

    auto valueOption = m_Sampler->Evaluate(time, true);
    if (!valueOption.has_value())
        return false;

    const auto value = valueOption.value();

    switch (m_Attribute)
    {
    case AnimationAttribute::Scaling:
        world.setScaling(m_TargetEntity, dm::double3(value.xyz()));
        break;

    case AnimationAttribute::Rotation: {
        dm::dquat quat = dm::dquat::fromXYZW(dm::double4(value));
        double len = length(quat);
        if (len == 0.0)
        {
            caustica::warning("Rotation quaternion interpolated to zero, ignoring.");
        }
        else
        {
            quat /= len;
            world.setRotation(m_TargetEntity, quat);
        }
        break;
    }

    case AnimationAttribute::Translation:
        world.setTranslation(m_TargetEntity, dm::double3(value.xyz()));
        break;

    case AnimationAttribute::LeafProperty: {
        if (material)
        {
            if (!material->SetProperty(m_LeafPropertyName, value))
            {
                caustica::warning("Cannot set property '%s' on material '%s': the material doesn't support this property.",
                    m_LeafPropertyName.c_str(), material->name.c_str());
            }
        }
        else if (ecs::isValid(m_TargetEntity))
        {
            // Look up a MeshInstanceComponent on the target entity and forward to SetProperty.
            auto* meshComp = world.world().get<scene::MeshInstanceComponent>(m_TargetEntity);
            if (meshComp && meshComp->mesh)
            {
                if (!SetMeshProperty(*meshComp->mesh, m_LeafPropertyName, value))
                {
                    caustica::warning("Cannot set property '%s' on mesh instance: the instance doesn't support this property.",
                        m_LeafPropertyName.c_str());
                }
            }
            else
            {
                auto* lightComp = world.world().get<scene::LightComponent>(m_TargetEntity);
                if (lightComp && lightComp->light)
                {
                    if (!lightComp->light->SetProperty(m_LeafPropertyName, value))
                    {
                        caustica::warning("Cannot set property '%s' on light: the light doesn't support this property.",
                            m_LeafPropertyName.c_str());
                    }
                }
                else
                {
                    caustica::warning("Cannot set property '%s': entity has no supported component with this property.",
                        m_LeafPropertyName.c_str());
                }
            }
        }
        break;
    }

    case AnimationAttribute::Undefined:
    default:
        caustica::warning("Unsupported animation target (%d), ignoring.", uint32_t(m_Attribute));
        return false;
    }

    return true;
}

// =============================================================================
// SceneAnimation
// =============================================================================

std::shared_ptr<SceneAnimation> SceneAnimation::Clone()
{
    auto copy = std::make_shared<SceneAnimation>();
    copy->name = name;
    for (const auto& channel : m_Channels)
    {
        std::shared_ptr<SceneAnimationChannel> channelCopy;

        auto targetMaterial = channel->GetTargetMaterial();
        if (targetMaterial)
        {
            channelCopy = std::make_shared<SceneAnimationChannel>(channel->GetSampler(), targetMaterial);
        }
        else
        {
            channelCopy = std::make_shared<SceneAnimationChannel>(
                channel->GetSampler(), channel->GetTargetEntity(), channel->GetAttribute());
        }

        channelCopy->SetLeafPropertyName(channel->GetLeafPropertyName());
        copy->AddChannel(channelCopy);
    }
    return copy;
}

bool SceneAnimation::Apply(float time, scene::SceneEntityWorld& world) const
{
    bool success = true;
    for (const auto& channel : m_Channels)
        success = channel->Apply(time, world) && success;
    return success;
}

void SceneAnimation::AddChannel(const std::shared_ptr<SceneAnimationChannel>& channel)
{
    m_Channels.push_back(channel);
    m_Duration = std::max(m_Duration, channel->GetSampler()->GetEndTime());
}

bool SceneAnimation::IsVald() const
{
    for (const auto& channel : m_Channels)
    {
        if (!channel->IsValid())
            return false;
    }
    return true;
}
