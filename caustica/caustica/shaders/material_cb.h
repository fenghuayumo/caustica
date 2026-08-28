#ifndef MATERIAL_CB_H
#define MATERIAL_CB_H

static const int MaterialDomain_Opaque                   = 0;
static const int MaterialDomain_AlphaTested              = 1;
static const int MaterialDomain_AlphaBlended             = 2;
static const int MaterialDomain_Transmissive             = 3;
static const int MaterialDomain_TransmissiveAlphaTested  = 4;
static const int MaterialDomain_TransmissiveAlphaBlended = 5;

static const int MaterialFlags_UseSpecularGlossModel            = 0x00000001;
static const int MaterialFlags_DoubleSided                      = 0x00000002;
static const int MaterialFlags_UseMetalRoughOrSpecularTexture   = 0x00000004;
static const int MaterialFlags_UseBaseOrDiffuseTexture          = 0x00000008;
static const int MaterialFlags_UseEmissiveTexture               = 0x00000010;
static const int MaterialFlags_UseNormalTexture                 = 0x00000020;
static const int MaterialFlags_UseOcclusionTexture              = 0x00000040;
static const int MaterialFlags_UseTransmissionTexture           = 0x00000080;
static const int MaterialFlags_MetalnessInRedChannel            = 0x00000100;
static const int MaterialFlags_UseOpacityTexture                = 0x00000200;
static const int MaterialFlags_SubsurfaceScattering             = 0x00000400;
static const int MaterialFlags_Hair                             = 0x00000800;

#endif // MATERIAL_CB_H
