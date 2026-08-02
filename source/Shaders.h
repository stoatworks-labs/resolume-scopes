#pragma once

#include <string>

/**
    Every shader the plugin uses.

    ---------------------------------------------------------------------------
    The rule that matters here
    ---------------------------------------------------------------------------

    **No colorimetry constant is written down in GLSL.** The luma coefficients
    and the received-to-signal mapping arrive as uniforms computed by
    `Colorimetry.cpp`, and the graticule is built from that same module. Copying
    `0.2126` into a shader is precisely how a graticule and the trace it is meant
    to measure end up disagreeing by a fraction of a degree that nobody notices
    until a client does.

    The shared prelude is prepended to every program that measures anything, so
    `toSignal`, `lumaOf` and `chromaOf` mean one thing everywhere.

    ---------------------------------------------------------------------------
    Two things that differ from the same shaders in WebGL
    ---------------------------------------------------------------------------

    **The scatter passes use `texelFetch`, not `texture`.** A scope has to sample
    NEAREST -- a linear tap blends neighbours and drags every reading toward the
    local mean, so clipped whites quietly stop reading as clipped. The usual way
    to guarantee that is `glTexParameteri` on the texture, but this texture
    belongs to Resolume and is about to be used for other things, so changing its
    filter state is not ours to do. `texelFetch` ignores sampler state
    altogether. It also sidesteps `MaxUV`: texels `0 .. PictureSize-1` are the
    picture, whatever padding sits beyond them.

    **`gl_PointSize` does nothing unless `GL_PROGRAM_POINT_SIZE` is enabled.** In
    GL ES a vertex shader may always write it; in a desktop core profile the
    write is ignored and the fixed `glPointSize` value is used instead. The
    symptom is a trace that is numerically perfect and looks blank. `Scopes.cpp`
    enables it.

    Sources are assembled once, on first use, and returned by reference. They are
    functions rather than namespace-scope strings so that nothing depends on the
    initialisation order of two translation units.
*/
namespace scopes
{

/// Matches the loop bound in the false-colour shader. A GLSL uniform array has
/// to have a fixed size, so this is the ceiling on how many bands there can be.
constexpr int kMaxFalseColourBands = 16;

//Scatter passes: one GL_POINT per source sample per channel, drawn additively.
const std::string& waveformVertex();
const std::string& vectorscopeVertex();
const std::string& traceFragment();

//Histogram: accumulate into a bins x 1 float target, then draw the bins.
const std::string& histogramAccumVertex();
const std::string& histogramAccumFragment();
const std::string& histogramDrawVertex();
const std::string& histogramDrawFragment();

//The picture, with the exposure and focus overlays on it.
const std::string& pictureVertex();
const std::string& pictureFragment();

//Graticules, drawn as line primitives from CPU-built geometry.
const std::string& lineVertex();
const std::string& lineFragment();

//Putting the finished scope onto the output.
const std::string& compositeVertex();
const std::string& compositeFragment();

} // namespace scopes
