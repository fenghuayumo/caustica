#pragma once

#include <math/math.h>
#include <string>
#include <memory>
#include <unordered_map>
#include <optional>
#include <vector>

namespace Json
{
    class Value;
}

namespace caustica::animation
{
    struct Keyframe
    {
        float time = 0.f;
        dm::float4 value = 0.f;
        dm::float4 inTangent = 0.f;
        dm::float4 outTangent = 0.f;
    };

    enum class InterpolationMode
    {
        Step,
        Linear,
        Slerp,
        CatmullRomSpline,
        HermiteSpline
    };

    dm::float4 Interpolate(InterpolationMode mode, 
        const Keyframe& a, const Keyframe& b,
        const Keyframe& c, const Keyframe& d, float t, float dt);

    class Sampler
    {
    protected:
        std::vector<Keyframe> m_Keyframes;
        InterpolationMode m_Mode = InterpolationMode::Step;

    public:
        Sampler() = default;
        virtual ~Sampler() = default;

        std::optional<dm::float4> Evaluate(float time, bool extrapolateLastValues = false) const;

        [[nodiscard]] std::vector<Keyframe>& GetKeyframes() { return m_Keyframes; }
        void AddKeyframe(const Keyframe keyframe);

        [[nodiscard]] InterpolationMode GetMode() const { return m_Mode; }
        void SetInterpolationMode(InterpolationMode mode) { m_Mode = mode; }

        [[nodiscard]] float GetStartTime() const;
        [[nodiscard]] float GetEndTime() const;

        void Load(Json::Value& node);
    };

    class Sequence
    {
    protected:
        std::unordered_map<std::string, std::shared_ptr<Sampler>> m_Tracks;
        float m_Duration = 0.f;

    public:
        Sequence() = default;
        virtual ~Sequence() = default;

        std::shared_ptr<Sampler> GetTrack(const std::string& name)
        {
            return m_Tracks[name];
        }

        std::optional<dm::float4> Evaluate(const std::string& name, float time, bool extrapolateLastValues = false);

        void AddTrack(const std::string& name, const std::shared_ptr<Sampler>& track);

        [[nodiscard]] float GetDuration() const { return m_Duration; }

        void Load(Json::Value& node);
    };
}
