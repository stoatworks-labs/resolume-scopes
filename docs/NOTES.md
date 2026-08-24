# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*resolume-scopes — waveform/vectorscope/histogram/picture-assist as one FFGL effect; PUBLIC MIT v0.1.0, video and website page live, never loaded into Resolume*

**resolume-scopes** (created 2026-08-02, `~/Projects/resolume-scopes`, **PUBLIC MIT at
`stoatworks-labs/resolume-scopes`, released v0.1.0**) — waveform (luma / RGB parade /
Y Cb Cr parade), vectorscope, histogram and picture assist (false colour / zebra / focus
peaking) as a **single** FFGL effect with a Scope dropdown. Layout switches between
replacing the picture and overlaying it. 19 parameters in 5 groups.

Fifth FFGL plugin, sharing the build and harness shape of [porthole](https://github.com/stoatworks-labs/porthole/blob/main/docs/NOTES.md) (`porthole`) /
[old cathode](https://github.com/stoatworks-labs/old-cathode/blob/main/docs/NOTES.md) (`old-cathode`) / [resolume luma keyer](https://github.com/stoatworks-labs/resolume-luma-keyer/blob/main/docs/NOTES.md) (`resolume-luma-keyer`). The scope maths is a **port of
[atem scopes](https://github.com/stoatworks-labs/atem-scopes/blob/main/docs/NOTES.md) (`atem-scopes`)** — same measurement core, C++ instead of TypeScript, desktop GL
4.1 core instead of WebGL2.

All release homes agree as of 2026-08-03: video `5fIK9vonhas` (23rd in the series),
website `/software/resolume-scopes/` + the Resolume suite page, downloads generated,
`gen-downloads.py --check` clean.

## The design rule

**Matrix, Range and Unpremultiply are parameters, never heuristics.** By the time an FFGL
effect sees the texture, Resolume's engine has already chosen a matrix, decided about
studio levels, and premultiplied by alpha — and recorded none of it. Each one wrong is
*plausibly* wrong: 709-as-601 throws vectorscope targets 5.7° off, unexpanded studio
levels put white at 92 IRE, a 50%-opacity layer reads a stop down. See
[video scope traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_video_scope_traps.md).

No colorimetry constant is in the GLSL; coefficients and the range mapping are uniforms
from `Colorimetry.cpp`, and `Graticule.cpp` builds the boxes from the same module — which
is what makes "the trace lands in the box" a real check rather than a tautology.

## Verified vs not — be precise

**Verified** by `tools/verify.sh` on one M4 Max: waveform lands on each bar's luma to
within **0.08 IRE** (readback row quantisation, not maths), vectorscope within **1.2 px**
of each derived target, histogram in the right bin and no other — across 75%/100% bars in
BT.601/709/2020. GLSL false-colour banding matches the C++ on every ramp column in all 3
matrices × both ranges. All 19 parameters measurably affect output. Bundle is universal +
exports `plugMain`.

**Not verified**: **never loaded into Resolume** — so whether the input really is
premultiplied is unconfirmed, and that is the one assumption the offline harness
structurally cannot test. Windows built and shipped but never run. No performance figure.

## Traps worth remembering

- **The SDK's `Scoped*` bindings clear rather than restore**, so allocating an FBO
  unbinds the input texture — correct on every frame *except* the one that reallocates.
  Cost the first hour. Now in [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md) along with the desktop-GL
  `gl_PointSize` / `GL_PROGRAM_POINT_SIZE` difference.
- **Histogram gain is calibrated against real pictures, not the arithmetic.** A bin's
  height is (share of samples) × bins × gain, so a *flat* distribution reaches exactly
  `gain`. Reading "flat fills the tile" as the target and defaulting to 4 put every real
  picture ~30× over the top and drew a solid white wall. Real footage puts 2–5% of pixels
  in its busiest bin against a flat one's 0.4%, so useful gains are well **below** 1.
- **Expectations must use the *quantised* picture.** 8-bit 75% white is 191/255 = 74.90
  IRE, not 75.00. Comparing to nominal blames the plugin for the picture's quantisation;
  on a 256-bin histogram it is the difference between the right bin and its neighbour.
- **`--probe` must render 1:1.** The picture pass filters (correctly — nothing is measured
  off it), so at any other scale a pixel is a blend of two texels and the probe compares
  the band logic against the resampler. Produced one spurious mismatch at a band boundary.
- **`sweep.py` must use a 2D gradient, not bars.** Bars are flat down every column, so the
  trace saturates and Intensity *and* Quality both read dead while working perfectly.
  Unpremultiply is correctly dead on an opaque source — sweep it against `--alpha 0.5`.
