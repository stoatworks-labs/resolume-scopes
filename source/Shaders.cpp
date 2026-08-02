#include "Shaders.h"

namespace scopes
{
namespace
{

// ---------------------------------------------------------------------------
// Prelude -- the signal model, and nothing else
// ---------------------------------------------------------------------------

const char* const kPrelude = R"(#version 410 core

//kr, kg, kb -- from coefficients() in Colorimetry.cpp.
uniform vec3 LumaCoeff;
//The affine received -> signal mapping: signal = received * RangeMap.x + RangeMap.y.
//Identity for full range; undoes 16-235 for studio range.
uniform vec2 RangeMap;
//1 to divide out alpha before measuring anything.
uniform float Unpremultiply;

vec3 toSignal( vec3 rgb )
{
	return rgb * RangeMap.x + RangeMap.y;
}

float lumaOf( vec3 sig )
{
	return dot( sig, LumaCoeff );
}

//Cb, Cr from signal RGB. The offset in toSignal cancels in a colour difference,
//so limited range only ever scales the chroma -- which is correct, and worth
//knowing when reading this.
vec2 chromaOf( vec3 sig, float y )
{
	return vec2(
		( sig.b - y ) / ( 2.0 * ( 1.0 - LumaCoeff.b ) ),
		( sig.r - y ) / ( 2.0 * ( 1.0 - LumaCoeff.r ) )
	);
}

//Resolume composites in premultiplied alpha, so a clip at 50% opacity arrives
//with its colour already halved. Measuring that gives a picture that reads a
//stop down for no reason visible in the composition, so this undoes it. The
//guard matters: a fully transparent pixel carries no colour at all, and
//dividing it by zero puts NaN in the trace -- which additive blending then
//spreads across the entire scope.
vec4 straighten( vec4 texel )
{
	if( Unpremultiply > 0.5 && texel.a > 0.0039 )
		return vec4( texel.rgb / texel.a, texel.a );
	return texel;
}
)";

// ---------------------------------------------------------------------------
// Sampling, shared by the three scatter passes
// ---------------------------------------------------------------------------

const char* const kSampling = R"(
uniform sampler2D InputTexture;
uniform vec2 PictureSize;   //picture pixels; the texture may be larger
uniform vec2 SampleGrid;    //columns, rows
uniform float PointSize;

//texelFetch and not texture, deliberately -- see Shaders.h.
vec3 fetchCell( vec2 cell )
{
	vec2 f      = ( cell + 0.5 ) / SampleGrid;
	ivec2 texel = ivec2( f * PictureSize );
	return toSignal( straighten( texelFetch( InputTexture, texel, 0 ) ).rgb );
}
)";

// ---------------------------------------------------------------------------
// Waveform
// ---------------------------------------------------------------------------

const char* const kWaveformVertexBody = R"(
uniform float Segments;   //1 for luma, 3 for either parade
uniform float Mode;       //0 luma, 1 RGB parade, 2 Y Cb Cr parade
uniform vec2 IreRange;    //min, max IRE mapped to the full height of the tile

out vec3 traceColour;

void main()
{
	int gx         = int( SampleGrid.x );
	int gy         = int( SampleGrid.y );
	int perSegment = gx * gy;
	int segment    = gl_VertexID / perSegment;
	int index      = gl_VertexID % perSegment;
	int column     = index % gx;
	vec2 cell      = vec2( column, index / gx );

	vec3 sig = fetchCell( cell );

	float value;
	vec3 colour;
	if( Mode < 0.5 )
	{
		value  = lumaOf( sig );
		colour = vec3( 0.55, 1.0, 0.65 );
	}
	else if( Mode < 1.5 )
	{
		value  = segment == 0 ? sig.r : ( segment == 1 ? sig.g : sig.b );
		colour = segment == 0 ? vec3( 1.0, 0.25, 0.25 )
		       : segment == 1 ? vec3( 0.25, 1.0, 0.35 )
		                      : vec3( 0.35, 0.5, 1.0 );
	}
	else
	{
		float y = lumaOf( sig );
		if( segment == 0 )
		{
			value  = y;
			colour = vec3( 0.85 );
		}
		else
		{
			vec2 c = chromaOf( sig, y );
			//Chroma is bipolar. Centring it on 50 IRE lets it share one
			//graticule with luma; the centre line means chroma zero there
			//rather than 50 IRE, and the graticule draws it brighter to say so.
			value  = 0.5 + ( segment == 1 ? c.x : c.y );
			colour = segment == 1 ? vec3( 0.45, 0.65, 1.0 ) : vec3( 1.0, 0.55, 0.55 );
		}
	}

	float t        = ( value * 100.0 - IreRange.x ) / ( IreRange.y - IreRange.x );
	float segWidth = 1.0 / Segments;
	float xInSeg   = ( float( column ) + 0.5 ) / float( gx );
	float x        = ( float( segment ) + xInSeg ) * segWidth;

	gl_Position  = vec4( x * 2.0 - 1.0, t * 2.0 - 1.0, 0.0, 1.0 );
	gl_PointSize = PointSize;
	traceColour  = colour;
}
)";

// ---------------------------------------------------------------------------
// Vectorscope
// ---------------------------------------------------------------------------

const char* const kVectorscopeVertexBody = R"(
uniform float FullScale;   //chroma magnitude at the graticule's outer ring
uniform float Zoom;
uniform float Aspect;      //tile width / height, so the plot stays circular

out vec3 traceColour;

void main()
{
	int gx    = int( SampleGrid.x );
	vec2 cell = vec2( gl_VertexID % gx, gl_VertexID / gx );

	vec3 sig = fetchCell( cell );
	vec2 c   = chromaOf( sig, lumaOf( sig ) ) / FullScale * Zoom;

	//Cb runs right and Cr runs up -- the mathematical convention, and the same
	//one vectorTargets() uses to place the graticule, so the trace and the boxes
	//cannot drift apart.
	vec2 p = c;
	if( Aspect > 1.0 )
		p.x /= Aspect;
	else
		p.y *= Aspect;

	gl_Position  = vec4( p, 0.0, 1.0 );
	gl_PointSize = PointSize;
	traceColour  = vec3( 0.45, 1.0, 0.55 );
}
)";

const char* const kTraceFragmentBody = R"(
uniform float Intensity;

in vec3 traceColour;
out vec4 fragColour;

void main()
{
	//Additive, so Intensity is a density control rather than a brightness: one
	//sample barely marks the tile and a thousand on the same spot saturate it.
	//Alpha carries the same amount, so where the trace is bright the scope is
	//opaque -- which is what makes Background = 0 usable, with the trace still
	//reading over whatever is underneath.
	fragColour = vec4( traceColour * Intensity, Intensity );
}
)";

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

const char* const kHistogramAccumVertexBody = R"(
uniform float Bins;
//R, G, B and luma are channels 0..3. Only the ones the current mode draws are
//accumulated, so luma-only costs a quarter of what all four would -- hence a
//base rather than a straight division.
uniform float ChannelBase;

flat out int binChannel;

void main()
{
	int gx         = int( SampleGrid.x );
	int gy         = int( SampleGrid.y );
	int perChannel = gx * gy;
	int channel    = int( ChannelBase ) + gl_VertexID / perChannel;
	int index      = gl_VertexID % perChannel;
	vec2 cell      = vec2( index % gx, index / gx );

	vec3 sig    = fetchCell( cell );
	float value = channel == 0 ? sig.r
	            : channel == 1 ? sig.g
	            : channel == 2 ? sig.b
	                           : lumaOf( sig );

	//Out-of-range samples pile into the end bins rather than vanishing. A
	//histogram that hides clipping is worse than no histogram at all.
	float bin = clamp( value, 0.0, 1.0 ) * ( Bins - 1.0 );
	float x   = ( bin + 0.5 ) / Bins;

	gl_Position = vec4( x * 2.0 - 1.0, 0.0, 0.0, 1.0 );
	//Deliberately 1.0 and not PointSize: this pass writes into a Bins x 1
	//target, where a fatter point would smear one sample across its neighbours.
	gl_PointSize = 1.0;
	binChannel   = channel;
}
)";

const char* const kHistogramAccumFragmentSource = R"(#version 410 core

flat in int binChannel;
out vec4 fragColour;

void main()
{
	//One-hot, so additive blending counts the four channels independently in
	//the four components of a single texel.
	fragColour = vec4(
		binChannel == 0 ? 1.0 : 0.0,
		binChannel == 1 ? 1.0 : 0.0,
		binChannel == 2 ? 1.0 : 0.0,
		binChannel == 3 ? 1.0 : 0.0
	);
}
)";

const char* const kHistogramDrawVertexSource = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	uv          = vUV;
	gl_Position = vPosition;
}
)";

const char* const kHistogramDrawFragmentSource = R"(#version 410 core

uniform sampler2D BinTexture;
uniform float Norm;         //scales raw counts to 0..1 of the tile height
uniform vec4 ChannelMask;   //which of R, G, B, luma to draw

in vec2 uv;
out vec4 fragColour;

void main()
{
	vec4 counts = texture( BinTexture, vec2( uv.x, 0.5 ) ) * Norm;

	vec3 rgb   = vec3( 0.0 );
	float hits = 0.0;

	//Each channel is its own column, and where they overlap the colours *add*
	//rather than averaging. That is the convention every other RGB histogram
	//uses and it is the one that carries information: red over green reads
	//yellow, all three read white, and "all three channels agree here" is the
	//single most useful thing an RGB histogram can tell you. Averaging turns
	//that same region into a muddy mid-grey that looks like a fourth colour.
	if( ChannelMask.r > 0.5 && uv.y < counts.r ) { rgb += vec3( 1.0, 0.2, 0.2 );  hits += 1.0; }
	if( ChannelMask.g > 0.5 && uv.y < counts.g ) { rgb += vec3( 0.2, 1.0, 0.3 );  hits += 1.0; }
	if( ChannelMask.b > 0.5 && uv.y < counts.b ) { rgb += vec3( 0.3, 0.45, 1.0 ); hits += 1.0; }
	if( ChannelMask.a > 0.5 && uv.y < counts.a ) { rgb += vec3( 0.85 );           hits += 1.0; }

	if( hits <= 0.0 )
		discard;

	//Alpha still grows with agreement, so a region every channel occupies is
	//opaque and a lone channel is translucent. Written premultiplied, like
	//everything else that lands in the scope buffer.
	float a    = min( 0.45 * hits, 1.0 );
	fragColour = vec4( min( rgb, vec3( 1.0 ) ) * a, a );
}
)";

// ---------------------------------------------------------------------------
// Picture, and the assist overlays
// ---------------------------------------------------------------------------

const char* const kPictureVertexSource = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

uniform vec2 MaxUV;

out vec2 uv;

void main()
{
	//Folding MaxUV in here is right for a pass that samples exactly where it is
	//told. The scatter passes must not do it -- they index texels directly.
	uv          = vUV * MaxUV;
	gl_Position = vPosition;
}
)";

/// 16 here is kMaxFalseColourBands; the static_assert below is what keeps them
/// equal, because a GLSL array bound cannot be written in terms of the C++ one.
const char* const kPictureFragmentBody = R"(
uniform sampler2D InputTexture;
uniform vec2 Texel;          //one input texel in UV, for the peaking gradient
uniform float Overlay;       //0 none, 1 false colour, 2 zebra, 3 focus peaking

uniform float BandCount;
uniform float BandIre[ 16 ];
uniform vec3 BandColour[ 16 ];

uniform float ZebraLevel;
uniform float PeakThreshold;

in vec2 uv;
out vec4 fragColour;

float lumaAt( vec2 at )
{
	return lumaOf( toSignal( straighten( texture( InputTexture, at ) ).rgb ) );
}

void main()
{
	vec4 texel = straighten( texture( InputTexture, uv ) );
	vec3 rgb   = texel.rgb;
	float ire  = lumaOf( toSignal( rgb ) ) * 100.0;

	//Everything below writes premultiplied, because that is how the picture
	//arrived and how the rest of the composition expects to be handed it back.
	if( Overlay < 0.5 )
	{
		fragColour = vec4( rgb * texel.a, texel.a );
		return;
	}

	if( Overlay < 1.5 )
	{
		//Lower-inclusive and ascending, exactly as in falseColourBandFor().
		//sctest --probe measures this loop against that one.
		vec3 banded = BandColour[ 0 ];
		for( int i = 0; i < 16; i++ )
		{
			if( float( i ) >= BandCount )
				break;
			if( ire >= BandIre[ i ] )
				banded = BandColour[ i ];
		}
		fragColour = vec4( banded * texel.a, texel.a );
		return;
	}

	if( Overlay < 2.5 )
	{
		//45-degree stripes in output pixels, so they keep their width however
		//the composition is scaled and never alias into a flat wash.
		float stripe = step( 0.5, fract( ( gl_FragCoord.x + gl_FragCoord.y ) / 12.0 ) );
		if( ire >= ZebraLevel && stripe > 0.5 )
		{
			fragColour = vec4( vec3( 1.0 ) * texel.a, texel.a );
			return;
		}
		fragColour = vec4( rgb * texel.a, texel.a );
		return;
	}

	//Sobel on luma. Cheap, isotropic enough, and it answers to edge contrast
	//rather than to absolute level, so a dark but sharp subject still peaks.
	float tl = lumaAt( uv + Texel * vec2( -1.0, -1.0 ) );
	float tt = lumaAt( uv + Texel * vec2(  0.0, -1.0 ) );
	float tr = lumaAt( uv + Texel * vec2(  1.0, -1.0 ) );
	float ll = lumaAt( uv + Texel * vec2( -1.0,  0.0 ) );
	float rr = lumaAt( uv + Texel * vec2(  1.0,  0.0 ) );
	float bl = lumaAt( uv + Texel * vec2( -1.0,  1.0 ) );
	float bb = lumaAt( uv + Texel * vec2(  0.0,  1.0 ) );
	float br = lumaAt( uv + Texel * vec2(  1.0,  1.0 ) );

	float gx = ( tr + 2.0 * rr + br ) - ( tl + 2.0 * ll + bl );
	float gy = ( bl + 2.0 * bb + br ) - ( tl + 2.0 * tt + tr );

	if( length( vec2( gx, gy ) ) >= PeakThreshold )
	{
		fragColour = vec4( vec3( 1.0, 0.2, 0.1 ) * texel.a, texel.a );
		return;
	}

	//Desaturated underneath, so the tint is the only colour on the picture and
	//a red subject cannot be mistaken for a peak.
	float y    = lumaOf( toSignal( rgb ) );
	fragColour = vec4( vec3( y ) * 0.8 * texel.a, texel.a );
}
)";

// ---------------------------------------------------------------------------
// Graticule
// ---------------------------------------------------------------------------

const char* const kLineVertexSource = R"(#version 410 core

layout( location = 0 ) in vec2 vPosition;
layout( location = 1 ) in float vBrightness;

out float brightness;

void main()
{
	brightness  = vBrightness;
	gl_Position = vec4( vPosition, 0.0, 1.0 );
}
)";

const char* const kLineFragmentSource = R"(#version 410 core

uniform vec3 LineColour;
uniform float LineGain;

in float brightness;
out vec4 fragColour;

void main()
{
	//Additive like everything else in the scope buffer, so a graticule line
	//under a dense trace brightens it rather than punching a hole in it.
	float a    = brightness * LineGain;
	fragColour = vec4( LineColour * a, a );
}
)";

// ---------------------------------------------------------------------------
// Composite
// ---------------------------------------------------------------------------

const char* const kCompositeVertexSource = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

//Where on the output the scope goes: xy origin, zw size, in 0..1.
uniform vec4 Placement;

out vec2 uv;

void main()
{
	uv = vUV;

	vec2 p      = Placement.xy + vUV * Placement.zw;
	gl_Position = vec4( p * 2.0 - 1.0, 0.0, 1.0 );
}
)";

const char* const kCompositeFragmentSource = R"(#version 410 core

uniform sampler2D ScopeTexture;
uniform float Opacity;

in vec2 uv;
out vec4 fragColour;

void main()
{
	//The scope buffer accumulated additively into a float target, so both
	//colour and alpha can be well over 1. Clamped here rather than at
	//accumulation time, where clamping would stop the trace building up at all.
	vec4 scope = clamp( texture( ScopeTexture, uv ), 0.0, 1.0 );

	//Premultiplied by construction -- the trace writes colour and alpha in step
	//-- so this is the source term of a premultiplied "over" and the blend func
	//is (ONE, ONE_MINUS_SRC_ALPHA).
	fragColour = scope * Opacity;
}
)";

static_assert( kMaxFalseColourBands == 16,
               "the false-colour arrays and loop bound are written as 16 in the GLSL above" );

std::string assemble( const char* sampling, const char* body )
{
	std::string source = kPrelude;
	if( sampling != nullptr )
		source += sampling;
	source += body;
	return source;
}

} // namespace

const std::string& waveformVertex()
{
	static const std::string source = assemble( kSampling, kWaveformVertexBody );
	return source;
}

const std::string& vectorscopeVertex()
{
	static const std::string source = assemble( kSampling, kVectorscopeVertexBody );
	return source;
}

const std::string& traceFragment()
{
	static const std::string source = assemble( nullptr, kTraceFragmentBody );
	return source;
}

const std::string& histogramAccumVertex()
{
	static const std::string source = assemble( kSampling, kHistogramAccumVertexBody );
	return source;
}

const std::string& histogramAccumFragment()
{
	static const std::string source = kHistogramAccumFragmentSource;
	return source;
}

const std::string& histogramDrawVertex()
{
	static const std::string source = kHistogramDrawVertexSource;
	return source;
}

const std::string& histogramDrawFragment()
{
	static const std::string source = kHistogramDrawFragmentSource;
	return source;
}

const std::string& pictureVertex()
{
	static const std::string source = kPictureVertexSource;
	return source;
}

const std::string& pictureFragment()
{
	static const std::string source = assemble( nullptr, kPictureFragmentBody );
	return source;
}

const std::string& lineVertex()
{
	static const std::string source = kLineVertexSource;
	return source;
}

const std::string& lineFragment()
{
	static const std::string source = kLineFragmentSource;
	return source;
}

const std::string& compositeVertex()
{
	static const std::string source = kCompositeVertexSource;
	return source;
}

const std::string& compositeFragment()
{
	static const std::string source = kCompositeFragmentSource;
	return source;
}

} // namespace scopes
