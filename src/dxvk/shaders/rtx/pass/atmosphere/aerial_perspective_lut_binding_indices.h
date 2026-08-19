#pragma once

#define AERIAL_PERSPECTIVE_LUT_ATMOSPHERE_ARGS       0
#define AERIAL_PERSPECTIVE_LUT_TRANSMITTANCE_INPUT   1
#define AERIAL_PERSPECTIVE_LUT_MULTISCATTERING_INPUT 2
#define AERIAL_PERSPECTIVE_LUT_SAMPLER               3
#define AERIAL_PERSPECTIVE_LUT_OUTPUT                4
// Opaque scene TLAS, for the sun-occlusion trace along the marched column. Null for the first
// frames of a scene; AtmosphereArgs::aerialPerspectiveSceneShadowRange is forced to 0 then.
#define AERIAL_PERSPECTIVE_LUT_ACCELERATION_STRUCTURE 5
