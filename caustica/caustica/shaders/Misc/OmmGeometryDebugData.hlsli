#ifndef __OMM_GEOMETRY_DEBUG_DATA_HLSLI__ // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
#define __OMM_GEOMETRY_DEBUG_DATA_HLSLI__


struct GeometryDebugData
{
    unsigned int ommArrayDataBufferIndex;
    unsigned int ommArrayDataBufferOffset;
    unsigned int ommDescArrayBufferIndex;
    unsigned int ommDescArrayBufferOffset;

    unsigned int ommIndexBufferIndex;
    unsigned int ommIndexBufferOffset;
    unsigned int ommIndexBuffer16Bit; // (bool) 16 or 32 bit indices.
    unsigned int _pad0;
};

#endif // __OMM_GEOMETRY_DEBUG_DATA_HLSLI__
