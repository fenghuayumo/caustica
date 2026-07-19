#include <animation/KeyframeAnimation.h>
#include <core/json.h>
#include <core/log.h>
#include <json/json-forwards.h>
#include <algorithm>
#include <cassert>
#include <cmath>

using namespace caustica::math;
using namespace caustica;
using namespace caustica::animation;

float4 caustica::animation::interpolate(const InterpolationMode mode,
    const Keyframe& a, const Keyframe& b, const Keyframe& c, const Keyframe& d, const float t, const float dt)
{
    switch (mode)
    {
    case InterpolationMode::Step:
        return b.value;

    case InterpolationMode::Linear:
        return lerp(b.value, c.value, t);

    case InterpolationMode::Slerp: {
        quat qb = quat::fromXYZW(b.value);
        quat qc = quat::fromXYZW(c.value);
        quat qr = slerp(qb, qc, t);
        return float4(qr.x, qr.y, qr.z, qr.w);
    }

    case InterpolationMode::CatmullRomSpline: {
        // https://en.wikipedia.org/wiki/Cubic_Hermite_spline#Interpolation_on_the_unit_interval_with_matched_derivatives_at_endpoints
        // a = p[n-1], b = p[n], c = p[n+1], d = p[n+2]
        float4 i = -a.value + 3.f * b.value - 3.f * c.value + d.value;
        float4 j = 2.f * a.value - 5.f * b.value + 4.f * c.value - d.value;
        float4 k = -a.value + c.value;
        return 0.5f * ((i * t + j) * t + k) * t + b.value;
    }

    case InterpolationMode::HermiteSpline: {
        // https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#appendix-c-spline-interpolation
        const float t2 = t * t;
        const float t3 = t2 * t;
        return (2.f * t3 - 3.f * t2 + 1.f) * b.value
             + (t3 - 2.f * t2 + t) * b.outTangent * dt
             + (-2.f * t3 + 3.f * t2) * c.value
             + (t3 - t2) * c.inTangent * dt;
    }

    default:
        assert(!"Unknown interpolation mode");
        return b.value;
    }
}

std::optional<dm::float4> Sampler::evaluate(float time, bool extrapolateLastValues) const
{
    const size_t count = m_Keyframes.size();

    if (count == 0)
        return std::optional<float4>();

    if (time <= m_Keyframes[0].time)
        return std::optional(m_Keyframes[0].value);

    if (count == 1 || time >= m_Keyframes[count - 1].time)
    {
        if (extrapolateLastValues)
            return std::optional(m_Keyframes[count - 1].value);
        else
            return std::optional<float4>();
    }
    
    // Use binary search to locate the pair of keyframes (b, c) so that (b.time <= time < c.time).
    // Assume that the keyframe vector is sorted by time.
    // Right limit starts at (count - 2) because we're always looking at consecutive pairs of items, not single items.
    size_t left = 0;
    size_t right = count - 2;
    while (left <= right)
    {
        size_t const middle = (left + right) / 2;

        const float tb = m_Keyframes[middle].time;
        const float tc = m_Keyframes[middle + 1].time;

        if (time < tb)
            right = middle - 1;
        else if (time >= tc)
            left = middle + 1;
        else
        {
            // Found the pair containing the required time, stop.
            left = right = middle;
            break;
        }
    }

    // load 4 keyframes around the required time.
    // The outside keyframes (a) and (d) are needed for higher-order interpolation.
    size_t const offset = left;
    const Keyframe& b = m_Keyframes[offset];
    const Keyframe& c = m_Keyframes[offset + 1];
    const Keyframe& a = (offset > 0) ? m_Keyframes[offset - 1] : b;
    const Keyframe& d = (offset < count - 2) ? m_Keyframes[offset + 2] : c;
    
    // Validate that the (b, c) keyframes indeed contain the required time.
    if (time < b.time || time >= c.time)
    {
        assert(!"Incorrect keyframe search result! Array not sorted?");
        return std::nullopt;
    }
    
    const float dt = c.time - b.time;
    const float u = (time - b.time) / dt;

    float4 y = interpolate(m_Mode, a, b, c, d, u, dt);
    
    return std::optional(y);
}

void Sampler::addKeyframe(const Keyframe keyframe)
{
    m_Keyframes.push_back(keyframe);
}

bool Sampler::upsertKeyframe(const Keyframe& keyframe, float timeEpsilon)
{
    auto it = std::lower_bound(
        m_Keyframes.begin(),
        m_Keyframes.end(),
        keyframe.time,
        [](const Keyframe& candidate, float time) { return candidate.time < time; });

    if (it != m_Keyframes.end() && std::fabs(it->time - keyframe.time) <= timeEpsilon)
    {
        it->value = keyframe.value;
        it->inTangent = keyframe.inTangent;
        it->outTangent = keyframe.outTangent;
        return false;
    }
    if (it != m_Keyframes.begin())
    {
        auto previous = std::prev(it);
        if (std::fabs(previous->time - keyframe.time) <= timeEpsilon)
        {
            previous->value = keyframe.value;
            previous->inTangent = keyframe.inTangent;
            previous->outTangent = keyframe.outTangent;
            return false;
        }
    }

    m_Keyframes.insert(it, keyframe);
    return true;
}

bool Sampler::removeKeyframe(float time, float timeEpsilon)
{
    const auto oldSize = m_Keyframes.size();
    std::erase_if(m_Keyframes, [time, timeEpsilon](const Keyframe& keyframe) {
        return std::fabs(keyframe.time - time) <= timeEpsilon;
    });
    return m_Keyframes.size() != oldSize;
}

bool Sampler::hasKeyframe(float time, float timeEpsilon) const
{
    const auto it = std::lower_bound(
        m_Keyframes.begin(),
        m_Keyframes.end(),
        time,
        [](const Keyframe& candidate, float candidateTime) { return candidate.time < candidateTime; });
    if (it != m_Keyframes.end() && std::fabs(it->time - time) <= timeEpsilon)
        return true;
    return it != m_Keyframes.begin() && std::fabs(std::prev(it)->time - time) <= timeEpsilon;
}

float Sampler::getStartTime() const
{
    if (!m_Keyframes.empty())
        return m_Keyframes[0].time;

    return 0.f;
}

float Sampler::getEndTime() const
{
    if (!m_Keyframes.empty())
        return m_Keyframes[m_Keyframes.size() - 1].time;

    return 0.f;
}

void Sampler::load(Json::Value& node)
{
    if (node["mode"].isString())
    {
        std::string mode = node["mode"].asString();
        if (mode == "step")
            setInterpolationMode(InterpolationMode::Step);
        else if (mode == "linear")
            setInterpolationMode(InterpolationMode::Linear);
        if (mode == "spline")
            setInterpolationMode(InterpolationMode::CatmullRomSpline);
    }

    bool warningPrinted = false;
    Json::Value& valuesNode = node["values"];
    if (valuesNode.isArray())
    {
        for (Json::Value& valueNode : valuesNode)
        {
            Keyframe keyframe;
            keyframe.time = valueNode["time"].asFloat();
            if (valueNode.isNumeric())
            {
                keyframe.value.x = valueNode.asFloat();
            }
            else if (valueNode.isArray())
            {
                if (valueNode.size() >= 1) keyframe.value.x = valueNode[0].asFloat();  // NOLINT(readability-container-size-empty)
                if (valueNode.size() >= 2) keyframe.value.y = valueNode[1].asFloat();
                if (valueNode.size() >= 3) keyframe.value.z = valueNode[2].asFloat();
                if (valueNode.size() >= 4) keyframe.value.w = valueNode[3].asFloat();
            }
            else if ((valueNode.isObject() || valueNode.isString()) && !warningPrinted)
            {
                caustica::warning("Objects and strings are not supported as animation keyframe values.");
                warningPrinted = true;
                continue;
            }

            addKeyframe(keyframe);
        }
        
        std::sort(m_Keyframes.begin(), m_Keyframes.end(),
            [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    }
}

std::optional<dm::float4> Sequence::evaluate(const std::string& name, float time, bool extrapolateLastValues)
{
    std::shared_ptr<Sampler> track = getTrack(name);
    if (!track)
        return std::optional<dm::float4>();

    return track->evaluate(time, extrapolateLastValues);
}

void Sequence::addTrack(const std::string& name, const std::shared_ptr<Sampler>& track)
{
    m_Tracks[name] = track;
    m_Duration = std::max(m_Duration, track->getEndTime());
}

void Sequence::load(Json::Value& node)
{
    for (auto& trackNode : node)
    {
        auto track = std::make_shared<Sampler>();
        track->load(trackNode);

        std::string name = trackNode["name"].asString();
        addTrack(name, track);
    }
}