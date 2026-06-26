#pragma once

// =============================================================================
// GameTypes.h — Sample game utility types (application layer).
// =============================================================================

#include <math/math.h>
#include <scene/SceneGraph.h>

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

        bool Read(const Json::Value& node);
        Json::Value Write();

        dm::affine3 ToTransform() const;
        std::tuple<dm::float3, dm::float3, dm::float3> GetPosDirUp() const;

        void SetTransform(const dm::affine3& transform);
        void SetTransformFromCamera(const dm::float3& pos, const dm::float3& dir, const dm::float3& up);
    };

    struct KeyframeAnimation
    {
        std::vector<Pose> Keys;
        double KeyTimeMin = 0.0;
        double KeyTimeMax = 0.0;

        bool Read(const Json::Value& node);
        Json::Value Write();

        void FromKeys(const std::vector<Pose>& keys);
        bool GetAt(double time, bool wrap, Pose& outPose, float& outAnimTime);

    private:
        int LastFound = -1;
    };

    struct LightController
    {
        caustica::SceneGraphNode* Node = nullptr;

        dm::float3 Color               = dm::float3(1, 1, 1);
        float      Intensity           = 1.0f;
        bool       Enabled             = true;
        bool       ToggleOnUIClick     = false;
        float      InnerAngle          = 0.0f;
        float      OuterAngle          = 0.0f;
        float      AutoOffTime         = 0.0f;
        float      AutoOnTime          = 0.0f;
        float      AutoOnOffTimeOffset = 0.0f;

        caustica::SpotLight*  GetSpotLight();
        caustica::PointLight* GetPointLight();

        bool Read(const Json::Value& node);
        Json::Value Write();
    };

    struct ScreenGUISel
    {
        dm::float2 ScreenPos     = { 0, 0 };
        float      ScreenRadius  = 0.0f;
        float      RangeToCamera = FLT_MAX;
        bool       Selected      = false;
    };
}

inline void operator>>(const Json::Value& node, game::Pose& p) { p.Read(node); }
