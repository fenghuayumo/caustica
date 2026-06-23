#include <shaders/binding_helpers.hlsli>

Buffer<float> t_IesData : register(t0);
VK_IMAGE_FORMAT("r16f") RWTexture2D<float> u_OutputTexture : register(u0);

float FindAngleIndex(float angle, int offset, int count)
{
    if (count == 1)
        return 0;

    float left;
    float right = t_IesData[offset];

    if(angle <= right)
        return 0;

    for(int i = 1; i < count; i++)
    {
        left = right;
        right = t_IesData[offset + i];

        if (angle >= left && angle <= right)
            return float(i - 1) + ((right > left) ? (angle - left) / (right - left) : 0.0);
    }

    return count - 1;
}

[numthreads(16, 16, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID)
{
    int numVerticalAngles = int(t_IesData[3]);
    int numHorizontalAngles = int(t_IesData[4]);
    int headerSize = 13;

    float verticalAngle = float(globalIndex.x) * (180.0 / 128.0);
    float horizontalAngle = float(globalIndex.y) * (360.0 / 128.0) - 180.0;

    float lastVerticalAngle = t_IesData[headerSize + numVerticalAngles - 1];
    float lastHorizontalAngle = t_IesData[headerSize + numVerticalAngles + numHorizontalAngles - 1];

    if (verticalAngle > lastVerticalAngle)
    {
        u_OutputTexture[globalIndex] = 0;
        return;
    }
    
    if (lastHorizontalAngle <= 180.0)
    {
        // Apply symmertry
        horizontalAngle = abs(horizontalAngle);
        if (lastHorizontalAngle == 90.0 && horizontalAngle > 90.0)
        {
            horizontalAngle = 180.0 - horizontalAngle;
        }
    }
    else
    {
        // No symmetry, but the profile has data in 0..360 degree range, convert our -180..180 range to that
        if (horizontalAngle < 0)
            horizontalAngle += 360.0;
    }

    float verticalAngleIndex = FindAngleIndex(verticalAngle, headerSize, numVerticalAngles);
    float horizontalAngleIndex = FindAngleIndex(horizontalAngle, headerSize + numVerticalAngles, numHorizontalAngles);

    int dataOffset = headerSize + numHorizontalAngles + numVerticalAngles;

    float a = t_IesData[dataOffset + int(floor(horizontalAngleIndex)) * numVerticalAngles + int(floor(verticalAngleIndex))];
    float b = t_IesData[dataOffset + int(floor(horizontalAngleIndex)) * numVerticalAngles + int(ceil(verticalAngleIndex))];
    float c = t_IesData[dataOffset + int(ceil(horizontalAngleIndex)) * numVerticalAngles + int(floor(verticalAngleIndex))];
    float d = t_IesData[dataOffset + int(ceil(horizontalAngleIndex)) * numVerticalAngles + int(ceil(verticalAngleIndex))];

    float candelas = lerp(
        lerp(a, b, frac(verticalAngleIndex)),
        lerp(c, d, frac(verticalAngleIndex)),
        frac(horizontalAngleIndex));

    float normalization = t_IesData[0];

    float result = candelas * normalization;

    u_OutputTexture[globalIndex] = result;
}