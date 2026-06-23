#ifndef __HOMOGENEOUS_VOLUME_DATA_HLSLI__ // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
#define __HOMOGENEOUS_VOLUME_DATA_HLSLI__

#include "../../Config.h"    

struct HomogeneousVolumeData
{
    float3 sigmaA;  ///< Absorption coefficient.
    float3 sigmaS;  ///< Scattering coefficient.
    float g;        ///< Phase function anisotropy.
};

#endif // __HOMOGENEOUS_VOLUME_DATA_HLSLI__