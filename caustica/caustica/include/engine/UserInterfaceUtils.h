#pragma once

#include <math/math.h>
#include <string>

namespace caustica
{
    struct Material;
    class Light;
    class DirectionalLight;
    class PointLight;
    class SpotLight;
}

namespace caustica
{
    bool FileDialog(bool bOpen, const char* pFilters, std::string& fileName);
    bool FolderDialog(const char* pTitle, const char* pDefaultFolder, std::string& outFolderName);
    
    bool MaterialEditor(Material* material, bool allowMaterialDomainChanges);

    bool LightEditor_Directional(DirectionalLight& light);
    bool LightEditor_Point(PointLight& light);
    bool LightEditor_Spot(SpotLight& light);
    bool LightEditor(Light& light);

    bool AzimuthElevationSliders(dm::double3& direction, bool negative = false);
}
