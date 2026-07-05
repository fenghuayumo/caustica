#pragma once

// Caustica RHI fork entry point.
// Include this (or <rhi/nvrhi.h> directly) for GPU device APIs.
// Caustica-specific extensions live under <rhi/caustica/...>.

#include <rhi/nvrhi.h>
#include <rhi/utils.h>
#include <rhi/caustica/format.h>

namespace nvrhi::caustica
{
static constexpr const char* c_ForkName = "caustica-rhi";
}
