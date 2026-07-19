#ifndef __LOBE_TYPE_HLSLI__ // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
#define __LOBE_TYPE_HLSLI__

#include "../../Config.h"    

/** Flags representing the various lobes of a BxDF.
*/
enum class LobeType // : uint32_t
{
    None                    = 0x00u,

    DiffuseReflection       = 0x01u,
    SpecularReflection      = 0x02u,
    DeltaReflection         = 0x04u,
    ClearcoatDeltaReflection= 0x08u, // OpenPBR coat delta reflection (PSD index 2)

    DiffuseTransmission     = 0x10u,
    SpecularTransmission    = 0x20u,
    DeltaTransmission       = 0x40u,

    Diffuse                 = 0x11u,
    Specular                = 0x22u,
    Delta                   = 0x4Cu, // DeltaReflection | ClearcoatDeltaReflection | DeltaTransmission
    NonDelta                = 0x33u,

    Reflection              = 0x0fu,
    Transmission            = 0xf0u,

    NonDeltaReflection      = 0x03u,
    NonDeltaTransmission    = 0x30u,

    All                     = 0xffu,
};

#endif // __LOBE_TYPE_HLSLI__