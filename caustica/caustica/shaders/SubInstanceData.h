#ifndef __SUB_INSTANCE_DATA_H__
#define __SUB_INSTANCE_DATA_H__

#if !defined(__cplusplus) // not needed in the port so far
#pragma pack_matrix(row_major) // matrices below are expected in row_major
#else
using namespace caustica::math;
#endif

#define SUBINSTANCEDATA_EXTENDED 1

// per-instance-geometry data (avoids 1 layer of indirection that requires reading from instance and geometry buffers)
struct SubInstanceData  // could have been called GeometryInstanceData but that's already used in Falcor codebase
{
    static const int Flags_AlphaTested      	= (1<<16);
    static const int Flags_ExcludeFromNEE    	= (1<<17);
    static const int Flags_Dummy0               = (1<<18); // free up to (1<<23)

    static const int Flags_AlphaOffsetMask      = (0xFF000000);
    static const int Flags_AlphaOffsetOffset    = (24);

    uint FlagsAndAlphaInfo;                         // includes AlphaCutoff and AlphaTextureIndex
    uint GlobalGeometryIndex_PTMaterialDataIndex;   // index into t_GeometryData and t_GeometryDebugData in higher 16 bits, index in PTMaterial list in lower 16 bits
    uint EmissiveLightMappingOffset;                // if emissive mesh, index of the first light (see LightsBaker); will be 0xFFFFFFFF if triangles are not emissive!
    uint AnalyticProxyLightIndex;                   // if standing in as an analytic light proxy
    
#if SUBINSTANCEDATA_EXTENDED
    uint IndexBufferIndex_VertexBufferIndex;        // (allows skipping loading of t_GeometryData for alpha testing) packed 16 bits each
    uint IndexOffset;                               // (allows skipping loading of t_GeometryData for alpha testing) 
    uint TexCoord1Offset;                           // (allows skipping loading of t_GeometryData for alpha testing) 
    uint padding0;
#endif

    float AlphaCutoff()                         { return (FlagsAndAlphaInfo>>Flags_AlphaOffsetOffset) / 255.0f; }
    uint  AlphaTextureIndex()                   { return FlagsAndAlphaInfo & 0xFFFF; }
};

#endif // __SUB_INSTANCE_DATA_H__