#pragma once

// DEMO-ONLY — editor game scripts. Not engine ECS. Not an embedding API.
// Canonical host sample: examples/cpp/thin_client (EntityWorld / SceneSpawn).

#include <math/math.h>
#include <ecs/Entity.h>
#include <scene/SceneContent.h>

#include <json/json-forwards.h>

#include <cfloat>
#include <tuple>
#include <vector>

namespace caustica { class FirstPersonCamera; }
class GameScene;

using namespace caustica::math;

namespace demo
{
    class ModelInstance;

    struct Pose
    {
        math::double3 Translation = { 0, 0, 0 };
        math::dquat   Rotation    = { 0, 0, 0, 1 };
        math::double3 Scaling     = { 1, 1, 1 };
        double      KeyTime     = 0.0;

        bool read(const Json::Value& node);
        Json::Value write();

        math::affine3 toTransform() const;
        std::tuple<math::float3, math::float3, math::float3> getPosDirUp() const;

        void setTransform(const math::affine3& transform);
        void setTransformFromCamera(const math::float3& pos, const math::float3& dir, const math::float3& up);
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

        math::float3 Color               = math::float3(1, 1, 1);
        float      Intensity           = 1.0f;
        bool       enabled             = true;
        bool       ToggleOnUIClick     = false;
        float      InnerAngle          = 0.0f;
        float      OuterAngle          = 0.0f;
        float      AutoOffTime         = 0.0f;
        float      AutoOnTime          = 0.0f;
        float      AutoOnOffTimeOffset = 0.0f;

        bool read(const Json::Value& node);
        Json::Value write();
    };

    struct ScreenGUISel
    {
        math::float2 ScreenPos     = { 0, 0 };
        float      ScreenRadius  = 0.0f;
        float      RangeToCamera = FLT_MAX;
        bool       Selected      = false;
    };
}

inline void operator>>(const Json::Value& node, demo::Pose& p) { p.read(node); }
