#pragma once

// =============================================================================
// GameModel.h — Engine-level model instance management.
//
// Moved from editor/SampleGame/GameModel.h.  ModelInstance is pure engine.
// ModelType previously depended on GameScene& but only used GetScene() —
// now stores caustica::Scene& directly.
// =============================================================================

#include <scene/game/GameTypes.h>
#include <scene/SceneGraph.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace caustica { class Scene; }

namespace game
{
    class ModelInstance;

    class ModelType
    {
    public:
        ModelType(caustica::Scene& scene, const std::string& name, const Json::Value& node);

        bool IsValid() const { return m_valid; }
        const std::shared_ptr<caustica::SceneGraphNode>& GetNode() const { return m_node; }
        const std::string& GetModelName() const { return m_name; }
        const Pose& GetModelPose() const { return m_modelPose; }
        caustica::Scene& GetScene() const { return m_scene; }

        std::string FindLightControllerInfo(const std::string& nodeName);

    private:
        caustica::Scene& m_scene;
        bool             m_valid = false;
        std::string      m_name;
        Pose             m_modelPose;
        std::shared_ptr<caustica::SceneGraphNode> m_node;
        std::map<std::string, std::string> m_lightsInfos;
    };

    class ModelInstance
    {
    public:
        ModelInstance(const std::string& name,
                      const std::shared_ptr<ModelType>& modelType,
                      const std::shared_ptr<caustica::SceneGraphNode>& parentNode);

        const std::string& GetInstanceName() const { return m_node->GetName(); }
        const std::string& GetModelName() const { return m_modelType->GetModelName(); }
        const std::shared_ptr<caustica::SceneGraphNode>& GetRootNode() const { return m_node; }

        void SetTransform(const dm::double3& translation, const dm::dquat& rotation, const dm::double3& scaling);
        void SetTransform(const dm::float3& translation, const dm::quat& rotation, const dm::float3& scaling);

        void UpdateLightFromControllers(double gameTime);

        const std::vector<std::shared_ptr<LightController>>& GetLights() const { return m_lightControllers; }

    private:
        std::shared_ptr<ModelType> m_modelType;
        std::shared_ptr<caustica::SceneGraphNode> m_node;
        std::vector<std::shared_ptr<LightController>> m_lightControllers;

        void MapLightControllers(caustica::SceneGraphNode* node);
    };
}
