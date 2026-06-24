#pragma once

#include <shaders/PathTracer/Config.h>
//#include <SampleCommon/SampleCommon.h>
//#include <SampleUI.h>
#include "GameMisc.h"

#include <scene/camera/Camera.h>

#include <map>

namespace caustica
{
    class FirstPersonCamera;
}

class GameScene;

namespace game
{
    class ModelInstance;

    class ModelType
    {
    public:
        ModelType(GameScene & game, const std::string & name, const Json::Value & node);

        bool                                                    IsValid() const         { return m_valid; }
        const std::shared_ptr<caustica::SceneGraphNode> &  GetNode() const         { return m_node; }
        const std::string &                                     GetModelName() const    { return m_name; }
        const Pose &                                            GetModelPose() const    { return m_modelPose; }
        GameScene &                                             GetGame() const         { return m_game; }

        std::string                                             FindLightControllerInfo( const std::string & nodeName );

    private:
        GameScene &     m_game;
        bool            m_valid = false;
        std::string     m_name;
        //int             m_modelIndex = -1;
        Pose            m_modelPose;
        std::shared_ptr<caustica::SceneGraphNode> m_node;

        std::map<std::string, std::string>
                        m_lightsInfos;
    };

    class ModelInstance
    {
    public:
        ModelInstance( const std::string & name, const std::shared_ptr<ModelType> & modelType, const std::shared_ptr<caustica::SceneGraphNode> & parentNode);

        const std::string &     GetInstanceName() const { return m_node->GetName(); }
        const std::string &     GetModelName() const { return m_modelType->GetModelName(); }
        const std::shared_ptr<caustica::SceneGraphNode> &
                                GetRootNode() const { return m_node; }

        void                    SetTransform(const dm::double3 & translation, const dm::dquat& rotation, const dm::double3 & scaling);
        void                    SetTransform(const dm::float3 & translation, const dm::quat & rotation, const dm::float3 & scaling);

        void                    UpdateLightFromControllers(double gameTime);

        const std::vector<std::shared_ptr<LightController>> & 
                                GetLights() const { return m_lightControllers; }

    private:
        std::shared_ptr<ModelType>      m_modelType;
        std::shared_ptr<caustica::SceneGraphNode> m_node;  // weak_ptr?
        std::vector<std::shared_ptr<LightController>>   m_lightControllers;

    private:
        void                    MapLightControllers( caustica::SceneGraphNode * node );
    };

}
