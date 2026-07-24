#pragma once

//#include "NrdIntegration.h"
#include <render/passes/denoisers/nrd/NRD.h>

namespace NrdConfig {

    enum class DenoiserMethod : uint32_t
    {
        REBLUR,
        RELAX,
        MaxCount
    };

    nrd::RelaxSettings getDefaultRELAXSettings();

    nrd::ReblurSettings getDefaultREBLURSettings();


}
