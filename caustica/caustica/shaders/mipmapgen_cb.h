#ifndef MIPMAP_GEN_CB_H
#define MIPMAP_GEN_CB_H

#define GROUP_SIZE 16
#define LOD0_TILE_SIZE 8
#define NUM_LODS 4

// Number of compute dispatches needed to reduce all the 
// mip-levels at a maximum resolution of 16k : 
//     (uint)(std::ceil(std::log2f(16384)/NUM_LODS)) = 4
#define MAX_PASSES 4

#define MODE_COLOR  0
#define MODE_MIN    1
#define MODE_MAX    2
#define MODE_MINMAX 3

struct MipmmapGenConstants
{
    uint dispatch;
    uint numLODs;
    uint padding[2];
};

#endif // MIPMAP_GEN_CB_H