#include "GamePropComponent.h"
#include "GameScene.h"
#include "GameProps.h"
#include <scene/GameModel.h>

#include <core/log.h>
#include <core/json.h>
#include <math/math.h>
#include <scene/camera/Camera.h>
#include <scene/SceneEcs.h>
#include <render/core/View.h>
#include <cmath>

#include <render/passes/debug/Korgi.h>

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

using namespace game;

PropComponentBase::PropComponentBase(class PropBase& prop, const std::string & type)
    : m_prop(prop), m_type(type)
{

}

class PropComponentPoliceLights : public PropComponentBase
{
public:
    PropComponentPoliceLights(class PropBase & prop, const std::string & type) : PropComponentBase(prop, type) {}

protected:
    float   m_start = 0.0f;
    float   m_stop  = 0.0f;

    std::shared_ptr<LightController> m_spotLeft;
    std::shared_ptr<LightController> m_spotRight;
    std::shared_ptr<LightController> m_blobLeft;
    std::shared_ptr<LightController> m_blobRight;
    float m_spotLeftIntensity = 0.0f;
    float m_spotRightIntensity = 0.0f;
    float m_blobLeftIntensity = 0.0f;
    float m_blobRightIntensity = 0.0f;
    dquat               m_blobLeftRot = dquat::identity();
    dquat               m_blobRightRot = dquat::identity();

    virtual void Load(const Json::Value & loadData) override
    {
        loadData["animTimeStart"] >> m_start;
        loadData["animTimeStop"] >> m_stop;

        auto* ew = m_prop.EntityWorld();
        for (auto & model : m_prop.GetModels())
        {
            for (auto & light : model->GetLights() )
            {
                if (!ew || light->Entity == caustica::ecs::NullEntity) continue;
                std::string lightName = ew->getEntityName(light->Entity);
                auto* ltc = ew->world().tryGet<caustica::scene::LocalTransformComponent>(light->Entity);
                dquat lightRot = ltc ? ltc->rotation : dquat::identity();

                if (lightName == "SpotLeft")
                { m_spotLeft = light; m_spotLeftIntensity = light->Intensity; }
                if (lightName == "SpotRight")
                { m_spotRight = light; m_spotRightIntensity = light->Intensity; }
                if (lightName == "BlobLeft")
                { m_blobLeft = light; m_blobLeftRot = lightRot; m_blobLeftIntensity = light->Intensity; }
                if (lightName == "BlobRight")
                { m_blobRight = light; m_blobRightRot = lightRot; m_blobRightIntensity = light->Intensity; }
            }
        }
    }

    void Tick(double gameTime, float animationTime, float deltaTime) override 
    {
        if (!m_spotLeft || !m_spotRight || !m_blobLeft || !m_blobRight)
            return;

        float lightTime = animationTime - m_start;
        float blobRightI = 0.0f;
        float blobRotAngle = 0.0f;
        if (animationTime > m_start && animationTime < m_stop)
        {
            float spotLeftI = 0.0f;
            float spotRightI = 0.0f;
            float blobLeftI = 0.0f;
            if (animationTime < m_start+2 || animationTime > m_stop-2) // intro / outro
            {
                spotLeftI   = m_spotLeftIntensity   * 0.1f;
                spotRightI  = m_spotRightIntensity  * 0.1f;
                blobLeftI    = m_blobLeftIntensity  * 0.01f;
                blobRightI   = m_blobRightIntensity * 0.01f;
            }
            else
            {
                float spotPeriod = 0.9f;
                spotPeriod = dm::saturate(fmodf(animationTime, spotPeriod) / spotPeriod);
                if (spotPeriod < 0.5f)
                    spotLeftI = m_spotLeftIntensity;
                else
                    spotRightI = m_spotRightIntensity;
                
                float blobPeriod = 1.1f;
                blobPeriod = dm::saturate(fmodf(animationTime, blobPeriod) / blobPeriod);
                blobRotAngle = (blobPeriod-0.5f) * PI_f * 2.0f;
                blobLeftI = m_blobLeftIntensity;
                blobRightI = m_blobRightIntensity;
            }
            m_spotLeft->Intensity   = spotLeftI;
            m_spotRight->Intensity  = spotRightI;
            m_blobLeft->Intensity   = blobLeftI;
            m_blobRight->Intensity  = blobRightI;
        }
        else
        {
            m_spotLeft->Intensity = 0.0f;
            m_spotRight->Intensity = 0.0f;
            m_blobLeft->Intensity = 0.0f;
            m_blobRight->Intensity = 0.0f;
        }
        dquat rot = rotationQuat<double>( {(double)blobRotAngle, 0, 0} );
        auto* ew = m_prop.EntityWorld();
        if (ew)
        {
            if (m_blobLeft->Entity != caustica::ecs::NullEntity)
                ew->setRotation(m_blobLeft->Entity, m_blobLeftRot * rot);
            if (m_blobRight->Entity != caustica::ecs::NullEntity)
                ew->setRotation(m_blobRight->Entity, m_blobRightRot * rot);
        }
    }
};

class PropComponentBasicInteractableUI : public PropComponentBase
{
public:
    PropComponentBasicInteractableUI(class PropBase& prop, const std::string& type) : PropComponentBase(prop, type) { }

protected:

    virtual void Load(const Json::Value& loadData) override
    {
        // loadData["animTimeStart"] >> m_start;
    }

    void Tick(double gameTime, float animationTime, float deltaTime) override
    {
    }

    ScreenGUISel StandaloneGUI(const std::shared_ptr<caustica::PlanarView> & view, const float2 & mousePos, const float2 & displaySize) override
    {
        ScreenGUISel sel;

        if (!view)
            return sel;

        // Get global bounds from ECS BoundsComponent
        auto* ew = m_prop.EntityWorld();
        caustica::ecs::Entity entity = m_prop.GetEntity();
        box3 bbox = box3::empty();
        if (ew && entity != caustica::ecs::NullEntity)
        {
            auto* boundsComp = ew->world().tryGet<caustica::scene::BoundsComponent>(entity);
            if (boundsComp)
                bbox = boundsComp->globalBounds;
        }
        if (bbox.isempty())
            return sel;

        float3 bcenter = bbox.center();
        float4x4 viewProj = view->getViewProjectionMatrix();

        static constexpr float2 invalidPos = float2(FLT_MAX, FLT_MAX);
        auto projectToScreen = [&viewProj, &view, &displaySize](const float3 & worldPos)
        {
            float4 projv = float4(worldPos, 1) * viewProj;
            projv /= projv.w;
            if( projv.z < 0 )
                return invalidPos;
            projv.xy() = projv.xy() * float2(0.5,-0.5) + float2(0.5,0.5);
            projv.xy() *= displaySize;
            if (projv.x < 0 || projv.x > displaySize.x || projv.y < 0 || projv.y > displaySize.y)
                return invalidPos;
            return projv.xy();
        };

        float2 sCenter = projectToScreen(bcenter);
        if (sCenter.x == FLT_MAX)
            return sel;

        float sRadius = 0.0;
        for( int i = 0; i < 8; i++ )
        {
            float2 sCorner = projectToScreen(bbox.getCorner(i));
            if (sCorner.x == FLT_MAX) 
                continue;
            sRadius = max( sRadius, length(sCenter - sCorner) );
        }

        if ( sRadius > 0 )
        {
            sRadius += 10.0f;

            if ( length(mousePos - sCenter) < sRadius)
            {
                sel.ScreenPos = sCenter;
                sel.ScreenRadius = sRadius + 10.0f;
                sel.Selected = true;
                sel.RangeToCamera = length(bcenter - view->getViewOrigin());
            }
        }
        return sel;
    }
};

std::shared_ptr<PropComponentBase> PropComponentBase::Create(class PropBase & prop, const Json::Value & loadData)
{
    std::string type;
    loadData["type"] >> type;

    std::shared_ptr<PropComponentBase> ret = nullptr;
    if (type == "PoliceLightingOnRX6")
        ret = std::make_shared<PropComponentPoliceLights>(prop, type);
    if (type == "BasicInteractableUI")
        ret = std::make_shared<PropComponentBasicInteractableUI>(prop, type);
    if (ret != nullptr)
        ret->Load(loadData);
    return ret;
}

