#ifndef LIGHT_PROBE_CB_H
#define LIGHT_PROBE_CB_H

struct LightProbeProcessingConstants
{
    uint sampleCount;
    float lodBias;
    float roughness;
    float inputCubeSize;
};

#endif // LIGHT_PROBE_CB_H