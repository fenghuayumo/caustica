#ifndef __POLYMORPHIC_LIGHT_PT_CONFIG_H__
#define __POLYMORPHIC_LIGHT_PT_CONFIG_H__

#ifndef POLYLIGHT_OVERRIDE_CONFIG

// Polymorphic light config - RTXDI will also need ENV 
#define POLYLIGHT_SPHERE_ENABLE         1
#define POLYLIGHT_POINT_ENABLE          0   // handled by sphere
#define POLYLIGHT_TRIANGLE_ENABLE       1
#define POLYLIGHT_DIRECTIONAL_ENABLE    0   // baked into envmap (Distant lighting code)
#define POLYLIGHT_ENV_ENABLE            0   // handled by Distant lighting code, not polymorphic light
#define POLYLIGHT_QT_ENV_ENABLE         1   // environment map quad tree in equal area octahedral mapping

#define POLYLIGHT_CONFIGURED

#endif // POLYLIGHT_OVERRIDE_CONFIG

#endif // #define __POLYMORPHIC_LIGHT_PT_CONFIG_H__