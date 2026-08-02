#include "Graticule.h"

#include <cmath>

namespace scopes
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

/// The two weights the graticule speaks in. There is no text on it, so a line's
/// brightness is the only thing that can say whether it is a reference or a
/// ruling.
constexpr float kReference = 1.0f;
constexpr float kRuling    = 0.35f;

void segment( LineVertices& out, double x0, double y0, double x1, double y1, float brightness )
{
	out.push_back( static_cast< float >( x0 ) );
	out.push_back( static_cast< float >( y0 ) );
	out.push_back( brightness );
	out.push_back( static_cast< float >( x1 ) );
	out.push_back( static_cast< float >( y1 ) );
	out.push_back( brightness );
}

/// Aspect correction, kept identical to the vectorscope vertex shader. If these
/// two ever disagree the trace slides under a stationary graticule -- which
/// reads as a badly calibrated scope rather than as a bug.
void applyAspect( double& x, double& y, double aspect )
{
	if( aspect > 1.0 )
		x /= aspect;
	else
		y *= aspect;
}

void circle( LineVertices& out, double radius, double aspect, float brightness, int steps = 128 )
{
	double px = 0.0;
	double py = 0.0;
	for( int i = 0; i <= steps; ++i )
	{
		const double theta = 2.0 * kPi * static_cast< double >( i ) / static_cast< double >( steps );
		double x           = radius * std::cos( theta );
		double y           = radius * std::sin( theta );
		applyAspect( x, y, aspect );

		if( i > 0 )
			segment( out, px, py, x, y, brightness );
		px = x;
		py = y;
	}
}

/// A target box, drawn in the aspect-corrected space so it stays square on
/// screen rather than being stretched with the plot.
void box( LineVertices& out, double cx, double cy, double half, double aspect, float brightness )
{
	double hx = half;
	double hy = half;
	if( aspect > 1.0 )
		hx /= aspect;
	else
		hy *= aspect;

	segment( out, cx - hx, cy - hy, cx + hx, cy - hy, brightness );
	segment( out, cx + hx, cy - hy, cx + hx, cy + hy, brightness );
	segment( out, cx + hx, cy + hy, cx - hx, cy + hy, brightness );
	segment( out, cx - hx, cy + hy, cx - hx, cy - hy, brightness );
}

/// IRE to the tile's NDC, using the same range the waveform vertex shader maps
/// with.
double ireToNdc( double ire )
{
	const double t = ( ire - kWaveformIreMin ) / ( kWaveformIreMax - kWaveformIreMin );
	return t * 2.0 - 1.0;
}
} // namespace

// ---------------------------------------------------------------------------
// Waveform
// ---------------------------------------------------------------------------

LineVertices waveformGraticule( WaveformMode mode )
{
	LineVertices out;

	for( double ire : waveformGraticuleIre() )
	{
		// 0 and 100 IRE are what the operator actually reads against; the rest
		// are there to make a level estimable, not to be looked at.
		const bool isReference = ( ire == 0.0 || ire == 100.0 );
		const double y         = ireToNdc( ire );
		segment( out, -1.0, y, 1.0, y, isReference ? kReference : kRuling );
	}

	if( mode != WaveformMode::Luma )
	{
		// Dividers between the three panels of a parade.
		for( int i = 1; i < 3; ++i )
		{
			const double x = static_cast< double >( i ) / 3.0 * 2.0 - 1.0;
			segment( out, x, -1.0, x, 1.0, kReference );
		}
	}

	if( mode == WaveformMode::YCbCrParade )
	{
		// The chroma panels are bipolar and centred on 50 IRE, so on those two
		// the centre line means chroma zero -- a different thing from the 50 IRE
		// ruling it coincides with. Drawn at reference weight across the two
		// right-hand panels only, which is the only way to say so without text.
		const double y = ireToNdc( 50.0 );
		segment( out, -1.0 / 3.0, y, 1.0, y, kReference );
	}

	return out;
}

// ---------------------------------------------------------------------------
// Vectorscope
// ---------------------------------------------------------------------------

LineVertices vectorscopeGraticule( Matrix matrix, double amplitude, double zoom, double aspect )
{
	LineVertices out;

	// The ring the targets sit on. At 75% bars that is the 75% circle, so it
	// lands on the boxes rather than somewhere they can be compared against by
	// eye and found not to match.
	circle( out, amplitude * zoom, aspect, kRuling );

	// Full scale: a saturated primary. Anything outside this ring is chroma the
	// signal has no business containing.
	circle( out, 1.0 * zoom, aspect, kReference );

	// Axes.
	{
		const double r = 1.05 * zoom;
		double x0 = -r, y0 = 0.0, x1 = r, y1 = 0.0;
		applyAspect( x0, y0, aspect );
		applyAspect( x1, y1, aspect );
		segment( out, x0, y0, x1, y1, kRuling );

		double x2 = 0.0, y2 = -r, x3 = 0.0, y3 = r;
		applyAspect( x2, y2, aspect );
		applyAspect( x3, y3, aspect );
		segment( out, x2, y2, x3, y3, kRuling );
	}

	// The skin-tone line, at whatever angle this matrix puts the NTSC I axis.
	{
		const double deg = skinToneAxisDeg( matrix );
		const double rad = deg * kPi / 180.0;
		double x         = std::cos( rad ) * zoom;
		double y         = std::sin( rad ) * zoom;
		applyAspect( x, y, aspect );
		segment( out, 0.0, 0.0, x, y, kReference );
	}

	// The six boxes, from the same matrix the shader plots with.
	for( const VectorTarget& target : vectorTargets( matrix, amplitude ) )
	{
		double x = target.cb / kVectorFullScale * zoom;
		double y = target.cr / kVectorFullScale * zoom;
		applyAspect( x, y, aspect );

		// A real scope's boxes are its stated tolerance. This one has no
		// calibration to state, so the box is simply a legible target -- 2.5% of
		// full scale, which is about the width of the trace at normal density.
		box( out, x, y, 0.025 * zoom, aspect, kReference );
	}

	return out;
}

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

LineVertices histogramGraticule()
{
	LineVertices out;

	for( int i = 0; i <= 4; ++i )
	{
		const double t = static_cast< double >( i ) / 4.0;
		const double x = t * 2.0 - 1.0;
		// The ends are black and nominal white; the quarters are only a ruler.
		const bool isReference = ( i == 0 || i == 4 );
		segment( out, x, -1.0, x, 1.0, isReference ? kReference : kRuling );
	}

	return out;
}

} // namespace scopes
