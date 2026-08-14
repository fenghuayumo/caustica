#ifndef CAUSTICA_HAIR_GEOMETRY_HLSLI
#define CAUSTICA_HAIR_GEOMETRY_HLSLI

// Analytic normal of the tapered infinite cylinder/cone represented by a DOTS
// ribbon hit. The ray is intersected with the fiber surface so both ribbons
// shade as one continuous cylindrical strand.
float3 CausticaAdjustDotsNormal(
    float3 rayOrigin, float3 rayDirection,
    float3 endpoint0, float3 endpoint1,
    float radius0, float radius1,
    float3 fallbackNormal)
{
    const float3 oa = rayOrigin - endpoint0;
    const float3 ba = endpoint1 - endpoint0;
    const float radiusDelta = radius0 - radius1;
    const float m0 = dot(ba, ba);
    const float m1 = dot(ba, oa);
    const float m2 = dot(ba, rayDirection);
    const float m3 = dot(rayDirection, oa);
    const float m5 = dot(oa, oa);
    const float d2 = m0 - radiusDelta * radiusDelta;
    const float k2 = d2 - m2 * m2;
    const float k1 = d2 * m3 - m1 * m2 + m2 * radiusDelta * radius0;
    const float k0 = d2 * m5 - m1 * m1 + 2.f * m1 * radiusDelta * radius0 - m0 * radius0 * radius0;
    const float discriminant = k1 * k1 - k0 * k2;
    if (discriminant < 0.f || abs(k2) < 1e-12f)
        return fallbackNormal;

    const float hitT = (-sqrt(discriminant) - k1) / k2;
    const float y = m1 - radius0 * radiusDelta + hitT * m2;
    const float3 hitVector = d2 * (oa + hitT * rayDirection);
    const float3 axisVector = ba * y;
    const float3 normal = hitVector - axisVector;
    return dot(normal, normal) > 1e-12f ? normalize(normal) : fallbackNormal;
}

#endif
