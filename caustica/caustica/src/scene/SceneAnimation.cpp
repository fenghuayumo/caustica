#include <scene/SceneAnimation.h>
#include <scene/SceneAnimationAccess.h>

namespace caustica
{

bool SceneAnimationChannel::IsValid() const
{
    scene::AnimationChannelData data;
    data.sampler = m_Sampler;
    data.targetEntity = m_TargetEntity;
    data.targetMaterial = m_TargetMaterial.lock();
    data.attribute = m_Attribute;
    data.leafPropertyName = m_LeafPropertyName;
    return scene::IsAnimationChannelValid(data);
}

bool SceneAnimationChannel::Apply(float time, scene::SceneEntityWorld& world) const
{
    scene::AnimationChannelData data;
    data.sampler = m_Sampler;
    data.targetEntity = m_TargetEntity;
    data.targetMaterial = m_TargetMaterial.lock();
    data.attribute = m_Attribute;
    data.leafPropertyName = m_LeafPropertyName;
    return scene::ApplyAnimationChannel(data, time, world);
}

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
    scene::AnimationComponent component;
    scene::InitializeAnimationComponent(component, *this);
    return scene::ApplyAnimation(component, time, world);
}

void SceneAnimation::AddChannel(const std::shared_ptr<SceneAnimationChannel>& channel)
{
    m_Channels.push_back(channel);
    m_Duration = std::max(m_Duration, channel->GetSampler()->GetEndTime());
}

bool SceneAnimation::IsVald() const
{
    scene::AnimationComponent component;
    scene::InitializeAnimationComponent(component, *this);
    return scene::IsAnimationValid(component);
}

} // namespace caustica
