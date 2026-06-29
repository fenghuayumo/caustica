#pragma once

#include <scene/GameTypes.h>
#include <ecs/Entity.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace caustica { class Scene; }

namespace caustica::scene { class SceneEntityWorld; }

namespace game
{
    class ModelInstance;

    class ModelType
    {
    public:
        ModelType(caustica::Scene& scene, const std::string& name, const Json::Value& node);

        bool IsValid() const { return m_valid; }
        [[nodiscard]] caustica::ecs::Entity GetRootEntity() const { return m_rootEntity; }
        [[nodiscard]] const std::shared_ptr<caustica::scene::SceneEntityWorld>& GetEntityWorld() const { return m_entityWorld; }
        const std::string& GetModelName() const { return m_name; }
        const Pose& GetModelPose() const { return m_modelPose; }
        caustica::Scene& GetScene() const { return m_scene; }

        std::string FindLightControllerInfo(const std::string& nodeName);

    private:
        caustica::Scene& m_scene;
        bool             m_valid = false;
        std::string      m_name;
        Pose             m_modelPose;
        caustica::ecs::Entity      m_rootEntity = caustica::ecs::NullEntity;
        std::shared_ptr<caustica::scene::SceneEntityWorld> m_entityWorld;
        std::map<std::string, std::string> m_lightsInfos;
    };

    class ModelInstance
    {
    public:
        ModelInstance(const std::string& name,
                      const std::shared_ptr<ModelType>& modelType,
                      caustica::ecs::Entity parentEntity);

        const std::string& GetInstanceName() const { return m_name; }
        const std::string& GetModelName() const { return m_modelType->GetModelName(); }
        [[nodiscard]] caustica::ecs::Entity GetRootEntity() const { return m_entity; }

        void SetTransform(const dm::double3& translation, const dm::dquat& rotation, const dm::double3& scaling);
        void SetTransform(const dm::float3& translation, const dm::quat& rotation, const dm::float3& scaling);

        void UpdateLightFromControllers(double gameTime);

        const std::vector<std::shared_ptr<LightController>>& GetLights() const { return m_lightControllers; }

    private:
        std::shared_ptr<ModelType> m_modelType;
        caustica::ecs::Entity m_entity = caustica::ecs::NullEntity;
        std::string m_name;
        std::vector<std::shared_ptr<LightController>> m_lightControllers;

        void MapLightControllers(caustica::ecs::Entity entity);
    };
}
