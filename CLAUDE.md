# resolume-scopes

Waveform, vectorscope, histogram and picture assist as one FFGL effect for
Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the measurement path.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/sctest --out /tmp/frame.png`
- List parameters: `./build/sctest --list`

## Verify
- Everything: `tools/verify.sh`
- Do the scopes read right? `./build/sctest --verify`
- GPU vs C++: `./build/sctest --probe`
- No dead controls: `python3 tools/sweep.py`

## Notes
- **Matrix, Range and Unpremultiply are parameters, never heuristics.** None of
  the three is detectable from the pixels, and each one wrong is *plausibly*
  wrong rather than obviously so. Don't add detection.
- **No colorimetry constant belongs in GLSL.** Coefficients and the range
  mapping are uniforms from `Colorimetry.cpp`; the graticule is built from the
  same module, which is what makes "the trace lands in the box" a real test.
- The SDK's `Scoped*` bindings **clear to 0 rather than restoring**, so
  allocating an FBO unbinds your input texture — correct on every frame except
  the one that reallocates. `ScopeBuffer::Ensure` saves and restores it.
- `gl_PointSize` does nothing without `GL_PROGRAM_POINT_SIZE` in a desktop core
  profile, and drivers round it to an integer.
- Histogram bins **must** be RGBA32F (half-float stops counting at the tallest
  bins); the trace buffer is RGBA16F and should stay that way.
- Scatter passes use `texelFetch` (NEAREST, ignores the host's sampler state,
  sidesteps MaxUV). The picture pass filters, correctly.
- All host parameters are 0..1 and mapped in `Layout.cpp`. `SetParamInfo` clamps
  a standard default into 0..1 before `SetParamRange` can widen it.
- `scopes_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- `flat`, `active`, `input`, `output`, `sample` are GLSL reserved words. Shader
  errors surface only at runtime, in the diagnostics log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It exists for the one failure that actually
happens: a shader that will not compile, which otherwise looks like "the effect
does nothing" with no message anywhere. It names the stage and logs the GL
vendor/renderer/version next to it.
