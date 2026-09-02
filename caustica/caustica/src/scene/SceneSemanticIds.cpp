#include <scene/SceneSemanticIds.h>
#include <scene/SceneEcs.h>

#include <cstdio>

namespace caustica::scene
{

uint32_t hashStableLabel(std::string_view text)
{
    uint32_t hash = 2166136261u;
    for (unsigned char ch : text)
    {
        hash ^= uint32_t(ch);
        hash *= 16777619u;
    }
    return hash == 0u ? 1u : hash;
}

uint32_t resolveInstanceId(const ecs::World& world, ecs::Entity entity)
{
    if (!ecs::isValid(entity) || !world.isAlive(entity))
        return 0u;

    if (const auto* label = world.tryGet<SemanticLabelComponent>(entity))
    {
        if (label->instanceId != 0u)
            return label->instanceId;
    }

    if (const auto* authoring = world.tryGet<SceneAuthoringIdComponent>(entity))
    {
        if (!authoring->id.empty())
            return hashStableLabel(authoring->id);
    }

    if (const auto* path = world.tryGet<PathComponent>(entity))
    {
        const std::string pathText = path->value.generic_string();
        if (!pathText.empty())
            return hashStableLabel(pathText);
    }

    if (const auto* name = world.tryGet<NameComponent>(entity))
    {
        if (!name->value.empty())
            return hashStableLabel(name->value);
    }

    char fallback[32];
    std::snprintf(fallback, sizeof(fallback), "entity:%u", static_cast<uint32_t>(entity));
    return hashStableLabel(fallback);
}

uint32_t resolveSemanticId(const ecs::World& world, ecs::Entity entity)
{
    if (!ecs::isValid(entity) || !world.isAlive(entity))
        return 0u;

    const auto* label = world.tryGet<SemanticLabelComponent>(entity);
    if (!label)
        return 0u;
    if (label->semanticId != 0u)
        return label->semanticId;
    if (!label->semanticLabel.empty())
        return hashStableLabel(label->semanticLabel);
    return 0u;
}

std::string resolveSemanticLabel(const ecs::World& world, ecs::Entity entity)
{
    if (!ecs::isValid(entity) || !world.isAlive(entity))
        return {};
    if (const auto* label = world.tryGet<SemanticLabelComponent>(entity))
        return label->semanticLabel;
    return {};
}

bool setSemanticLabel(
    ecs::World& world,
    ecs::Entity entity,
    uint32_t instanceId,
    uint32_t semanticId,
    std::string semanticLabel)
{
    if (!ecs::isValid(entity) || !world.isAlive(entity))
        return false;

    if (auto* existing = world.tryGet<SemanticLabelComponent>(entity))
    {
        existing->instanceId = instanceId;
        existing->semanticId = semanticId;
        existing->semanticLabel = std::move(semanticLabel);
        world.notifyComponentChanged<SemanticLabelComponent>(entity);
        return true;
    }

    SemanticLabelComponent component;
    component.instanceId = instanceId;
    component.semanticId = semanticId;
    component.semanticLabel = std::move(semanticLabel);
    world.emplace<SemanticLabelComponent>(entity, std::move(component));
    return true;
}

} // namespace caustica::scene
