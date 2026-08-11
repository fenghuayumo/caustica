#include <render/passes/rtxdi/RtxdiApplicationSettings.h>

namespace caustica::rtxdi_config
{
DIInitialSamplingParameters defaultDIInitialSampling() { return {}; }
DITemporalResamplingParameters defaultDITemporalResampling() { return {}; }
DISpatialResamplingParameters defaultDISpatialResampling() { return {}; }
DIShadingParameters defaultDIShading() { return {}; }
GITemporalResamplingParameters defaultGITemporalResampling() { return {}; }
GISpatialResamplingParameters defaultGISpatialResampling() { return {}; }
GIFinalShadingParameters defaultGIFinalShading() { return {}; }
PTInitialSamplingParameters defaultPTInitialSampling() { return {}; }
PTTemporalResamplingParameters defaultPTTemporalResampling() { return {}; }
PTReconnectionParameters defaultPTReconnection() { return {}; }
PTHybridShiftParameters defaultPTHybridShift() { return {}; }
BoilingFilterParameters defaultPTBoilingFilter() { return {}; }
PTSpatialResamplingParameters defaultPTSpatialResampling() { return {}; }
} // namespace caustica::rtxdi_config
