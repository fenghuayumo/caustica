#include <scene/GameModel.h>
#include <scene/SceneImport.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
#include <core/log.h>
#include <core/json.h>
#include <core/format.h>
#include <math/math.h>
#include <cmath>
#include <scene/Scene.h>

using namespace caustica::math;
using namespace caustica;
using namespace game;

namespace
{
void ForEachEntityInSubtree(scene::SceneEntityWorld& world, ecs::Entity root, const auto& fn)
{
    if (!ecs::isValid(root))
        return;
    fn(root);
    for (ecs::Entity child : world.getEntityChildren(root))
        ForEachEntityInSubtree(world, child, fn);
}
} // namespace

ModelType::ModelType(caustica::Scene& scene, const std::string& name, const Json::Value& node)
    : m_scene(scene)
{
    m_valid = false;
    m_name = name;

    int modelIndex = -1;
    std::string modelName = "";
    node["sceneModelIndex"] >> modelIndex;
    node["sceneModelName"] >> modelName;

    node["modelPose"] >> m_modelPose;

    auto lights = node["lights"];
    if (lights.isArray())
    {
        for (auto& lightData : lights)
        {
            std::string lightName;
            lightData["name"] >> lightName;
            if (lightName == "" || m_lightsInfos.find(lightName) != m_lightsInfos.end())
            {
                assert(false && "malformed or repeated light name in .model.json");
                continue;
            }
            m_lightsInfos.insert({ lightName, caustica::json::toString(lightData) });
        }
    }

    const std::vector<caustica::SceneImportResult>& models = m_scene.getModels();

    if (modelIndex == -1 && modelName != "")
    {
        for (int i = 0; i < int(models.size()); i++)
        {
            if (!models[i].entityWorld || !ecs::isValid(models[i].rootEntity))
                continue;
            const std::string loadedName = models[i].entityWorld->getEntityName(models[i].rootEntity);
            if (findSubStringIgnoreCase(loadedName, modelName) != std::string::npos)
            {
                modelIndex = i;
                break;
            }
        }
    }

    if (modelIndex < 0 || modelIndex >= int(models.size()))
    {
        caustica::warning("Referenced model %d is not defined in the model array.", modelIndex);
        return;
    }

    const auto& loadedModel = models[modelIndex];
    if (!loadedModel.entityWorld || !ecs::isValid(loadedModel.rootEntity))
    {
        assert(false);
        return;
    }

    m_entityWorld = loadedModel.entityWorld;
    m_rootEntity = loadedModel.rootEntity;
    m_valid = m_name != "" && modelIndex != -1;
}

std::string ModelType::findLightControllerInfo(const std::string& nodeName)
{
    auto it = m_lightsInfos.find(nodeName);
    if (it == m_lightsInfos.end())
        return "";
    return it->second;
}

ModelInstance::ModelInstance(const std::string& name,
                             const std::shared_ptr<ModelType>& modelType,
                             ecs::Entity parentEntity)
    : m_modelType(modelType)
    , m_name(name)
{
    assert(modelType != nullptr);
    caustica::Scene& scene = modelType->getScene();
    auto* world = scene.getEntityWorld();
    if (!world)
        return;

    m_entity = world->createEntity(name, parentEntity);

    if (modelType->getEntityWorld() && ecs::isValid(modelType->getRootEntity()))
    {
        ecs::Entity importedRoot = world->importSubtree(
            m_entity,
            *modelType->getEntityWorld(),
            modelType->getRootEntity(),
            scene.getSceneTypeFactory().get());
        if (ecs::isValid(importedRoot))
        {
            const Pose& pose = modelType->getModelPose();
            world->setLocalTransform(importedRoot, &pose.Translation, &pose.Rotation, &pose.Scaling);
        }
    }

    mapLightControllers(m_entity);
}

void ModelInstance::mapLightControllers(ecs::Entity entity)
{
    auto* world = m_modelType->getScene().getEntityWorld();
    if (!world)
        return;

    auto& ecsWorld = world->world();

    ForEachEntityInSubtree(*world, entity, [&](ecs::Entity current) {
        if (!scene::hasAnyLightComponent(ecsWorld, current))
            return;

            const std::string entityName = world->getEntityName(current);
            std::string data = m_modelType->findLightControllerInfo(entityName);
            Json::Value jsonData;
            if (data != "" && caustica::json::fromString(data, jsonData))
            {
                auto lightController = std::make_shared<LightController>();
                if (!lightController->read(jsonData))
                {
                    assert(false && "Error reading LightController data");
                }
                else
                {
                    lightController->Entity = current;
                    m_lightControllers.push_back(lightController);
                }
            }
            else
            {
                caustica::warning("Model instance '%s', light '%s' has no controller",
                    m_name.c_str(), entityName.c_str());
            }
    });
}

void ModelInstance::updateLightFromControllers(double gameTime)
{
    auto* world = m_modelType->getScene().getEntityWorld();
    if (!world)
        return;

    auto& ecsWorld = world->world();

    for (const auto& controller : m_lightControllers)
    {
        if (!ecs::isValid(controller->Entity))
            continue;

        const auto* global = ecsWorld.get<scene::GlobalTransformComponent>(controller->Entity);
        if (!global)
            continue;

        float3 scale;
        dm::decomposeAffine<float>(global->transformFloat, nullptr, nullptr, &scale);
        float reallyAverageScale = (scale.x + scale.y + scale.z) / 3.0f;

        auto* spot = scene::tryGetSpotLight(ecsWorld, controller->Entity);
        auto* point = scene::tryGetPointLight(ecsWorld, controller->Entity);
        if (!spot && !point)
            continue;

        if (spot)
            spot->color = controller->Color;
        if (point)
            point->color = controller->Color;

        bool enabled = controller->enabled;

        if (controller->AutoOffTime != 0 && controller->AutoOnTime != 0)
        {
            double periodLength = controller->AutoOffTime + controller->AutoOnTime;
            double scaledTime = (gameTime + controller->AutoOnOffTimeOffset) / periodLength;
            double remainder = dm::saturate<double>(scaledTime - floor(scaledTime));
            enabled &= remainder < (controller->AutoOffTime / periodLength);
        }

        float intensity = enabled ? controller->Intensity : 0.0f;

        if (spot != nullptr)
        {
            spot->radius = reallyAverageScale;
            spot->intensity = intensity;
            spot->outerAngle = abs(controller->OuterAngle);
            spot->innerAngle = controller->InnerAngle;

            const bool kUseMinSpotlightFalloff = true;
            if (kUseMinSpotlightFalloff)
                spot->outerAngle = -spot->outerAngle;
        }
        if (point != nullptr)
        {
            point->radius = reallyAverageScale;
            point->intensity = intensity;
        }
    }
}

void ModelInstance::setTransform(const dm::double3& translation, const dm::dquat& rotation, const dm::double3& scaling)
{
    if (auto* world = m_modelType->getScene().getEntityWorld())
        world->setLocalTransform(m_entity, &translation, &rotation, &scaling);
}

void ModelInstance::setTransform(const dm::float3& translation, const dm::quat& rotation, const dm::float3& scaling)
{
    setTransform(dm::double3(translation), dm::dquat(rotation), dm::double3(scaling));
}
