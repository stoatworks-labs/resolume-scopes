#include "Layout.h"

#include <algorithm>
#include <cmath>

namespace scopes
{

SampleGrid sampleGridFor( int pictureWidth, int pictureHeight, Quality quality )
{
	if( pictureWidth <= 0 || pictureHeight <= 0 )
		return { 1, 1 };

	int stride = 1;
	switch( quality )
	{
	case Quality::Fast:
		stride = 4;
		break;
	case Quality::Good:
		stride = 2;
		break;
	case Quality::Full:
	default:
		stride = 1;
		break;
	}

	// Grow the stride until the budget is met. Doubling rather than solving for
	// it keeps the grid on the same lattice the requested stride was on, so the
	// samples stay evenly spread instead of landing on a near-resonant pattern
	// with the source's own structure.
	SampleGrid grid{ 0, 0 };
	for( ;; )
	{
		grid.columns = std::max( 1, pictureWidth / stride );
		grid.rows    = std::max( 1, pictureHeight / stride );
		if( grid.count() <= kMaxSamples || stride >= 64 )
			break;
		stride *= 2;
	}

	return grid;
}

float traceGain( float param, int sampleCount )
{
	if( sampleCount <= 0 )
		return 0.0f;

	// Perceptually the useful range is wide -- a flat grey field puts every one
	// of its samples on one waveform row, a busy picture spreads them over
	// hundreds -- so the control is exponential rather than linear.
	const double density = std::pow( 10.0, -1.0 + 2.0 * static_cast< double >( param ) );//0.1 .. 10

	// Normalised against a 1080p-worth of samples so that Quality changes the
	// measurement's resolution without changing how bright it looks.
	const double reference = 1920.0 * 1080.0;
	return static_cast< float >( density * reference / static_cast< double >( sampleCount ) * 0.02 );
}

double vectorZoom( float param )
{
	// 1x at 0, 8x at 1, exponentially -- so 2x sits at 1/3 and 4x at 2/3 and the
	// useful low end is not crammed into the first few pixels of the slider.
	return std::pow( 8.0, static_cast< double >( param ) );
}

float histogramNorm( float param, int sampleCount, int bins )
{
	if( sampleCount <= 0 || bins <= 0 )
		return 0.0f;

	// 0.5 at the bottom, 4 in the middle, ~32 at the top. Most pictures put
	// several times a flat distribution's worth of samples into their busiest
	// bin, so the useful settings are above 1 and the range is weighted that way.
	const double gain = std::pow( 10.0, -0.3 + 1.8 * static_cast< double >( param ) );

	return static_cast< float >( gain * static_cast< double >( bins ) / static_cast< double >( sampleCount ) );
}

HistogramChannels histogramChannels( HistogramMode mode )
{
	switch( mode )
	{
	case HistogramMode::Luma:
		return { 3, 1 };
	case HistogramMode::RgbAndLuma:
		return { 0, 4 };
	case HistogramMode::Rgb:
	default:
		return { 0, 3 };
	}
}

double zebraIre( float param )
{
	return 50.0 + 60.0 * static_cast< double >( param );
}

double peakThreshold( float param )
{
	// A Sobel on 0..1 luma maxes out around 4 for a hard black-to-white edge.
	// The response of interest is well down at the bottom of that, so this is
	// exponential too: 0.02 at the bottom, 2.0 at the top.
	return std::pow( 10.0, -1.7 + 2.0 * static_cast< double >( param ) );
}

Rect scopeRect( LayoutMode layout, float size, float x, float y, float outputAspect )
{
	if( layout == LayoutMode::ScopeOnly )
		return { 0.0f, 0.0f, 1.0f, 1.0f };

	if( outputAspect <= 0.0f )
		outputAspect = 1.0f;

	// Size spans the shorter dimension, so a given setting looks the same on a
	// 16:9 composition and on a square one.
	const float span = 0.15f + 0.70f * std::min( std::max( size, 0.0f ), 1.0f );

	float width  = span;
	float height = span;
	if( outputAspect > 1.0f )
		width = span / outputAspect;
	else
		height = span * outputAspect;

	// X and Y run edge to edge over what is left, so the overlay is always
	// wholly on screen: there is no setting that parks the scope off the frame.
	const float px = std::min( std::max( x, 0.0f ), 1.0f );
	const float py = std::min( std::max( y, 0.0f ), 1.0f );

	return { px * ( 1.0f - width ), py * ( 1.0f - height ), width, height };
}

} // namespace scopes
