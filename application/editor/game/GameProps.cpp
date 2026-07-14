#include "GameScene.h"
#include "GameProps.h"
#include "GamePropComponent.h"

#include <core/log.h>
#include <core/json.h>
#include <math/math.h>
#include <scene/camera/Camera.h>
#include <scene/SceneEcs.h>
#include <scene/Scene.h>
#include <cmath>

#include <core/command_line.h>

#include <render/passes/debug/Korgi.h>
#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/core/ScopedPerfMarker.h>
#include <render/core/TextureUtils.h>
#include <json/json.h>

#include <fstream>
#include <iostream>
#include <thread>

using namespace caustica::math;
using namespace caustica;
using namespace caustica;
using namespace caustica;
using namespace caustica::render;

using namespace game;

PropBase::PropBase(GameScene& gameScene, const std::string & name)
    : m_gameScene(gameScene) 
{
    auto* ew = EntityWorld();
    if (ew)
        m_entity = ew->createEntity(name, ew->root());
}

caustica::scene::SceneEntityWorld* PropBase::EntityWorld() const
{
    const auto& scene = m_gameScene.scene();
    return scene ? scene->getEntityWorld() : nullptr;
}

std::string PropBase::GetName() const
{
    auto* ew = EntityWorld();
    if (!ew || m_entity == caustica::ecs::NullEntity) return {};
    return ew->getEntityName(m_entity);
}

std::shared_ptr<ModelInstance> PropBase::CreateAndAttachModel(const std::shared_ptr<game::ModelType> & modelType, const std::string & instanceName, const dm::float3& translation, const dm::quat& rotation, const dm::float3& scaling )
{
    auto ret = std::make_shared<game::ModelInstance>(instanceName, modelType, m_entity);
    ret->setTransform(translation, rotation, scaling);
    return ret;
}

void PropBase::setTransform(const dm::double3& translation, const dm::dquat& rotation, const dm::double3& scaling)
{
    auto* ew = EntityWorld();
    if (ew && m_entity != caustica::ecs::NullEntity)
        ew->setLocalTransform(m_entity, &translation, &rotation, &scaling);
}

void PropBase::setTransform(const dm::float3& translation, const dm::quat& rotation, const dm::float3& scaling)
{
    dm::double3 transD = dm::double3(translation);
    dm::dquat rotD = dm::dquat(rotation);
    dm::double3 scalD = dm::double3(scaling);
    auto* ew = EntityWorld();
    if (ew && m_entity != caustica::ecs::NullEntity)
        ew->setLocalTransform(m_entity, &transD, &rotD, &scalD);
}

template<typename T>
quaternion<T> fixedRotationQuat(const vector<T, 3>& axis, T radians)
{
    // Note: assumes axis is normalized
    T sinHalfTheta = std::sin(T(0.5) * radians);
    T cosHalfTheta = std::cos(T(0.5) * radians);
    const vector<T, 3>& axisLocal = axis * sinHalfTheta;

    return quaternion<T>(cosHalfTheta, axisLocal.x, axisLocal.y, axisLocal.z);
}

void PropBase::Tick(double gameTime, float deltaTime)
{
#if 1
    if (m_allowKeyMoveIfSelected && m_gameScene.GetSelectedProp().get() == this) // yuck?
    {
        float forward = 0.0f;
        float right = 0.0f;
        float up = 0.0f;
        float rotateAroundForward = 0.0;
        float rotateAroundRight = 0.0;
        float rotateAroundUp = 0.0;

        auto IsDown = [&](int key) { return glfwGetKey( m_gameScene.GetGLFWWindow(), key ) == GLFW_PRESS; };

        float moveSpeed = deltaTime;
        if (IsDown(GLFW_KEY_LEFT_CONTROL) || IsDown(GLFW_KEY_RIGHT_CONTROL))
            moveSpeed *= 0.1f;
        if (IsDown(GLFW_KEY_LEFT_SHIFT) || IsDown(GLFW_KEY_RIGHT_SHIFT))
            moveSpeed *= 10.0f;
        float rotateSpeed = moveSpeed;

        bool rotateInsteadOfMove = IsDown(GLFW_KEY_RIGHT_ALT);
        if (!rotateInsteadOfMove)
        {
            if (IsDown(GLFW_KEY_KP_8))  forward += moveSpeed;
            if (IsDown(GLFW_KEY_KP_2))  forward -= moveSpeed;
            if (IsDown(GLFW_KEY_KP_4))  right -= moveSpeed;
            if (IsDown(GLFW_KEY_KP_6))  right += moveSpeed;
            if (IsDown(GLFW_KEY_KP_9))  up += moveSpeed;
            if (IsDown(GLFW_KEY_KP_3))  up -= moveSpeed;
        }
        else
        {
            if (IsDown(GLFW_KEY_KP_8))  rotateAroundRight += rotateSpeed;
            if (IsDown(GLFW_KEY_KP_2))  rotateAroundRight -= rotateSpeed;
            if (IsDown(GLFW_KEY_KP_4))  rotateAroundUp += rotateSpeed;
            if (IsDown(GLFW_KEY_KP_6))  rotateAroundUp -= rotateSpeed;
            if (IsDown(GLFW_KEY_KP_7))  rotateAroundForward -= rotateSpeed;
            if (IsDown(GLFW_KEY_KP_9))  rotateAroundForward += rotateSpeed;
        }

        if (forward != 0 || right != 0 || up != 0)
        {
            auto* ew = EntityWorld();
            if (ew && m_entity != caustica::ecs::NullEntity)
            {
                auto* ltc = ew->world().tryGet<caustica::scene::LocalTransformComponent>(m_entity);
                double3 trans = ltc ? ltc->translation : double3(0.0);
                
#if 1 // use rotation 
                dquat entityRot = ltc ? ltc->rotation : dquat::identity();
                auto rot = quat(entityRot).toAffine();
                float3 forwardVec   = rot.transformVector(m_referenceForward);
                float3 upVec        = rot.transformVector(m_referenceUp);
                float3 rightVec     = rot.transformVector(m_referenceRight);
#else
                float3 forwardVec   = m_referenceForward;
                float3 upVec        = m_referenceUp;
                float3 rightVec     = m_referenceRight;
#endif

                trans += double3( forwardVec * forward + upVec * up + rightVec * right );
                ew->setTranslation(m_entity, trans);
            }
        }
        if (rotateAroundForward != 0 || rotateAroundRight != 0 || rotateAroundUp != 0)
        {
            auto* ew = EntityWorld();
            if (ew && m_entity != caustica::ecs::NullEntity)
            {
                auto* ltc = ew->world().tryGet<caustica::scene::LocalTransformComponent>(m_entity);
                dquat rot = ltc ? ltc->rotation : dquat::identity();
                rot *= dquat( fixedRotationQuat<float>(m_referenceForward, rotateAroundForward) * fixedRotationQuat<float>(m_referenceUp, rotateAroundUp) * fixedRotationQuat<float>(m_referenceRight, rotateAroundRight) );
                ew->setRotation(m_entity, rot);
            }
        }
    }
#endif

    // if (!m_gameScene.isActive())
    //     return;

    Pose animPose;
    float animTime = 0.0f;
    if (m_animation.getAt(m_animPlaybackSpeed*gameTime + m_animOffset, true, animPose, animTime))
        setTransform(animPose.Translation, animPose.Rotation, animPose.Scaling);

    for (auto & comp : m_components)
        comp->Tick(gameTime, animTime, deltaTime);

    // transfer all changes to lights - especially necessary if scaling used
    for (auto& model : m_models)
        model->updateLightFromControllers(gameTime);
}

void PropBase::reset()
{
    setTransform(m_startPose.Translation, m_startPose.Rotation, m_startPose.Scaling);
    m_animOffset = 0;
}

void PropBase::load(const Json::Value& jsonRoot)
{
    jsonRoot["propType"] >> m_propType;

    m_startPose.read(jsonRoot["startPose"]);

    auto jsonDefaultCameraPose = jsonRoot["defaultCameraPose"];
    if (jsonDefaultCameraPose.empty())
        m_defaultCameraPose.setTransformFromCamera( {0,0,0}, {1, 0, 0}, {0, 1, 0} );
    else
        m_defaultCameraPose.read(jsonDefaultCameraPose);

    jsonRoot["animPlaybackSpeed"] >> m_animPlaybackSpeed;

    jsonRoot["showOnlyIfTagged"] >> m_showOnlyIfTagged;

    Json::Value animationRecording = jsonRoot["animation"];

    if (!animationRecording.empty() && animationRecording.isArray())
    {
        m_animation.read(animationRecording);
        m_animating = true;
    }

    Json::Value modelsLightsOverrides = jsonRoot["modelsLightsOverrides"];
    if (!modelsLightsOverrides.empty())
        m_modelsLightsOverrides = caustica::json::ToString(modelsLightsOverrides);

    Json::Value components = jsonRoot["components"];
    if (!components.empty())
        m_componentsData = caustica::json::ToString(components);
}

void PropBase::PostLoadSetup()
{
    if (m_modelsLightsOverrides != "")
    {
        Json::Value modelsLightsOverrides;
        caustica::json::FromString(m_modelsLightsOverrides, modelsLightsOverrides);
        for( Json::Value & m : modelsLightsOverrides )
        {
            std::string modelName; m["modelInstanceName"] >> modelName;
            
            auto it = std::find_if(m_models.begin(), m_models.end(), [&modelName](auto & model) { return model->getInstanceName() == modelName; });
            if (it != m_models.end())
            {
                const std::vector<std::shared_ptr<LightController>> & modelLights = (*it)->getLights();
                Json::Value lightOverrides = m["lightOverrides"];
                for (Json::Value& lo : lightOverrides)
                {
                    std::string lightName; lo["name"] >> lightName;

                    auto* ew = EntityWorld();
                    auto lit = std::find_if(modelLights.begin(), modelLights.end(), [&lightName, ew](auto& light) {
                        return ew && light->Entity != caustica::ecs::NullEntity
                            && ew->getEntityName(light->Entity) == lightName;
                    });
                    if (lit != modelLights.end())
                    {
                        (*lit)->read(lo);
                    }
                    else { caustica::warning("Bad light override, prop %s, model %s, light name %s", GetName().c_str(), modelName.c_str(), lightName.c_str()); }
                }
            }
            else { caustica::warning("Bad light override, prop %s, model %s", GetName().c_str(), modelName.c_str()); }
        }
    }
    if (m_componentsData != "")
    {
        Json::Value components;
        caustica::json::FromString(m_componentsData, components);
        for (Json::Value& m : components)
        {
            std::shared_ptr<PropComponentBase> comp = PropComponentBase::create(*this, m);
            if (comp != nullptr)
                m_components.push_back(comp);
        }
    }
}

Json::Value PropBase::Save()
{
    Json::Value jsonRoot;
    if (m_animation.Keys.size() > 0)
        jsonRoot["animation"] = m_animation.write();

    jsonRoot["defaultCameraPose"] = m_defaultCameraPose.write();
    jsonRoot["startPose"] = m_startPose.write();
    jsonRoot["propType"] = m_propType;
    jsonRoot["animPlaybackSpeed"] = m_animPlaybackSpeed;
    
    if (m_modelsLightsOverrides!="")
    {
        Json::Value outRootNode;
        if (caustica::json::FromString(m_modelsLightsOverrides, outRootNode))
            jsonRoot["modelsLightsOverrides"] = outRootNode;
    }
    if (m_componentsData != "")
    {
        Json::Value outRootNode;
        if (caustica::json::FromString(m_componentsData, outRootNode))
            jsonRoot["components"] = outRootNode;
    }
    return jsonRoot;
}

void PropBase::GUI(float indent, bool & gameCameraAttached, caustica::FirstPersonCamera & gameCamera)
{
    RAII_SCOPE(ImGui::Indent(indent); , ImGui::Unindent(indent); );
    ImGui::Text("Properties for %s", GetName().c_str());

    if (gameCameraAttached)
    {
        RAII_SCOPE(ImGui::PushID("CamPose");, ImGui::PopID(););
        ImGui::Text("Camera ATTACHED!"); ImGui::SameLine(); 
        if (ImGui::Button("Detach")) 
            gameCameraAttached = false;
#ifdef SAMPLE_GAME_DEVELOPER_SETTINGS
        ImGui::Text("Camera pose: "); ImGui::SameLine();
        if (ImGui::Button("Save"))
        {
            m_defaultCameraPose.setTransformFromCamera(gameCamera.getPosition(), gameCamera.getDir(), gameCamera.getUp());
        }
        ImGui::SameLine();
        if (ImGui::Button("load"))
        {
            auto [pos, dir, up] = m_defaultCameraPose.getPosDirUp();
            gameCamera.lookTo( pos, dir, up );
        }
#endif
    }
    else
    {
        ImGui::Text("Camera not attached"); ImGui::SameLine();
        if (ImGui::Button("Attach"))
            gameCameraAttached = true;
#ifdef SAMPLE_GAME_DEVELOPER_SETTINGS
        if (ImGui::Button("Move prop to camera pose"))
        {
            auto& pose = m_gameScene.GetLastRenderCameraPose();
            setTransform(pose.Translation, pose.Rotation, pose.Scaling);
        }
#endif
    }
#ifdef SAMPLE_GAME_DEVELOPER_SETTINGS
    if (ImGui::Button("Save current as start pose"))
    {
        auto* ew = EntityWorld();
        if (ew && m_entity != caustica::ecs::NullEntity)
        {
            auto* ltc = ew->world().tryGet<caustica::scene::LocalTransformComponent>(m_entity);
            if (ltc)
            {
                m_startPose.Translation = ltc->translation;
                m_startPose.Rotation = ltc->rotation;
                m_startPose.Scaling = ltc->scaling;
            }
        }
    }
    if (m_gameScene.GetCamRecAnimation().size() > 0 && ImGui::Button("Copy current cam animation"))
    {
        m_animation.Keys.clear();
        m_animation.fromKeys(m_gameScene.GetCamRecAnimation());
    }
    ImGui::Separator();
    {
        RAII_SCOPE(ImGui::PushID("Storage"); , ImGui::PopID(););
        //ImGui::Text("Storage: '%s'", m_storagePath.string().c_str());
        ImGui::Text("Storage: ");
        ImGui::SameLine();
        if (ImGui::Button("Save"))
            caustica::json::SaveToFile(m_storagePath, Save());

        if (ImGui::CollapsingHeader("Lights"))
        {
            int counter = 0;
            for (const auto & model : m_models)
                for (const auto & light : model->getLights())
                {
                    counter++; RAII_SCOPE(ImGui::PushID(counter);, ImGui::PopID(););
                    auto* ew = EntityWorld();
                    std::string lightName = (ew && light->Entity != caustica::ecs::NullEntity) ? ew->getEntityName(light->Entity) : "?";
                    ImGui::Checkbox( std::format("Light {} - {}", model->getInstanceName().c_str(), lightName.c_str()).c_str(), &light->enabled );
                }
        }
        // ImGui::SameLine();
        // if (ImGui::Button("load"))
        // {
        //     Json::Value jread;
        //     caustica::json::loadFromFile( m_storagePath, jread );
        //     load(jread);
        //     reset();
        // }
    }
#endif
}

ScreenGUISel PropBase::StandaloneGUI(const std::shared_ptr<caustica::PlanarView> & view, const float2 & mousePos, const float2 & displaySize)
{
    ScreenGUISel sel{};
    for (auto& comp : m_components)
    {
        ScreenGUISel selC = comp->StandaloneGUI(view, mousePos, displaySize);
        if (selC.Selected && selC.RangeToCamera < sel.RangeToCamera)
            sel = selC;
    }
    return sel;
}

SimpleProp::SimpleProp(class GameScene & gameScene, const std::string & name)
    : PropBase(gameScene, name)
{
}

void SimpleProp::Tick(double gameTime, float deltaTime)
{
    PropBase::Tick(gameTime, deltaTime);
}

void SimpleProp::reset()
{
    PropBase::reset();
}

void SimpleProp::load(const Json::Value& jsonRoot)
{
    PropBase::load(jsonRoot);

    if (m_showOnlyIfTagged != "")
        if (FindSubStringIgnoreCase(m_gameScene.GetCmdLine().PropShowTags, m_showOnlyIfTagged) == std::string::npos)
            return; // nothing to see here

    jsonRoot["modelName"] >> m_modelName;
    auto modelType = m_gameScene.FindModelType(m_modelName);
    if (modelType == nullptr)
        { assert( false ); return; }
    m_model = CreateAndAttachModel(modelType, "SimplePropOnlyModel", float3(0,0,0) );
    m_models.push_back(m_model);
}

Json::Value SimpleProp::Save()
{
    Json::Value ret = PropBase::Save();

    ret["modelName"] << m_modelName;
    return ret;
}
