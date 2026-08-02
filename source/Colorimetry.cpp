#include "Colorimetry.h"

#include <cmath>

namespace scopes
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

/// Only kr and kb are written down. kg is derived, in coefficients().
struct MatrixConstants
{
	double kr;
	double kb;
};

MatrixConstants constantsFor( Matrix matrix )
{
	switch( matrix )
	{
	case Matrix::BT601:
		return { 0.299, 0.114 };
	case Matrix::BT2020:
		return { 0.2627, 0.0593 };
	case Matrix::BT709:
	default:
		return { 0.2126, 0.0722 };
	}
}

double degreesOf( double cb, double cr )
{
	double deg = std::atan2( cr, cb ) * 180.0 / kPi;
	if( deg < 0.0 )
		deg += 360.0;
	return deg;
}
} // namespace

LumaCoefficients coefficients( Matrix matrix )
{
	const MatrixConstants k = constantsFor( matrix );
	return { k.kr, 1.0 - k.kr - k.kb, k.kb };
}

const char* matrixName( Matrix matrix )
{
	switch( matrix )
	{
	case Matrix::BT601:
		return "BT.601 (SD)";
	case Matrix::BT2020:
		return "BT.2020 (UHD)";
	case Matrix::BT709:
	default:
		return "BT.709 (HD)";
	}
}

YCbCr rgbToYCbCr( double r, double g, double b, Matrix matrix )
{
	const LumaCoefficients k = coefficients( matrix );
	const double y          = k.kr * r + k.kg * g + k.kb * b;
	return {
		y,
		( b - y ) / ( 2.0 * ( 1.0 - k.kb ) ),
		( r - y ) / ( 2.0 * ( 1.0 - k.kr ) )
	};
}

std::array< double, 3 > yCbCrToRgb( double y, double cb, double cr, Matrix matrix )
{
	const LumaCoefficients k = coefficients( matrix );
	const double r          = y + cr * 2.0 * ( 1.0 - k.kr );
	const double b          = y + cb * 2.0 * ( 1.0 - k.kb );
	const double g          = ( y - k.kr * r - k.kb * b ) / k.kg;
	return { r, g, b };
}

double luma( double r, double g, double b, Matrix matrix )
{
	const LumaCoefficients k = coefficients( matrix );
	return k.kr * r + k.kg * g + k.kb * b;
}

// ---------------------------------------------------------------------------
// Levels
// ---------------------------------------------------------------------------

AffineMapping receivedToSignal( Range range )
{
	if( range == Range::Full )
		return { 1.0, 0.0 };

	// Recovered as (f(1) - f(0), f(0)) from the one place the anchors are
	// written down, rather than restated as a second pair of magic numbers.
	const auto f = []( double v ) {
		return ( v * 255.0 - kStudioBlack8Bit ) / kStudioLumaSwing;
	};
	return { f( 1.0 ) - f( 0.0 ), f( 0.0 ) };
}

const std::vector< double >& waveformGraticuleIre()
{
	static const std::vector< double > lines = {
		-10, 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110
	};
	return lines;
}

// ---------------------------------------------------------------------------
// Vectorscope
// ---------------------------------------------------------------------------

namespace
{
/// R'G'B' switch pattern for each bar, at unit amplitude.
std::array< double, 3 > barPrimary( BarColour colour )
{
	switch( colour )
	{
	case BarColour::Red:
		return { 1, 0, 0 };
	case BarColour::Magenta:
		return { 1, 0, 1 };
	case BarColour::Blue:
		return { 0, 0, 1 };
	case BarColour::Cyan:
		return { 0, 1, 1 };
	case BarColour::Green:
		return { 0, 1, 0 };
	case BarColour::Yellow:
	default:
		return { 1, 1, 0 };
	}
}
} // namespace

std::vector< VectorTarget > vectorTargets( Matrix matrix, double amplitude )
{
	static const BarColour order[] = {
		BarColour::Red, BarColour::Magenta, BarColour::Blue,
		BarColour::Cyan, BarColour::Green, BarColour::Yellow
	};

	std::vector< VectorTarget > targets;
	targets.reserve( 6 );
	for( BarColour colour : order )
	{
		const auto rgb = barPrimary( colour );
		const YCbCr c  = rgbToYCbCr( rgb[ 0 ] * amplitude, rgb[ 1 ] * amplitude, rgb[ 2 ] * amplitude, matrix );
		targets.push_back( {
			colour,
			c.cb,
			c.cr,
			degreesOf( c.cb, c.cr ),
			std::hypot( c.cb, c.cr )
		} );
	}
	return targets;
}

double skinToneAxisDeg( Matrix matrix )
{
	// The skin-tone line is the NTSC I axis, which sits at 123 degrees on a
	// BT.601 vectorscope. That number is a BT.601 number, so it cannot simply be
	// drawn at 123 degrees on a scope plotting BT.709 chroma -- the same
	// physical colour lands somewhere else.
	//
	// Taken here as a *pure chroma* direction, y = 0. That matters: chroma alone
	// does not determine an RGB colour, so "the same direction under a different
	// matrix" is only well defined on the zero-luma plane, where both matrices'
	// chroma are linear isomorphisms of the same 2D space. The RGB below has
	// negative components and is not a colour -- it is a direction, which is all
	// that is being transported. The result is independent of how far along the
	// axis it is sampled, which it would not be for any other luma.
	constexpr double kIAxisBT601Deg = 123.0;

	const double rad = kIAxisBT601Deg * kPi / 180.0;
	const auto direction = yCbCrToRgb( 0.0, std::cos( rad ), std::sin( rad ), Matrix::BT601 );
	const YCbCr c        = rgbToYCbCr( direction[ 0 ], direction[ 1 ], direction[ 2 ], matrix );

	return degreesOf( c.cb, c.cr );
}

// ---------------------------------------------------------------------------
// False colour
// ---------------------------------------------------------------------------

const std::vector< FalseColourBand >& falseColourBands()
{
	static const std::vector< FalseColourBand > bands = {
		{ -1e9, { 0.25f, 0.00f, 0.50f }, "sub-black" },
		{ 0.0, { 0.00f, 0.00f, 0.75f }, "crushed" },
		{ 5.0, { 0.00f, 0.55f, 0.85f }, "shadow" },
		{ 20.0, { 0.35f, 0.35f, 0.35f }, "low mid" },
		{ 45.0, { 0.55f, 0.55f, 0.55f }, "mid" },
		{ 55.0, { 0.75f, 0.75f, 0.75f }, "high mid" },
		{ 70.0, { 0.95f, 0.85f, 0.00f }, "highlight" },
		{ 90.0, { 0.95f, 0.45f, 0.00f }, "near clip" },
		{ 100.0, { 0.90f, 0.00f, 0.00f }, "clip" }
	};
	return bands;
}

const FalseColourBand& falseColourBandFor( double ire )
{
	const auto& bands = falseColourBands();

	// Lower-inclusive and ascending. The GLSL runs the identical loop; sctest
	// --probe measures one against the other.
	const FalseColourBand* found = &bands[ 0 ];
	for( const FalseColourBand& band : bands )
	{
		if( ire >= band.fromIre )
			found = &band;
		else
			break;
	}
	return *found;
}

} // namespace scopes
