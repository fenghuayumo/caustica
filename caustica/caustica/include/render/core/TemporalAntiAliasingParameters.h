#pragma once

namespace caustica::render
{
    enum class TemporalAntiAliasingJitter
    {
        MSAA,
        Halton,
        R2,
        WhiteNoise
    };

    struct TemporalAntiAliasingParameters
    {
        float newFrameWeight = 0.1f;
        float clampingFactor = 1.0f;
        float maxRadiance = 10000.f;
        bool enableHistoryClamping = true;

        // Requires CreateParameters::historyClampRelax single channel [0, 1] mask to be provided.
        // For texels with mask value of 0 the behavior is unchanged; for texels with mask value > 0,
        // 'newFrameWeight' will be reduced and 'clampingFactor' will be increased proportionally.
        bool useHistoryClampRelax = false;
    };
}
