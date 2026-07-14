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

        bool isValid() const { return m_valid; }
        [[nodiscard]] caustica::ecs::Entity getRootEntity() const { return m_rootEntity; }
        [[nodiscard]] const std::shared_ptr<caustica::scene::SceneEntityWorld>& getEntityWorld() const { return m_entityWorld; }
        const std::string& getModelName() const { return m_name; }
        const Pose& getModelPose() const { return m_modelPose; }
        caustica::Scene& getScene() const { return m_scene; }

        std::string findLightControllerInfo(const std::string& nodeName);

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

        const std::string& getInstanceName() const { return m_name; }
        const std::string& getModelName() const { return m_modelType->getModelName(); }
        [[nodiscard]] caustica::ecs::Entity getRootEntity() const { return m_entity; }

        void setTransform(const dm::double3& translation, const dm::dquat& rotation, const dm::double3& scaling);
        void setTransform(const dm::float3& translation, const dm::quat& rotation, const dm::float3& scaling);

        void updateLightFromControllers(double gameTime);

        const std::vector<std::shared_ptr<LightController>>& getLights() const { return m_lightControllers; }

    private:
        std::shared_ptr<ModelType> m_modelType;
        caustica::ecs::Entity m_entity = caustica::ecs::NullEntity;
        std::string m_name;
        std::vector<std::shared_ptr<LightController>> m_lightControllers;

        void mapLightControllers(caustica::ecs::Entity entity);
    };
}
