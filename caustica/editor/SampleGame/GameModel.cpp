#include "GameScene.h"
#include "GameModel.h"

#include <core/log.h>
#include <core/json.h>
#include <math/math.h>
#include <scene/camera/Camera.h>
#include <cmath>

#include <scene/Scene.h>

#include <render/Passes/Debug/Korgi.h>
#include <SampleCommon/SampleCommon.h>

#include <fstream>
#include <iostream>
#include <thread>

using namespace caustica::math;
using namespace caustica;
using namespace caustica;
using namespace caustica;
using namespace caustica::render;

using namespace game;

void SpecialFixupsRec(const std::string & modelName, SceneGraphNode* node, dm::daffine3 globalTransform)
{
    dm::daffine3 transform = dm::scaling(node->GetScaling());
    transform *= node->GetRotation().toAffine();
    transform *= dm::translation(node->GetTranslation());
    globalTransform = transform * globalTransform;

    double3 scale;
    dm::decomposeAffine<double>(globalTransform, nullptr, nullptr, &scale);
    float reallyAverageScale = float((scale.x + scale.y + scale.z) / 3.0);

//    SpotLight* spotLight = dynamic_cast<SpotLight*>(node->GetLeaf().get());
//
//    if (spotLight != nullptr)
//    {
//        //spotLight->range = 10000.0f;
//        spotLight->intensity = 50.0f;   // TODO: fix on blender side
//        spotLight->innerAngle;
//        spotLight->outerAngle;
//        spotLight->radius = reallyAverageScale;
//
//        // if (spotLight->GetName() == "SpotLeft")
//        spotLight->outerAngle = -abs(spotLight->outerAngle); // this is a special flag to indicate that, instead of falling off to 0 when angle >= outerAngle, we should clamp to kMinSpotlightFalloff
//    }
//
//    PointLight* pointLight = dynamic_cast<PointLight*>(node->GetLeaf().get());
//
//    if (pointLight != nullptr)
//    {
//        pointLight->radius = reallyAverageScale;
//    }

    // if (modelName == "RX6SpaceShip")
    // if (modelName == "OrbLight")

    for (int i = 0; i < node->GetNumChildren(); i++)
        SpecialFixupsRec(modelName, node->GetChild(i), globalTransform);
}

// For stuff where it's easier to fix in code vs json
static void SpecialFixups(const std::string & modelName, SceneGraphNode* rootNode)
{
    dm::daffine3 transform = dm::daffine3::identity();
    SpecialFixupsRec(modelName, rootNode, transform);
}

ModelType::ModelType(class GameScene & game, const std::string & name, const Json::Value & node)
    : m_game(game)
{
    m_valid = false;
    const std::shared_ptr<caustica::Scene> & scene = game.GetScene();

    // node["name"] >> m_name;
    m_name = name;

    int modelIndex = -1;
    std::string modelName = "";
    node["sceneModelIndex"] >> modelIndex;
    node["sceneModelName"] >> modelName;

    node["modelPose"] >> m_modelPose;

    auto lights = node["lights"];
    if (lights.isArray())
    {
        for( auto & lightData : lights )
        {
            std::string lightName;
            lightData["name"] >> lightName;
            if (lightName == "" || m_lightsInfos.find(lightName) != m_lightsInfos.end())
            { assert( false && "malformed or repeated light name in .model.json" );  continue; }
            m_lightsInfos.insert( {lightName, caustica::json::ToString(lightData) } );
        }
        ///m_lightsInfoJson = caustica::json::ToString(lights);
    }

    const std::vector<caustica::SceneImportResult>& models = scene->GetModels();

    if (modelIndex==-1 && modelName != "")
        for( int i = 0; i < models.size(); i++ )
            if ( FindSubStringIgnoreCase(models[i].rootNode->GetName(), modelName) != std::string::npos )
            {
                modelIndex = i;
                break;
            }

    if (modelIndex<0 || modelIndex>=models.size())
    {
        caustica::warning("Referenced model %d is not defined in the model array.", modelIndex);
        return; 
    }

    const auto& loadedModel = models[modelIndex];
    if (!loadedModel.rootNode)
    { assert( false ); return; }

    // Keep a read-only reference to the scene prototype. Never mutate or re-parent
    // this node here; props instantiate detached copies via ModelInstance.
    m_node = loadedModel.rootNode;

    m_valid = m_name != "" && modelIndex != -1;
}

std::string ModelType::FindLightControllerInfo( const std::string & nodeName )
{
    auto it = m_lightsInfos.find(nodeName);
    if (it == m_lightsInfos.end())
        return "";
    else
        return it->second;
}

ModelInstance::ModelInstance( const std::string & name, const std::shared_ptr<ModelType> & modelType, const std::shared_ptr<caustica::SceneGraphNode> & parentNode )
    : m_modelType(modelType)
{
    assert( modelType != nullptr );
    const std::shared_ptr<caustica::Scene>& scene = modelType->GetGame().GetScene();
    m_node = std::make_shared<SceneGraphNode>();

    m_node = scene->GetSceneGraph()->Attach(parentNode, m_node);
    m_node->SetName(name);

    // Attach clones the scene prototype when it already belongs to the scene graph.
    std::shared_ptr<SceneGraphNode> modelRoot = scene->GetSceneGraph()->Attach(m_node, modelType->GetNode());
    if (modelRoot != nullptr)
    {
        const Pose& pose = modelType->GetModelPose();
        modelRoot->SetTransform(&pose.Translation, &pose.Rotation, &pose.Scaling);
        SpecialFixups(modelType->GetModelName(), modelRoot.get());
    }

    MapLightControllers( m_node.get() );
}

void ModelInstance::MapLightControllers( SceneGraphNode* node )
{
    if (node->GetLeaf() != nullptr && (node->GetLeaf()->GetContentFlags() & SceneContentFlags::Lights) != 0)
    {
        std::string data = m_modelType->FindLightControllerInfo(node->GetName());
        Json::Value jsonData;
        if (data != "" && caustica::json::FromString(data, jsonData))
        {
            auto lightController = std::make_shared<LightController>();
            if (!lightController->Read(jsonData))
            {
                assert(false && "Error reading LightController data");
            }
            else
            {
                lightController->Node = node;
                m_lightControllers.push_back(lightController);
            }
        }
        else
        {
            caustica::warning( "Model instance '%s', light '%s' has no controller", m_node->GetName().c_str(), node->GetName().c_str() );
        }
    }
    for (int i = 0; i < node->GetNumChildren(); i++)
        MapLightControllers(node->GetChild(i));
}

void ModelInstance::UpdateLightFromControllers(double gameTime)
{
    for (const auto & controller : m_lightControllers)
    {
        auto node = controller->Node;

        // NOTE - this gets frame old world transform - if changing scale at runtime, this won't work and instead it has to be fully updated
        float3 scale;
        dm::decomposeAffine<float>(node->GetLocalToWorldTransformFloat(), nullptr, nullptr, &scale);
        float reallyAverageScale = (scale.x + scale.y + scale.z) / 3.0f;

        Light* light = dynamic_cast<Light*>(node->GetLeaf().get());
        assert( light != nullptr );
        SpotLight* spotLight = dynamic_cast<SpotLight*>(node->GetLeaf().get());
        PointLight* pointLight = dynamic_cast<PointLight*>(node->GetLeaf().get());

        light->color = controller->Color;

        bool enabled = controller->Enabled;

        if (controller->AutoOffTime != 0 && controller->AutoOnTime != 0)
        {
            double periodLength = controller->AutoOffTime + controller->AutoOnTime;
            double scaledTime = (gameTime+controller->AutoOnOffTimeOffset) / periodLength;
            double remainder = dm::saturate<double>(scaledTime - floor(scaledTime));
            enabled &= remainder < (controller->AutoOffTime / periodLength);
        }

        float intensity = (enabled)?(controller->Intensity):(0);
        
        if (spotLight != nullptr)
        {
            spotLight->radius = reallyAverageScale;
            spotLight->intensity = intensity;
            
            spotLight->outerAngle = abs(controller->OuterAngle);
            spotLight->innerAngle = controller->InnerAngle;
            
            const bool kUseMinSpotlightFalloff = true;
            if (kUseMinSpotlightFalloff)
                spotLight->outerAngle = -spotLight->outerAngle; // this is a special flag to indicate that, instead of falling off to 0 when angle >= outerAngle, we should clamp to kMinSpotlightFalloff

        }
        if (pointLight != nullptr)
        {
            pointLight->radius = reallyAverageScale;
            pointLight->intensity = intensity;
        }
    }
}

void ModelInstance::SetTransform(const dm::double3& translation, const dm::dquat& rotation, const dm::double3& scaling)
{
    m_node->SetTransform(&translation, &rotation, &scaling);
}

void ModelInstance::SetTransform(const dm::float3 & translation, const dm::quat & rotation, const dm::float3 & scaling)
{
    dm::double3 transD = dm::double3(translation);
    dm::dquat rotD = dm::dquat(rotation);
    dm::double3 scalD = dm::double3(scaling);
    m_node->SetTransform(&transD, &rotD, &scalD);
}