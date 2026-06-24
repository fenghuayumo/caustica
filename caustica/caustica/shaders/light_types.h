#ifndef LIGHT_TYPES_H
#define LIGHT_TYPES_H

#ifdef __cplusplus
constexpr int LightType_None = 0;
constexpr int LightType_Directional = 1;
constexpr int LightType_Spot = 2;
constexpr int LightType_Point = 3;
constexpr int LightType_Environment = 1000;
#else
static const int LightType_None = 0;
static const int LightType_Directional = 1;
static const int LightType_Spot = 2;
static const int LightType_Point = 3;
static const int LightType_Environment = 1000;
#endif

#endif // LIGHT_CB_H