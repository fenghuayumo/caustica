#include <scene/SceneAnimation.h>
#include <scene/SceneAnimationAccess.h>

namespace caustica
{
namespace
{

scene::AnimationComponent makeAnimationComponent(const SceneAnimation& animation)
{
    scene::AnimationComponent component;
    component.duration = animation.getDuration();
    component.channels.reserve(animation.getChannels().size());
    for (const auto& channel : animation.getChannels())
    {
        scene::AnimationChannelData data;
        data.sampler = channel->getSampler();
        data.targetEntity = channel->getTargetEntity();
        data.targetMaterial = channel->getTargetMaterial();
        data.attribute = channel->getAttribute();
        data.leafPropertyName = channel->getLeafPropertyName();
        component.channels.push_back(std::move(data));
    }
    return component;
}

} // namespace

bool SceneAnimationChannel::isValid() const
{
    scene::AnimationChannelData data;
    data.sampler = m_sampler;
    data.targetEntity = m_targetEntity;
    data.targetMaterial = m_targetMaterial.lock();
    data.attribute = m_attribute;
    data.leafPropertyName = m_leafPropertyName;
    return scene::isAnimationChannelValid(data);
}

bool SceneAnimationChannel::apply(float time, scene::SceneEntityWorld& world) const
{
    scene::AnimationChannelData data;
    data.sampler = m_sampler;
    data.targetEntity = m_targetEntity;
    data.targetMaterial = m_targetMaterial.lock();
    data.attribute = m_attribute;
    data.leafPropertyName = m_leafPropertyName;
    return scene::applyAnimationChannel(data, time, world);
}

std::shared_ptr<SceneAnimation> SceneAnimation::clone()
{
    auto copy = std::make_shared<SceneAnimation>();
    copy->name = name;
    for (const auto& channel : m_channels)
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
    scene::AnimationComponent component = makeAnimationComponent(*this);
    return scene::applyAnimation(component, time, world);
}

void SceneAnimation::addChannel(const std::shared_ptr<SceneAnimationChannel>& channel)
{
    m_channels.push_back(channel);
    m_duration = std::max(m_duration, channel->getSampler()->getEndTime());
}

bool SceneAnimation::isVald() const
{
    return scene::isAnimationValid(makeAnimationComponent(*this));
}

} // namespace caustica
