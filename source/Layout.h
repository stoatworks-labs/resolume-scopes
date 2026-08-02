#pragma once

#include "Colorimetry.h"

/**
    Where every host parameter turns into a physical quantity, and where the
    scope's rectangle on the output is worked out.

    All of this is on the CPU and none of it touches GL, so `sctest` can ask the
    same questions the plugin does.

    ---------------------------------------------------------------------------
    Why every parameter is a plain 0..1 float
    ---------------------------------------------------------------------------

    `SetParamRange` exists and Resolume honours it, but
    `CFFGLPluginManager::SetParamInfo` clamps an `FF_TYPE_STANDARD` default into
    0..1 *before* a range can be attached -- the range call looks the parameter
    up by ID, so the parameter has to exist first, and there is no
    `SetParamDefault` (SDK b1afaf9). A parameter declared in IRE therefore cannot
    declare a default in IRE: 100 silently becomes 1.

    So the host side is uniformly 0..1 and every conversion to IRE, to a
    magnification, to a sample stride lives here.
*/
namespace scopes
{

// ---------------------------------------------------------------------------
// The scope selector, and the per-scope mode selectors
// ---------------------------------------------------------------------------

enum class Scope
{
	Waveform,
	Vectorscope,
	Histogram,
	PictureAssist
};

enum class WaveformMode
{
	Luma,
	RgbParade,
	YCbCrParade
};

enum class HistogramMode
{
	Rgb,
	Luma,
	RgbAndLuma
};

enum class AssistMode
{
	FalseColour,
	Zebra,
	FocusPeaking
};

enum class LayoutMode
{
	ScopeOnly,
	Overlay
};

enum class Quality
{
	Fast,
	Good,
	Full
};

/// Option parameters arrive as the element value the host was given, which is
/// the index as a float. Rounded rather than truncated: a host is entitled to
/// hand back 0.9999999 for element 1.
template< typename E >
E optionFrom( float value, int count )
{
	int index = static_cast< int >( value + 0.5f );
	if( index < 0 )
		index = 0;
	if( index >= count )
		index = count - 1;
	return static_cast< E >( index );
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

/**
    How many source pixels get measured, as a grid of sample cells.

    A scope is a scatter plot with one point per source pixel per channel, and a
    parade draws three of them. At UHD that is 25 million points a frame, which
    no GPU is going to do at 60 Hz alongside whatever else the composition is
    running -- so there is a sample budget, and above it the stride grows.

    That is a real limitation and worth stating plainly rather than hiding: at
    Full on a 4K source the scope measures every second pixel, not every pixel.
    For a waveform or a histogram this is invisible (both are summarising
    millions of samples into a few hundred columns either way); the one place it
    can matter is a single stray hot pixel, which a strided scope can miss.
*/
constexpr int kMaxSamples = 1920 * 1080;

struct SampleGrid
{
	int columns;
	int rows;

	int count() const
	{
		return columns * rows;
	}
};

SampleGrid sampleGridFor( int pictureWidth, int pictureHeight, Quality quality );

// ---------------------------------------------------------------------------
// Continuous parameters
// ---------------------------------------------------------------------------

/**
    Trace density.

    The trace is drawn additively, so this is not a brightness in the ordinary
    sense: it says how much one sample contributes, and therefore how many
    coincident samples it takes to saturate. Scaled by the sample count so that
    changing Quality does not change how bright the trace looks -- otherwise
    every quality change would need the density re-dialled, which would make the
    control useless as a measurement aid.
*/
float traceGain( float param, int sampleCount );

/// Vectorscope magnification. 1x at the bottom of the range, 8x at the top;
/// 2x and 5x, the settings a hardware scope offers, land inside it.
double vectorZoom( float param );

/// Zebra threshold in IRE. 50 to 110, so both the 70-ish skin-tone check and a
/// clipping check are reachable.
double zebraIre( float param );

/// Focus-peaking threshold, as a Sobel gradient magnitude on signal-level luma.
double peakThreshold( float param );

/// Histogram bin count. Fixed rather than exposed: 256 matches 8-bit source
/// data, and a bin count that is not a divisor of the source quantisation
/// produces a comb pattern that looks like a signal problem.
constexpr int kHistogramBins = 256;

/**
    Scales raw bin counts to 0..1 of the tile height.

    Expressed so that a perfectly flat histogram reaches `gain` -- gain of 1
    exactly fills the tile. That makes the control mean something absolute
    rather than being a knob you turn until it looks right, and it makes the
    height independent of Quality and of the source resolution.
*/
float histogramNorm( float param, int sampleCount, int bins );

/// Which of the four accumulated channels a histogram mode actually needs.
/// R, G, B, luma are channels 0..3, so luma alone is a base of 3 and a count of
/// 1 -- the accumulation pass then costs a quarter of what drawing all four
/// would.
struct HistogramChannels
{
	int base;
	int count;
};

HistogramChannels histogramChannels( HistogramMode mode );

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

/// A rectangle on the output, in 0..1 with the origin at the bottom left --
/// the same orientation as GL's NDC, so the conversion at the end is one
/// multiply-add rather than a flip nobody remembers.
struct Rect
{
	float x;
	float y;
	float width;
	float height;
};

/**
    Where the scope goes.

    In Scope Only the scope is the whole frame and Size, X and Y do nothing --
    stated here rather than left for the operator to discover by dragging.

    In Overlay, Size is the fraction of the *shorter* output dimension the scope
    spans, so the same setting gives a similar-looking overlay whether the
    composition is 16:9 or 1:1, and X/Y position it with 0 at one edge and 1 at
    the other -- meaning the overlay is always fully on screen and cannot be
    dragged into nothing.
*/
Rect scopeRect( LayoutMode layout, float size, float x, float y, float outputAspect );

} // namespace scopes
