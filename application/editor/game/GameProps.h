#pragma once

#include <shaders/PathTracer/Config.h>
#include <EditorUI.h>

#include <scene/camera/Camera.h>
#include "GameModel.h"
#include <ecs/Entity.h>

namespace demo
{
    class PropComponentBase;

    // DEMO-ONLY editor game OO prop -- not an engine ECS component.
    class PropBase
    {
    public:
        PropBase(class GameScene & gameScene, const std::string & name);
        virtual ~PropBase() { }

        virtual void            Tick(double gameTime, float deltaTime);
        virtual void            reset();
        virtual void            load(const Json::Value& jsonRoot);
        virtual void            PostLoadSetup();
        virtual Json::Value     Save();

        virtual void            setTransform(const math::double3& translation, const math::dquat& rotation, const math::double3& scaling);
        virtual void            setTransform(const math::float3& translation, const math::quat& rotation, const math::float3& scaling);

        const Pose &            GetDefaultCameraPose() const            { return m_defaultCameraPose; }

        void                    SetStoragePath(const std::filesystem::path& path) { m_storagePath = path; }
        void                    SetAnimOffset(double animOffset)        { m_animOffset = animOffset; }
        std::string             getName() const;
        caustica::ecs::Entity   GetEntity() const                       { return m_entity; }
        const std::vector<std::shared_ptr<ModelInstance>> &
                                getModels() const                       { return m_models; }

        virtual void            GUI(float indent, bool & gameCameraAttached, caustica::FirstPersonCamera & gameCamera);
        virtual ScreenGUISel    StandaloneGUI(const std::shared_ptr<caustica::ViewInfo> & view, const float2 & mousePos, const float2 & displaySize);

        caustica::scene::SceneEntityWorld* EntityWorld() const;

    protected:
        std::shared_ptr<ModelInstance> CreateAndAttachModel( const std::shared_ptr<demo::ModelType> & modelType, const std::string & instanceName, const math::float3& translation, const math::quat& rotation = math::quat::identity(), const math::float3& scaling = math::float3(1,1,1) );
        
    protected:
        class GameScene & m_gameScene;
        std::string                 m_propType;

        Pose                        m_startPose;
        
        Pose                        m_defaultCameraPose;

        float3                      m_referenceForward  = math::float3(1.f, 0.f, 0.f);
        float3                      m_referenceUp       = math::float3(0.f, 1.f, 0.f);
        float3                      m_referenceRight    = math::float3(0.f, 0.f, 1.f);

        std::filesystem::path       m_storagePath;

        KeyframeAnimation           m_animation;
        float                       m_animPlaybackSpeed = 1.0f;
        double                      m_animOffset    = 0.0;
        bool                        m_animating     = false;
        std::string                 m_showOnlyIfTagged = "";

        bool                        m_allowKeyMoveIfSelected = true;

        caustica::ecs::Entity       m_entity = caustica::ecs::NullEntity;

        std::vector<std::shared_ptr<ModelInstance>>
                                    m_models;
        std::vector<std::shared_ptr<PropComponentBase>>
                                    m_components;

        std::string                 m_modelsLightsOverrides;

        std::string                 m_componentsData;
    };

    // Single model prop
    class SimpleProp : public PropBase
    {
    public:
        SimpleProp(class GameScene & gameScene, const std::string & name);
        virtual ~SimpleProp() {}

    protected:
        virtual void            Tick(double gameTime, float deltaTime) override;
        virtual void            reset() override;
        virtual void            load(const Json::Value& jsonRoot) override;
        virtual Json::Value     Save() override;

    protected:
        std::string             m_modelName;
        std::shared_ptr<ModelInstance>
                                m_model;
    };
}
