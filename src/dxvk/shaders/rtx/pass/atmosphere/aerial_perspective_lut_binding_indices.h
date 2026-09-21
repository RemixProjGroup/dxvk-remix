#pragma once

#define AERIAL_PERSPECTIVE_LUT_ATMOSPHERE_ARGS       0
#define AERIAL_PERSPECTIVE_LUT_TRANSMITTANCE_INPUT   1
#define AERIAL_PERSPECTIVE_LUT_MULTISCATTERING_INPUT 2
#define AERIAL_PERSPECTIVE_LUT_SAMPLER               3
#define AERIAL_PERSPECTIVE_LUT_OUTPUT                4
// NV-DXVK start: Missing TLAS selects a shader without ray queries.
#define AERIAL_PERSPECTIVE_LUT_ACCELERATION_STRUCTURE 5
#define AERIAL_PERSPECTIVE_LUT_VISIBILITY             6
// NV-DXVK end
// NV-DXVK start: Local light in-scatter.
#define AERIAL_PERSPECTIVE_LUT_LIGHTS                 7
#define AERIAL_PERSPECTIVE_LUT_LIGHT_CLUSTERS         8
#define AERIAL_PERSPECTIVE_LUT_LOCAL_OUTPUT           9
// NV-DXVK end
// NV-DXVK start: Match sky/cloud camera projection.
#define AERIAL_PERSPECTIVE_LUT_CAMERA 10
// NV-DXVK end
