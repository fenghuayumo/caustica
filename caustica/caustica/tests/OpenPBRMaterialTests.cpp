#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace
{
constexpr float kEpsilon = 2e-4f;

bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "OpenPBR material test failed: %s\n", message);
    return false;
}

bool near(float a, float b, float epsilon = kEpsilon)
{
    return std::abs(a - b) <= epsilon;
}

float dielectricF0(float ior)
{
    const float f = (ior - 1.0f) / (ior + 1.0f);
    return f * f;
}

float modulatedIor(float ior, float weight)
{
    const float epsilon = std::copysign(std::sqrt(std::clamp(weight * dielectricF0(ior), 0.0f, 1.0f)), ior - 1.0f);
    return (1.0f + epsilon) / (1.0f - epsilon);
}

float schlick(float f0, float cosine)
{
    return f0 + (1.0f - f0) * std::pow(1.0f - cosine, 5.0f);
}

float f82(float f0, float edgeTint, float cosine)
{
    constexpr float muBar = 1.0f / 7.0f;
    const float base = schlick(f0, cosine);
    const float at82 = schlick(f0, muBar);
    const float correction = cosine * std::pow(1.0f - cosine, 6.0f)
        / (muBar * std::pow(1.0f - muBar, 6.0f));
    return std::clamp(base - correction * (at82 - edgeTint * at82), 0.0f, 1.0f);
}

std::array<float, 3> dispersionEta(float eta, float abbe, float scale)
{
    if (scale <= 0.0f)
        return { eta, eta, eta };
    const float nD = eta < 1.0f ? 1.0f / eta : eta;
    const float vd = abbe / scale;
    constexpr float c = 656.3f;
    constexpr float d = 587.6f;
    constexpr float f = 486.1f;
    const float B = (nD - 1.0f) / (vd * (1.0f / (f * f) - 1.0f / (c * c)));
    const float A = nD - B / (d * d);
    const std::array<float, 3> wavelengths{ 650.0f, d, f };
    std::array<float, 3> result{};
    for (size_t i = 0; i < result.size(); ++i)
    {
        const float n = A + B / (wavelengths[i] * wavelengths[i]);
        const float ratio = n / nD;
        result[i] = eta < 1.0f ? eta / ratio : eta * ratio;
    }
    return result;
}

float fuzzDirectionalAlbedo(float cosine, float roughness)
{
    const float exponent = 3.0f + (0.8f - 3.0f) * roughness;
    const float grazing = std::pow(std::clamp(1.0f - cosine, 0.0f, 1.0f), exponent);
    return std::clamp((0.18f + (0.65f - 0.18f) * roughness)
        + grazing * (0.35f + (0.2f - 0.35f) * roughness), 0.0f, 1.0f);
}
}

int main()
{
    bool passed = true;

    for (float weight : { 0.0f, 0.25f, 1.0f, 2.0f })
    {
        const float baseF0 = dielectricF0(1.5f);
        const float effective = modulatedIor(1.5f, weight);
        passed &= expect(near(dielectricF0(effective), std::min(weight * baseF0, 1.0f)),
            "specular_weight did not map to an equivalent effective IOR");
    }

    constexpr float mu82 = 1.0f / 7.0f;
    passed &= expect(near(f82(0.72f, 0.35f, mu82), 0.35f * schlick(0.72f, mu82)),
        "F82 tint did not hit the requested near-grazing reflectivity");
    passed &= expect(near(f82(0.72f, 1.0f, 0.42f), schlick(0.72f, 0.42f)),
        "neutral F82 tint did not reduce to Schlick");

    const auto noDispersion = dispersionEta(1.0f / 1.5f, 20.0f, 0.0f);
    passed &= expect(near(noDispersion[0], noDispersion[1]) && near(noDispersion[1], noDispersion[2]),
        "zero dispersion changed relative IOR");
    const auto dispersed = dispersionEta(1.0f / 1.5f, 20.0f, 1.0f);
    passed &= expect(dispersed[0] > dispersed[1] && dispersed[1] > dispersed[2],
        "RGB entering IORs do not produce increasing blue refraction");
    passed &= expect(near(dispersed[1], 1.0f / 1.5f),
        "dispersion changed the d-line reference IOR");

    // White-furnace layer regression: white, non-absorbing coat and fuzz must
    // redistribute substrate energy rather than add energy. Exercise the soft
    // ranges over view angle, coverage, roughness, and coat IOR.
    for (int viewStep = 1; viewStep <= 20; ++viewStep)
    {
        const float noV = static_cast<float>(viewStep) / 20.0f;
        for (int weightStep = 0; weightStep <= 10; ++weightStep)
        {
            const float weight = static_cast<float>(weightStep) / 10.0f;
            for (float coatIor : { 1.0f, 1.3f, 1.6f, 2.0f, 3.0f })
            {
                const float coatF = schlick(dielectricF0(coatIor), noV);
                const float coatEnergy = weight * coatF + (1.0f - weight * coatF);
                passed &= expect(near(coatEnergy, 1.0f),
                    "white coat failed the furnace layer identity");
            }
            for (float roughness : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                const float fuzzEnergy = fuzzDirectionalAlbedo(noV, roughness);
                const float layeredEnergy = weight * fuzzEnergy + (1.0f - weight * fuzzEnergy);
                passed &= expect(near(layeredEnergy, 1.0f),
                    "white fuzz failed the furnace layer identity");
            }
        }
    }

    return passed ? 0 : 1;
}
