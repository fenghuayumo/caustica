#ifndef SKINNING_CB_H
#define SKINNING_CB_H

#define SkinningFlag_FirstFrame     0x01
#define SkinningFlag_Normals        0x02
#define SkinningFlag_Tangents       0x04
#define SkinningFlag_TexCoord1      0x08
#define SkinningFlag_TexCoord2      0x10

struct SkinningConstants
{
    uint numVertices;
    uint flags;
    uint inputPositionOffset;
    uint inputNormalOffset;

    uint inputTangentOffset;
    uint inputTexCoord1Offset;
    uint inputTexCoord2Offset;
    uint inputJointIndexOffset;

    uint inputJointWeightOffset;
    uint outputPositionOffset;
    uint outputPrevPositionOffset;
    uint outputNormalOffset;

    uint outputTangentOffset;
    uint outputTexCoord1Offset;
    uint outputTexCoord2Offset;
};

#endif // SKINNING_CB_H