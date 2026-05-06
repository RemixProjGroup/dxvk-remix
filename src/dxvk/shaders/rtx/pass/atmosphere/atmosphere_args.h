/*
* Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#pragma once

#include "rtx/utility/shader_types.h"

// Maximum number of independent moons the atmosphere system can render.
// Bumping requires a corresponding bump in the shader unrolling budget —
// currently 4 fits comfortably in the constant buffer and unrolls cleanly.
#define MAX_MOONS 4u

// Per-moon parameters. Hand-padded to 16-byte alignment.
struct MoonParams {
  // Pose (game-driven via NoSave RTX_OPTIONs)
  vec3 direction;          // Normalized direction in Y-up space
  float angularRadius;     // Half-angle in radians

  vec3 color;              // Base albedo
  float brightness;        // Overall radiance multiplier

  uint surfaceStyle;       // 0 = Rocky, 1 = Volcanic
  float phase;             // [0,1]: 0=new, 0.5=full
  float enabled;           // 1.0 = render, 0.0 = skip
  float craterDensity;     // [0,1] multiplier on crater contribution

  float surfaceContrast;   // Multiplier on surface light/dark variation
  float surfaceNoiseScale; // Multiplier on UV scale fed into surface noise
  float darkSideBrightness;// Fraction of lit radiance applied on dark side
  float roughnessAmount;   // Multiplier on micro-detail amplitude
};

// Atmosphere parameters for Hillaire physically-based atmospheric scattering
struct AtmosphereArgs {
  vec3 sunDirection;
  float planetRadius;  // in km

  vec3 sunIlluminance;
  float atmosphereThickness;  // in km

  vec3 rayleighScattering;
  float mieAnisotropy;  // Henyey-Greenstein phase function g parameter [-1, 1]

  vec3 mieScattering;
  float sunRayBrightness;  // Multiplier for direct sun ray brightness

  // Ozone absorption (important for realistic sunset colors per Hillaire paper Section 3.4)
  vec3 ozoneAbsorption;  // Absorption coefficients (km^-1)
  float ozoneLayerAltitude;  // Peak altitude of ozone layer (km)

  uint transmittanceLutWidth;
  uint transmittanceLutHeight;
  uint multiscatteringLutSize;
  uint skyViewLutWidth;

  uint skyViewLutHeight;
  float ozoneLayerWidth;  // Width of ozone layer (km)
  float viewAltitude;     // Camera altitude offset (km)
  uint pad2;

  // Derived parameters (computed on CPU)
  float atmosphereRadius;  // planetRadius + atmosphereThickness
  float rayleighScaleHeight;  // exponential density falloff for Rayleigh (km)
  float mieScaleHeight;  // exponential density falloff for Mie (km)
  float sunAngularRadius; // Sun angular radius in radians

  // ----- Night-sky additions (fork) -----
  float starBrightness;     // Overall star brightness multiplier
  float starDensity;        // Density threshold (0=all stars, 1=no stars)
  float starTwinkleSpeed;   // Animation rate
  float nightSkyBrightness; // Airglow / ambient night-sky brightness

  vec3 nightSkyColor;       // Base color of night-sky airglow
  float timeSeconds;        // Elapsed time for star twinkle animation

  // Sidereal sky rotation (axis-angle representation).
  // Default elevation=90 / rotation=0 puts the celestial pole at zenith,
  // and starRotation=0 leaves the star sample direction unchanged — preserving
  // original at-the-pole behavior. Games push starRotation per frame; the axis
  // fields are persistent and set once at startup or via rtx.conf.
  float starRotation;       // Sidereal angle, degrees [0, 360]
  float starAxisElevation;  // Celestial pole elevation from horizon, degrees
  float starAxisRotation;   // Celestial pole azimuth, degrees
  float pad3;               // 16-byte alignment

  // ----- Per-moon parameters (fork) -----
  MoonParams moons[MAX_MOONS];

  // ----- Cloud parameters (fork: procedural FBM cloud layer at fixed altitude) -----
  vec3 cloudColor;          // Cloud base color (typically white)
  float cloudDensity;       // Overall opacity/density multiplier

  float cloudAltitude;      // Altitude of cloud layer (km)
  float cloudScale;         // Horizontal noise scale (smaller = larger clouds)
  float cloudEnabled;       // 1.0 if clouds should be rendered, 0.0 otherwise
  float cloudShadowStrength;// How strongly clouds dim ground/atmosphere lighting [0..1]

  vec2 cloudWindOffset;     // Accumulated wind-driven UV offset (km)
  float cloudAnisotropy;    // HG g for cloud sun forward-scatter (silver lining)
  float cloudCurvature;     // 0 = Earth-scale dome, 1 = tight dome

  // ----- Cloud volumetric / appearance enhancements (fork) -----
  vec3 cloudShadowTint;        // RGB sky-bounce tint on shadow side
  float cloudShadowTintStrength;

  float cloudThickness;        // Cloud-slab vertical depth, km
  float cloudDetailWeight;     // Pre-fade detail FBM weight [0..1]
  float cloudSunsetWarmth;     // Strength of low-sun warm tint
  uint cloudViewSamples;       // Ray-march steps through cloud slab

  // ----- Spatial variation fields (Nubis-style weather) -----
  float cloudTypeMean;             // [0,1] mean cloud type. 0=stratus, 0.5=stratocumulus, 1=cumulus.
  float cloudTypeSpread;           // [0,1] amplitude of type variation around mean.
  float cloudTypeNoiseScale;       // Region size frequency for type noise.
  float cloudCoverageMean;         // [0,1] mean coverage across the sky.

  float cloudCoverageSpread;       // [0,1] amplitude of coverage variation around mean.
  float cloudCoverageNoiseScale;   // Region size frequency for coverage noise (independent of type).
  float cloudAnvilBias;            // [0,1] cumulus top inflation strength (Nubis anvil pow trick).
  float pad4;                      // 16-byte alignment
};
