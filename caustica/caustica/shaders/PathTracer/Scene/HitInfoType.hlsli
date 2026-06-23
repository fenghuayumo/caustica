#ifndef __HIT_INFO_TYPE_HLSLI__ // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
#define __HIT_INFO_TYPE_HLSLI__

#include "../Config.h"    

enum class HitType : uint32_t
{
    None                = 0,    ///< No hit.
    Triangle            = 1,    ///< Triangle hit.
    
#if 0 // not enabled in RTXPT
    Volume              = 2,    ///< Volume hit.

    // The following hit types are only available if hit info compression is disabled.

    DisplacedTriangle   = 3,    ///< Displaced triangle hit.
    Curve               = 4,    ///< Curve hit.
    SDFGrid             = 5,    ///< SDF grid hit.
#endif 
    //
    // Add new hit types here
    //

    Count // Must be last
};

#endif // __HIT_INFO_TYPE_HLSLI__