# RTXDI, OMM, and RTXTF — vendored inside the caustica engine

set(RTXDI_SKIP_SHADER_VALIDATION ON CACHE STRING "Skip RTXDI shader validation")

add_subdirectory(rtxdi)
add_subdirectory(omm)
