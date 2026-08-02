#pragma once

#include <array>
#include <vector>

/**
    The measurement core.

    Every number the scopes plot, and every line of every graticule they draw,
    comes through this file. Nothing here touches OpenGL, so all of it is
    reachable from `sctest` and none of it can be checked only by looking at a
    picture.

    ---------------------------------------------------------------------------
    The thing to understand before changing any of it
    ---------------------------------------------------------------------------

    A scope in Resolume never sees a video signal. It sees whatever RGB the
    engine has in the texture by the time the effect runs, and that has already
    been through decisions nobody records in the pixels:

      - which matrix took the clip from Y'CbCr to RGB (BT.601, BT.709, BT.2020),
      - whether studio-range levels (16-235) were expanded to full range,
      - whether the colour has been multiplied by alpha.

    Get any of them wrong and the scope is confidently, plausibly wrong rather
    than obviously broken. A BT.709 clip read as BT.601 throws every vectorscope
    target about 5.7 degrees off its box. Unexpanded studio levels put reference
    white at 92 IRE. A 50%-opacity white reads as 50 IRE. Each of those looks
    like a mildly misadjusted picture, which is exactly what makes it dangerous.

    So all three are explicit parameters on the plugin and none of them is
    guessed from the pixels, because none of them can be.
*/
namespace scopes
{

// ---------------------------------------------------------------------------
// Matrix
// ---------------------------------------------------------------------------

enum class Matrix
{
	BT601,
	BT709,
	BT2020
};

/**
    Luma coefficients.

    `kg` is derived rather than written down. The three have to sum to exactly
    1, and a transcribed kg is the classic place for that to quietly stop being
    true.
*/
struct LumaCoefficients
{
	double kr;
	double kg;
	double kb;
};

LumaCoefficients coefficients( Matrix matrix );

const char* matrixName( Matrix matrix );

struct YCbCr
{
	double y;  ///< 0 is black, 1 is nominal white.
	double cb; ///< -0.5 .. +0.5, zero at neutral.
	double cr;
};

/**
    R'G'B' (non-linear, 0..1) to Y'CbCr, in the non-constant-luminance form every
    video matrix we care about uses: luma is a weighted sum of the *gamma-encoded*
    primaries, and the colour differences are normalised so a full-amplitude
    primary swing lands at +/- 0.5.
*/
YCbCr rgbToYCbCr( double r, double g, double b, Matrix matrix );

/**
    Y'CbCr back to R'G'B'.

    Green is recovered from the luma equation rather than from a second set of
    constants, so it cannot disagree with rgbToYCbCr.
*/
std::array< double, 3 > yCbCrToRgb( double y, double cb, double cr, Matrix matrix );

/// The hot path: waveform luma, zebras, false colour.
double luma( double r, double g, double b, Matrix matrix );

// ---------------------------------------------------------------------------
// Levels
// ---------------------------------------------------------------------------

enum class Range
{
	Full,   ///< The engine already expanded studio levels, or the clip was full range.
	Limited ///< 16-235 arrived untouched and has to be undone here.
};

/// 8-bit studio-range anchors. Identical in BT.601, BT.709 and BT.2020.
constexpr double kStudioBlack8Bit = 16.0;
constexpr double kStudioWhite8Bit = 235.0;
constexpr double kStudioLumaSwing = kStudioWhite8Bit - kStudioBlack8Bit;//219

/**
    The received-to-signal mapping, as the affine pair `(scale, offset)` such
    that `signal = received * scale + offset`.

    Returned as a pair rather than applied here because the GPU needs exactly the
    same mapping and must not have its own copy of it: this goes to the shaders
    as a `vec2` uniform. Recovering the affine form as `f(1) - f(0)` and `f(0)`
    is what stops the two from drifting.

    Identity for full range. For limited range it undoes 16-235, and it
    deliberately does not clamp -- values outside 0..1 are super-white and
    sub-black, and hiding them defeats the point of owning a scope.
*/
struct AffineMapping
{
	double scale;
	double offset;
};

AffineMapping receivedToSignal( Range range );

/// Signal level to IRE. Black is 0 IRE and nominal white is 100 IRE by definition.
inline double signalToIre( double signal )
{
	return signal * 100.0;
}

inline double ireToSignal( double ire )
{
	return ire / 100.0;
}

/**
    The waveform's vertical extent, in IRE. Wider than 0..100 on purpose:
    Resolume will happily hand an effect values above 1.0 once anything upstream
    has boosted them, and a scope that crops at 100 cannot show you that.
*/
constexpr double kWaveformIreMin = -10.0;
constexpr double kWaveformIreMax = 110.0;

/// Graticule lines for the waveform, in IRE. 0 and 100 are drawn brighter.
const std::vector< double >& waveformGraticuleIre();

// ---------------------------------------------------------------------------
// Vectorscope
// ---------------------------------------------------------------------------

enum class BarColour
{
	Red,
	Magenta,
	Blue,
	Cyan,
	Green,
	Yellow
};

struct VectorTarget
{
	BarColour colour;
	double cb;
	double cr;
	/// Degrees anticlockwise from the +Cb axis -- the convention a vectorscope graticule uses.
	double angleDeg;
	/// Distance from the centre, in the same units as cb/cr.
	double magnitude;
};

/**
    The six colour-bar boxes, computed from the matrix rather than tabulated.

    `amplitude` is the bar amplitude as a fraction: 0.75 for 75% bars, 1.0 for
    100%. Because the targets fall out of the same matrix the shader plots with,
    changing Matrix moves the graticule and the trace together. That is the
    honest behaviour -- picking the wrong matrix must not silently make a
    mismatched signal look correct.
*/
std::vector< VectorTarget > vectorTargets( Matrix matrix, double amplitude = 0.75 );

/**
    The plotted radius at full scale. A saturated primary reaches |Cb| or |Cr| of
    exactly 0.5, so 0.5 is the natural unit circle; 75% bars then land at 0.375.
*/
constexpr double kVectorFullScale = 0.5;

/**
    The I axis, in degrees on the same anticlockwise-from-+Cb convention.

    Not a constant: it is the direction of the skin-tone line, which is where a
    real vectorscope's operators actually look, and it is defined as the axis
    the NTSC I signal lay along -- so in a BT.709 world it has to be recomputed
    from the matrix like everything else. Derived here from the colour that
    defines it rather than from the traditional "123 degrees", which is a BT.601
    number.
*/
double skinToneAxisDeg( Matrix matrix );

// ---------------------------------------------------------------------------
// False colour
// ---------------------------------------------------------------------------

struct FalseColourBand
{
	/// Inclusive lower bound in IRE. The band runs up to the next band's fromIre.
	double fromIre;
	std::array< float, 3 > colour;
	const char* label;
};

/**
    The default false-colour scale.

    These are *our* bands, chosen to be legible and to put the boundaries where
    they are useful: clipping, near-clip, the middle of the range, crush. They
    are deliberately not presented as a reproduction of any camera
    manufacturer's scale. ARRI's, RED's and Blackmagic's all differ from each
    other and are tied to their own log curves, and copying the colours without
    the curve produces something that looks authoritative and means nothing.
*/
const std::vector< FalseColourBand >& falseColourBands();

/// Resolves an IRE value to its band. Exists so the GPU's loop can be measured
/// against something on the CPU -- see `sctest --probe`.
const FalseColourBand& falseColourBandFor( double ire );

} // namespace scopes
