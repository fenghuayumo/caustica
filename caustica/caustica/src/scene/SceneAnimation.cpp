#include <scene/SceneAnimation.h>
#include <scene/SceneAnimationAccess.h>

namespace caustica
{

bool SceneAnimationChannel::isValid() const
{
    scene::AnimationChannelData data;
    data.sampler = m_Sampler;
    data.targetEntity = m_TargetEntity;
    data.targetMaterial = m_TargetMaterial.lock();
    data.attribute = m_Attribute;
    data.leafPropertyName = m_LeafPropertyName;
    return scene::isAnimationChannelValid(data);
}

bool SceneAnimationChannel::apply(float time, scene::SceneEntityWorld& world) const
{
    scene::AnimationChannelData data;
    data.sampler = m_Sampler;
    data.targetEntity = m_TargetEntity;
    data.targetMaterial = m_TargetMaterial.lock();
    data.attribute = m_Attribute;
    data.leafPropertyName = m_LeafPropertyName;
    return scene::applyAnimationChannel(data, time, world);
}

std::shared_ptr<SceneAnimation> SceneAnimation::clone()
{
    auto copy = std::make_shared<SceneAnimation>();
    copy->name = name;
    for (const auto& channel : m_Channels)
    {
        std::shared_ptr<SceneAnimationChannel> channelCopy;

        auto targetMaterial = channel->getTargetMaterial();
        if (targetMaterial)
        {
            channelCopy = std::make_shared<SceneAnimationChannel>(channel->getSampler(), targetMaterial);
        }
        else
        {
            channelCopy = std::make_shared<SceneAnimationChannel>(
                channel->getSampler(), channel->getTargetEntity(), channel->getAttribute());
        }

        channelCopy->setLeafPropertyName(channel->getLeafPropertyName());
        copy->addChannel(channelCopy);
    }
    return copy;
}

bool SceneAnimation::apply(float time, scene::SceneEntityWorld& world) const
{
    scene::AnimationComponent component;
    scene::initializeAnimationComponent(component, *this);
    return scene::applyAnimation(component, time, world);
}

void SceneAnimation::addChannel(const std::shared_ptr<SceneAnimationChannel>& channel)
{
    m_Channels.push_back(channel);
    m_Duration = std::max(m_Duration, channel->getSampler()->GetEndTime());
}

bool SceneAnimation::isVald() const
{
    scene::AnimationComponent component;
    scene::initializeAnimationComponent(component, *this);
    return scene::isAnimationValid(component);
}

} // namespace caustica
