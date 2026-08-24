# AGENTS.md — bringing an LLM up to speed on Scopes

Orientation for an AI assistant (or a new human) picking this project up cold.
`CLAUDE.md` holds the short command reference; this file explains the model and
the traps.

---

## 1. What this is

Waveform, vectorscope, histogram and picture assist as one FFGL effect for
Resolume Arena / Avenue, built on the official Resolume FFGL SDK (pinned at
`b1afaf9`). C++/GLSL, CMake, public MIT.

The one idea to internalise before changing anything:

> **We never see a video signal. We see whatever RGB Resolume's engine happens
> to have in the texture, and it has already made three decisions it does not
> record anywhere in the pixels.**

Those three are the matrix (BT.601/709/2020), the range (full or 16–235), and
whether the colour has been premultiplied by alpha. Each of them changes the
reading, none is detectable, and each failure is *plausible* rather than
obvious:

| got wrong | what it looks like |
|---|---|
| 709 read as 601 | every vectorscope target ~5.7° off its box |
| studio levels not expanded | reference white at 92 IRE |
| premultiplied colour measured raw | a 50%-opacity layer reads a stop down |

So all three are **explicit parameters and never heuristics**. Do not add
detection. There is nothing to detect from.

## 2. The shape of it

```
source/Colorimetry.{h,cpp}  the measurement core: matrices, levels, vector
                            targets, false-colour bands. Read this first.
source/Layout.{h,cpp}       every 0..1 host parameter -> a physical quantity,
                            plus where the scope's rectangle goes
source/Graticule.{h,cpp}    graticule geometry, *derived* from Colorimetry
source/Shaders.{h,cpp}      the GLSL
source/ScopeBuffer.{h,cpp}  an FBO that does not leak and does not clobber
source/Scopes.{h,cpp}       FFGL host glue, parameters, the render path
source/Diag.{h,cpp}         a log file, for the shader that will not compile
tools/sctest/               headless render and measurement harness
tools/sweep.py              no control is silently dead
tools/verify.sh             all of the above, in one go
```

**No colorimetry constant is written down in GLSL.** Coefficients and the
received-to-signal mapping arrive as uniforms from `Colorimetry.cpp`, and the
graticule is built from the same module. That is what makes "the trace must land
in the box" a real test rather than a tautology: both sides come from one
derivation, so if they disagree, something in between is broken.

Everything except Picture Assist renders in two stages — scope into its own
buffer, then composited onto the output. Two stages because the trace
accumulates additively in floating point and has to be clamped **once, at the
end**; clamping as it accumulates would stop it building up at all, and that
accumulation is the entire mechanism by which a scatter plot becomes a scope.

Picture Assist has no scope buffer. It is the picture with a different fragment
shader on it, which is why Layout, Size, X, Y and Opacity correctly do nothing
while it is selected.

## 3. Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Add `-DCMAKE_OSX_ARCHITECTURES=arm64` for a much faster dev build.

## 4. Traps

### The SDK's "Scoped" bindings clear rather than restore

This one cost the first hour, and it is the most dangerous kind of bug because
it is *nearly* invisible.

`ffglex::ScopedTextureBinding::EndScope()` binds **0** on scope exit — it does
not restore what was bound before. `FFGLFBO::Initialise` sizes its new colour
texture under one of those. So **allocating a buffer silently unbinds your input
texture from the active unit.**

The symptom: the plugin is correct on every frame *except* the one where a
buffer is actually allocated. The scope reads zero for one frame after loading,
and one frame each time an operator's drag on Size changes the buffer size. A
scope that is right except during a drag is one nobody reports and everybody
half-trusts. `ScopeBuffer::Ensure` saves and restores the binding around
allocation; do not remove that.

The same applies to any other SDK helper called `Scoped*` — check what its
destructor actually does before relying on it.

### `gl_PointSize` is ignored without `GL_PROGRAM_POINT_SIZE`

In GL ES a vertex shader may always write `gl_PointSize`. In a desktop core
profile the write is **ignored** unless `GL_PROGRAM_POINT_SIZE` is enabled, and
the fixed `glPointSize` value is used instead. Every point is then one pixel.

That is worse than cosmetic on a vectorscope: every sample of a flat colour
lands on one coordinate, so an entire colour bar renders as a single pixel and
reads as *no trace at all* — while being numerically perfect.

Drivers also round the point size to an integer, so asking for 1.4 gets you 1
and closes no gaps. `Scopes.cpp` rounds up.

### The histogram bins must be RGBA32F, and the trace must not be

A histogram accumulated in half-float **silently stops counting**: ~11 mantissa
bits means adding 1.0 to a bin holding 4096 rounds straight back to 4096. The
bins that stall first are the tallest — which are exactly the clipped whites and
crushed blacks the histogram was opened to find.

The *trace* buffer is RGBA16F and that is fine and deliberate: it is a picture,
not a tally. Do not "fix" it to 32F, and do not "optimise" the bins to 16F.

### Scatter passes use `texelFetch`, not `texture`

A scope has to sample NEAREST — a linear tap blends neighbours and drags every
reading toward the local mean, so clipped whites stop reading as clipped. The
usual way to force that is `glTexParameteri`, but the input texture belongs to
Resolume and is about to be used for other things. `texelFetch` ignores sampler
state entirely, and as a bonus sidesteps `MaxUV`: texels `0 .. PictureSize-1`
are the picture whatever padding lies beyond.

The *picture* pass does use `texture()` with filtering, correctly — it is
showing the picture, and nothing is measured off it.

### A ranged parameter cannot have a ranged default

`CFFGLPluginManager::SetParamInfo` clamps an `FF_TYPE_STANDARD` default into
0..1 **before** `SetParamRange` can be called (the range call looks the parameter
up by ID, so the parameter has to exist first), and there is no
`SetParamDefault`. A parameter declared in IRE therefore cannot declare a default
in IRE: 100 becomes 1.

Hence: **every continuous parameter here is a plain 0..1 float**, and the
conversion to IRE, magnifications and sample strides lives in `Layout.cpp`.

### A core profile will not draw without a VAO

The scatter passes read everything from `gl_VertexID` and source no vertex
attributes at all, but a core profile still refuses to draw with no vertex array
bound. `scatterVAO` exists solely for that and stays empty.

### The plugin registers itself from a static constructor

`CFFGLPluginInfo` is a file-scope object in `Scopes.cpp` that nothing references
by name. That is why `scopes_core` is an **OBJECT** library and not a **STATIC**
one: in an archive the linker is entitled to drop the whole translation unit, and
the result is a bundle that loads, exports `plugMain`, and reports that it
contains no plugins. Do not "tidy" it to STATIC.

```bash
nm -gU build/Scopes.bundle/Contents/MacOS/Scopes | grep _plugMain
lipo -archs build/Scopes.bundle/Contents/MacOS/Scopes
```

`CMAKE_OSX_ARCHITECTURES` must be set **before the first target is created**.
Set it later and CMake silently ignores it — an arm64-only binary the build log
calls a success, and an Intel Resolume that quietly fails to load it. Verify the
artefact, never the log.

### GLSL reserved words

`flat`, `active`, `filter`, `input`, `output`, `sample`, `common`, `partition`,
`resource` and a long tail. The failure mode is nasty: the shader fails to
compile at *runtime*, `InitGL` returns `FF_FAIL`, and Resolume shows an effect
that silently does nothing. That is what `source/Diag.cpp` is for.

## 5. Testing

```bash
tools/verify.sh          # everything below
```

### `--verify` — do the scopes report the right numbers?

Generates colour bars and asserts that the waveform trace lands on each bar's
luma, the vectorscope trace lands in each bar's box, and the histogram spikes in
each bar's bin. Swept over all three matrices and both bar amplitudes.

**The expectation must be computed from the quantised picture**, not the nominal
amplitude. The test texture is 8-bit, so 75% white is 191/255 = 74.90 IRE, not
75.00. Comparing against 75.00 measures the picture's quantisation and calls the
difference a plugin error; on the histogram, where a bin is one 255th wide, it is
the difference between the right bin and its neighbour. That is what
`quantised()` is for, and it caused a false failure while this was being written.

The histogram check also asserts a level **no bar contains is empty**. Without
that, the test passes just as well on a histogram that fills every column.

### `--probe` — the GPU against the C++

The false-colour band loop is the one piece of logic written twice, so the probe
runs a neutral ramp through it and compares every column against
`falseColourBandFor()`. It also exercises `toSignal` and `lumaOf` on the way.

**It renders 1:1 and refuses otherwise.** The picture pass filters, correctly, so
at any other scale an output pixel is a blend of two source texels and the probe
would be comparing the band logic against the resampler. That produced exactly
one spurious mismatch, at a band boundary, before it was pinned down.

### `sweep.py` — a dead control is invisible to the compiler

A uniform name that does not match between the C++ and the GLSL is silently
ignored: `glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented
no-op. A control can be stone dead while everything compiles, links, loads and
renders. `--verify` exercises four of the nineteen parameters; this covers the
rest.

Two things about the baseline that will fool you:

- **It must be a 2D gradient, not bars.** Colour bars are flat down every
  column, so all of a column's samples land on one waveform row and one
  vectorscope point, and the trace saturates whatever Intensity and Quality are
  set to. Against bars both read stone dead while working perfectly.
- **Unpremultiply is correctly dead against an opaque source**, so it is swept
  against a source the harness premultiplied at 50%.

## 6. What has never been checked

- **It has never been loaded into Resolume.** Not once. Parameter groups, the
  option dropdowns, Arena's real texture sizes and — the important one —
  **whether the input is actually premultiplied** are all unconfirmed. The
  harness supplies its own textures, so it cannot answer any of that.
- **The Windows build has never been run**, or compiled.
- **No performance figure has been taken.** A parade at Full quality on a 4K
  source scatters several million points a frame; nobody has measured the cost.
- Everything here comes from one M4 Max, never from CI — hosted macOS runners
  have no GPU, so `sctest` cannot run there.

## 6b. Driving Resolume, when you need to

Notes inherited from `old-cathode`, which did get as far as loading there.
`cmake --install` puts the bundle where Arena looks.

**Arena is accessible.** Its dialogs answer to `System Events` by button name.
The custom-drawn buttons inside a confirmation are *not* in the tree; get the
window's `position`/`size` and click a computed point, and check which button you
are aiming at first. "New Composition!" offers **New / Cancel / Save & New** with
**Save & New as the highlighted default**, so never dismiss one with Return.

**Arena has a REST API on `http://127.0.0.1:8080/api/v1`.** `GET .../effects` is
the honest answer to "did the plugin register".

**The trap:** `POST .../clips/{n}/open` **returns 200 and ignores the path you
sent it**, loading an unrelated file instead. Verify what actually loaded by
reading `fileinfo.path` back. There is **no endpoint for adding an effect** —
`POST .../effects` is 404 — so applying it is a UI job.

**Whatever you do, do not film or modify the operator's own composition.**

## 7. Conventions

- Public repo. "Commit" means commit **and** push.
- Standard AI disclaimer in the README — see the fleet's disclaimer scope.
- Sibling FFGL plugins share this build and harness shape: `porthole`,
  `old-cathode`, `resolume-luma-keyer`. The scope maths is a port of
  `atem-scopes`, which has the same measurement core in TypeScript.


## The browser demo

`demo/` is a static page at **resolume-scopes-demo.stoatworks-labs.com**: this
plugin's own GLSL, ported to WebGL2, running on clips generated in the page with
the parameters the constructor declares. It is deployed as a Cloudflare Worker
serving `demo/` as static assets (`wrangler.toml`), with **no build step** — what
is committed is what is served.

Three things about it are not visible from the files:

- **`demo/plugin.js` carries a second copy of the shader.** The demo cannot
  include a C++ file, so the GLSL from `source/Shaders.cpp` is duplicated there and
  *nothing enforces that they agree*. Change the shader and change both, or the
  page quietly goes on rendering the old maths.
- **`demo/vendor/` is vendored, not authored here.** The master is
  `stoatworks-backend/resolume-demo/`; fix it there and re-run its `sync.sh`.
  `sync.sh --check` reports drift. A fix applied to the copy fixes one plugin out
  of six.
- **Verify a deploy by content, never by status code.** A wrong page still
  answers 200.

```bash
cf-run npx wrangler deploy
curl -s 'https://resolume-scopes-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```

`Colorimetry`, `Graticule` and `Layout` are ported into
`demo/plugin.js` alongside the shaders, because the graticule being
built from the same module the shader plots with is the whole point of
the vectorscope. Port them together or not at all.

The page is emphatic that it is not the plugin, and lists what it does not
reproduce in a disclosure at the foot. Keep that: it is a port, so nothing on it
is evidence about the plugin, and the offline harness in this repository is
still the only thing that measures anything.

## Notes

`docs/NOTES.md` carries this repo's working notes — current status, decisions
already made, and the traps that have actually bitten. Read it before changing
anything non-obvious. Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
