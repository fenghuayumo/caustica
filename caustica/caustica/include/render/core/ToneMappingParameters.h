#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <math/math.h>

// ToneMapping_cb.h uses HLSL-style uint / float3x4; bind them before include.
using caustica::math::uint;
using caustica::math::float3x4;

#include <shaders/render/toneMapper/ToneMapping_cb.h>

enum class ExposureMode : uint32_t
{
    AperturePriority,
    ShutterPriority,
};

struct ToneMappingParameters
{
    ExposureMode exposureMode = ExposureMode::AperturePriority;
    ToneMapperOperator toneMapOperator = ToneMapperOperator::Aces;
    bool autoExposure = false;
    float exposureCompensation = 0.0f;
    float exposureValue = 0.0f;
    float filmSpeed = 100.f;
    float fNumber = 1.f;
    float shutter = 1.f;
    bool whiteBalance = false;
    float whitePoint = 6500.0f;
    float whiteMaxLuminance = 1.0f;
    float whiteScale = 5.1f;
    bool clamped = true;
    float exposureValueMin = -16.0f;
    float exposureValueMax = 16.0f;
};

static const std::unordered_map<ExposureMode, std::string> ExposureModeToString = {
    {ExposureMode::AperturePriority, "Aperture Priority"},
    {ExposureMode::ShutterPriority, "Shutter Priority"}
};
