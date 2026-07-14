#pragma once

#include <math/math.h>
#include <ecs/Entity.h>
#include <scene/SceneObjects.h>
#include <scene/SceneContent.h>

#include <json/json-forwards.h>

#include <tuple>
#include <vector>

namespace caustica { class FirstPersonCamera; }
class GameScene;

using namespace caustica::math;

namespace game
{
    class ModelInstance;

    struct Pose
    {
        dm::double3 Translation = { 0, 0, 0 };
        dm::dquat   Rotation    = { 0, 0, 0, 1 };
        dm::double3 Scaling     = { 1, 1, 1 };
        double      KeyTime     = 0.0;

        bool read(const Json::Value& node);
        Json::Value write();

        dm::affine3 toTransform() const;
        std::tuple<dm::float3, dm::float3, dm::float3> getPosDirUp() const;

        void setTransform(const dm::affine3& transform);
        void setTransformFromCamera(const dm::float3& pos, const dm::float3& dir, const dm::float3& up);
    };

    struct KeyframeAnimation
    {
        std::vector<Pose> Keys;
        double KeyTimeMin = 0.0;
        double KeyTimeMax = 0.0;

        bool read(const Json::Value& node);
        Json::Value write();

        void fromKeys(const std::vector<Pose>& keys);
        bool getAt(double time, bool wrap, Pose& outPose, float& outAnimTime);

    private:
        int LastFound = -1;
    };

    struct LightController
    {
        caustica::ecs::Entity Entity = caustica::ecs::NullEntity;

        dm::float3 Color               = dm::float3(1, 1, 1);
        float      Intensity           = 1.0f;
        bool       enabled             = true;
        bool       ToggleOnUIClick     = false;
        float      InnerAngle          = 0.0f;
        float      OuterAngle          = 0.0f;
        float      AutoOffTime         = 0.0f;
        float      AutoOnTime          = 0.0f;
        float      AutoOnOffTimeOffset = 0.0f;

        caustica::SpotLight*  getSpotLight();
        caustica::PointLight* getPointLight();

        bool read(const Json::Value& node);
        Json::Value write();
    };

    struct ScreenGUISel
    {
        dm::float2 ScreenPos     = { 0, 0 };
        float      ScreenRadius  = 0.0f;
        float      RangeToCamera = FLT_MAX;
        bool       Selected      = false;
    };
}

inline void operator>>(const Json::Value& node, game::Pose& p) { p.read(node); }
