# NTSC/VHS composite post-process

The NTSC/VHS effect is a display-space stack member. It runs after tonemapping
and before the final sRGB conversion/dither pass, so it can be reordered with
other display-space effects without crossing the HDR-to-display boundary.

The implementation follows the tape path used by
[Kim2091/ntsc-simulator](https://github.com/Kim2091/ntsc-simulator), adapted to
a normal rendered image rather than a sampled 4x-subcarrier waveform:

1. sRGB/YIQ luma and color-under bandwidth reduction with playback ringing;
2. worn-head luma smear and luminance-dependent tape noise;
3. sparse tape dropouts with previous-line compensation;
4. causal luma-only tape trail and conversion back to linear RGB.

The effect uses four compute passes because dropout compensation must read the
already smeared/noisy result. It is disabled by default:

```ini
rtx.ntsc.ntscEnable = True
```

## Time and randomness

Every random stream in the tape model - head-smear envelope phases, luma noise,
and dropout placement - is keyed by two integer counters that `dispatchNtsc`
derives from `GlobalTime`: an NTSC frame counter at 29.97 Hz and a field counter
at 59.94 Hz. Those counters are the only notion of time the shader has, and they
are mixed with the integer `pcg3d`/`pcg4d` hashes rather than `frac(sin(dot()))`.

Both halves of that matter. A float frame number pushed through a sine-based
hash loses its low-order variation once the argument is a few hundred thousand
radians, which froze the entire tape model a couple of minutes into a session;
frozen uniforms also collapse the Box-Muller luma noise into a constant, so it
read as a brightness shift with no grain. Keying off wall clock rather than the
render frame index also keeps the look frame-rate independent, as a real tape
would be, and keeps it working when `rtx.rngSeedWithFrameIndex` is disabled.

The main controls are `ntscLumaBW`, `ntscColorBW`, `ntscRinging`,
`ntscLumaNoise`, `ntscTapeDropoutRate`, `ntscTapeDropoutLength`,
`ntscHeadSmear`, and `ntscTapeTrail` in the `rtx.ntsc` category. The defaults
use a 3 MHz luma path, 425 kHz color-under path, moderate ringing/noise, and
the simulator's 15 microsecond average dropout length.
