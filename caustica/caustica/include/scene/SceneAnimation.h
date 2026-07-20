#pragma once

#include <scene/SceneObjects.h>
#include <scene/SceneContent.h>
#include <animation/KeyframeAnimation.h>
#include <ecs/Entity.h>
#include <memory>
#include <string>
#include <vector>

namespace caustica::scene { class SceneEntityWorld; }

namespace caustica
{
    enum class AnimationAttribute : uint32_t
    {
        Undefined,
        Scaling,
        Rotation,
        Translation,
        LeafProperty,
        // MeshInstance / GaussianSplat visibility (Step; value.x > 0.5 => visible).
        Visibility
    };

    class SceneAnimationChannel
    {
    private:
        std::shared_ptr<animation::Sampler> m_Sampler;
        ecs::Entity m_TargetEntity = ecs::NullEntity;
        std::weak_ptr<Material> m_TargetMaterial;
        AnimationAttribute m_Attribute;
        std::string m_LeafPropertyName;

    public:
        // Constructor for transform-targeting channels.
        SceneAnimationChannel(std::shared_ptr<animation::Sampler> sampler, ecs::Entity targetEntity, AnimationAttribute attribute)
            : m_Sampler(std::move(sampler))
            , m_TargetEntity(targetEntity)
            , m_Attribute(attribute)
        { }

        // Constructor for material property channels.
        SceneAnimationChannel(std::shared_ptr<animation::Sampler> sampler, const std::shared_ptr<Material>& targetMaterial)
            : m_Sampler(std::move(sampler))
            , m_TargetMaterial(targetMaterial)
            , m_Attribute(AnimationAttribute::LeafProperty)
        { }

        [[nodiscard]] bool isValid() const;
        [[nodiscard]] const std::shared_ptr<animation::Sampler>& getSampler() const { return m_Sampler; }
        [[nodiscard]] AnimationAttribute getAttribute() const { return m_Attribute; }
        [[nodiscard]] ecs::Entity getTargetEntity() const { return m_TargetEntity; }
        [[nodiscard]] std::shared_ptr<Material> getTargetMaterial() const { return m_TargetMaterial.lock(); }
        [[nodiscard]] const std::string& getLeafPropertyName() const { return m_LeafPropertyName; }
        void setTargetEntity(ecs::Entity entity) { m_TargetEntity = entity; }
        void setLeafPropertyName(const std::string& name) { m_LeafPropertyName = name; }

        // apply the sampled value for `time` to the target entity/material via `world`.
        // Returns false if the channel has no valid target or the sampler has no data at `time`.
        bool apply(float time, scene::SceneEntityWorld& world) const;  // NOLINT(modernize-use-nodiscard)
    };

    class SceneAnimation
    {
    private:
        std::vector<std::shared_ptr<SceneAnimationChannel>> m_Channels;
        float m_Duration = 0.f;

    public:
        std::string name;

        SceneAnimation() = default;

        [[nodiscard]] std::shared_ptr<SceneAnimation> clone();
        [[nodiscard]] SceneContentFlags getContentFlags() const { return SceneContentFlags::Animations; }
        [[nodiscard]] const std::vector<std::shared_ptr<SceneAnimationChannel>>& getChannels() const { return m_Channels; }
        [[nodiscard]] float getDuration() const { return m_Duration; }
        [[nodiscard]] bool isVald() const;  // note: preserves original typo
        bool apply(float time, scene::SceneEntityWorld& world) const;  // NOLINT(modernize-use-nodiscard)
        void addChannel(const std::shared_ptr<SceneAnimationChannel>& channel);
    };

} // namespace caustica
