#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class OidnDenoiser
{
public:
    enum class Passes
    {
        ColorOnly = 0,
        Albedo = 1,
        AlbedoNormal = 2
    };

    enum class Prefilter
    {
        None = 0,
        Fast = 1,
        Accurate = 2
    };

    enum class Quality
    {
        Fast = 0,
        Balanced = 1,
        High = 2
    };

    struct Options
    {
        bool UseGPU = true;
        Passes GuidePasses = Passes::Albedo;
        Prefilter GuidePrefilter = Prefilter::Fast;
        Quality FilterQuality = Quality::Balanced;
        const float* AlbedoRgb = nullptr;
        const float* NormalRgb = nullptr;
    };

    OidnDenoiser();
    ~OidnDenoiser();

    [[nodiscard]] bool isAvailable() const;
    [[nodiscard]] const std::string& getLastError() const;
    [[nodiscard]] const std::string& getDeviceDescription() const;

    bool denoise(const float* inputRgb, uint32_t width, uint32_t height, const Options& options, std::vector<float>& outputRgb);
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
