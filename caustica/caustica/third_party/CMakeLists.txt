# RTXDI and OMM backend implementations retained inside the Caustica engine.
# Their reusable dependencies are repository-level submodules under External.

set(RTXDI_SKIP_SHADER_VALIDATION ON CACHE STRING "Skip RTXDI shader validation")

add_subdirectory(rtxdi)
add_subdirectory(omm)
