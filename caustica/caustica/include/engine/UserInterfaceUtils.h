#pragma once

#include <math/math.h>
#include <platform/file_dialog.h>
#include <string>

namespace caustica
{
    struct Material;
}

namespace caustica
{
    bool materialEditor(Material* material, bool allowMaterialDomainChanges);

    bool azimuthElevationSliders(dm::double3& direction, bool negative = false);
}
