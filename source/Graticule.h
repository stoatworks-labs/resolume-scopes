#pragma once

#include "Colorimetry.h"
#include "Layout.h"

#include <vector>

/**
    The graticules, built as line geometry on the CPU.

    Every one of them is *derived* from `Colorimetry.cpp` rather than transcribed
    from a picture of a real scope. That is the whole point of the file: the
    vectorscope's boxes come out of `vectorTargets()` with the matrix the shader
    is plotting with, so selecting BT.601 moves the boxes and the trace together
    and there is no combination of settings in which a mismatched signal is made
    to look correct.

    A hardware scope labels its graticule. This one cannot: there is no text in
    an FFGL plugin and no sane way to put any there, so the lines have to carry
    the meaning on their own. They do it by weight -- the lines an operator reads
    against (0 and 100 IRE, chroma zero, the 75% ring) are drawn at full
    brightness and everything else is drawn faint.
*/
namespace scopes
{

/// Interleaved x, y, brightness. Positions are in the scope tile's own NDC,
/// -1..1, and are consumed as GL_LINES -- so every pair of vertices is a
/// segment and the buffer length is always a multiple of two vertices.
using LineVertices = std::vector< float >;

/// Horizontal IRE lines, plus the segment dividers when a parade is showing.
LineVertices waveformGraticule( WaveformMode mode );

/**
    The vectorscope: the 100% ring, the target boxes, the axes and the skin-tone
    line.

    `zoom` and `aspect` have to be the same values the shader is given, because
    the graticule is scaled here rather than in the shader -- if they disagree,
    the trace moves under a stationary graticule, which looks exactly like a
    miscalibrated scope.
*/
LineVertices vectorscopeGraticule( Matrix matrix, double amplitude, double zoom, double aspect );

/// Vertical lines at 0, 25, 50, 75 and 100% of the bin range.
LineVertices histogramGraticule();

} // namespace scopes
