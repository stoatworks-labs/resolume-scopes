#include "Scopes.h"

#include "Diag.h"
#include "Shaders.h"

#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace scopes;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Scopes >,                                  // Create method
	"SC01",                                                   // Plugin unique ID of maximum length 4.
	"Scopes",                                                 // Plugin name
	2,                                                        // API major version number
	1,                                                        // API minor version number
	0,                                                        // Plugin major version number
	1,                                                        // Plugin minor version number
	FF_EFFECT,                                                // Plugin type
	"Waveform, vectorscope, histogram and picture assist",     // Plugin description
	"Scopes FFGL effect"                                      // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and handing
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// The GL state this plugin changes, captured so it can be put back. FFGL
/// requires the context to be returned in a default state, and Resolume renders
/// the rest of the composition with whatever it finds.
struct SavedGLState
{
	GLint viewport[ 4 ];
	GLboolean blend;
	GLint blendSrcRGB;
	GLint blendDstRGB;
	GLint blendSrcAlpha;
	GLint blendDstAlpha;
	GLboolean programPointSize;

	void Capture()
	{
		glGetIntegerv( GL_VIEWPORT, viewport );
		blend = glIsEnabled( GL_BLEND );
		glGetIntegerv( GL_BLEND_SRC_RGB, &blendSrcRGB );
		glGetIntegerv( GL_BLEND_DST_RGB, &blendDstRGB );
		glGetIntegerv( GL_BLEND_SRC_ALPHA, &blendSrcAlpha );
		glGetIntegerv( GL_BLEND_DST_ALPHA, &blendDstAlpha );
		programPointSize = glIsEnabled( GL_PROGRAM_POINT_SIZE );
	}

	void Restore() const
	{
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
		glBlendFuncSeparate( blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha );
		if( blend )
			glEnable( GL_BLEND );
		else
			glDisable( GL_BLEND );
		if( programPointSize )
			glEnable( GL_PROGRAM_POINT_SIZE );
		else
			glDisable( GL_PROGRAM_POINT_SIZE );
		glBindVertexArray( 0 );
	}
};

/// Additive. Everything drawn into the scope buffer uses this, so a graticule
/// line under a dense trace brightens rather than punching a hole in it.
void setAdditiveBlend()
{
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE );
}

/// Premultiplied "over", for putting the finished scope onto the output.
void setOverBlend()
{
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
}
} // namespace

// ---------------------------------------------------------------------------
// Construction and parameter declaration
// ---------------------------------------------------------------------------

Scopes::Scopes()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back through GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// The default is a luma waveform overlaid bottom-left at a readable size,
	// because that is what somebody dropping an effect called Scopes onto a
	// layer is expecting to see. An effect that starts as a black rectangle
	// gets deleted before it is understood.
	//---------------------------------------------------------------------
	params[ SC_SCOPE ]     = 0.0f;//waveform
	params[ SC_WAVEFORM ]  = 0.0f;//luma
	params[ SC_HISTOGRAM ] = 0.0f;//RGB
	params[ SC_ASSIST ]    = 0.0f;//false colour
	params[ SC_INTENSITY ] = 0.5f;
	params[ SC_ZOOM ]      = 0.0f;//1x
	params[ SC_QUALITY ]   = 1.0f;//good

	params[ SC_LAYOUT ]  = 1.0f;//overlay
	params[ SC_SIZE ]    = 0.45f;
	params[ SC_X ]       = 0.02f;
	params[ SC_Y ]       = 0.02f;
	params[ SC_OPACITY ] = 0.85f;

	params[ SC_GRATICULE ]  = 0.5f;
	params[ SC_BARS ]       = 0.0f;//75%
	params[ SC_BACKGROUND ] = 0.8f;

	//BT.709 full range is what a modern Resolume composition on a modern
	//machine most often is. It is still a default and not a detection, and the
	//README says so.
	params[ SC_MATRIX ]         = 1.0f;//BT.709
	params[ SC_RANGE ]          = 0.0f;//full
	params[ SC_UNPREMULTIPLY ]  = 1.0f;//on

	params[ SC_ASSIST_LEVEL ] = 0.5f;

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every continuous parameter is a plain 0..1 float even where it stands for
	// IRE or a magnification. SetParamRange exists, but SetParamInfo clamps an
	// FF_TYPE_STANDARD default into 0..1 *before* a range can be attached (SDK
	// b1afaf9), so a parameter declared in IRE cannot declare a default in IRE.
	// The conversions live in Layout.cpp.
	//---------------------------------------------------------------------
	SetOptionParamInfo( SC_SCOPE, "Scope", 4, params[ SC_SCOPE ] );
	SetParamElementInfo( SC_SCOPE, 0, "Waveform", 0.0f );
	SetParamElementInfo( SC_SCOPE, 1, "Vectorscope", 1.0f );
	SetParamElementInfo( SC_SCOPE, 2, "Histogram", 2.0f );
	SetParamElementInfo( SC_SCOPE, 3, "Picture Assist", 3.0f );

	SetOptionParamInfo( SC_WAVEFORM, "Waveform", 3, params[ SC_WAVEFORM ] );
	SetParamElementInfo( SC_WAVEFORM, 0, "Luma", 0.0f );
	SetParamElementInfo( SC_WAVEFORM, 1, "RGB Parade", 1.0f );
	SetParamElementInfo( SC_WAVEFORM, 2, "Y Cb Cr Parade", 2.0f );

	SetOptionParamInfo( SC_HISTOGRAM, "Histogram", 3, params[ SC_HISTOGRAM ] );
	SetParamElementInfo( SC_HISTOGRAM, 0, "RGB", 0.0f );
	SetParamElementInfo( SC_HISTOGRAM, 1, "Luma", 1.0f );
	SetParamElementInfo( SC_HISTOGRAM, 2, "RGB + Luma", 2.0f );

	SetOptionParamInfo( SC_ASSIST, "Assist", 3, params[ SC_ASSIST ] );
	SetParamElementInfo( SC_ASSIST, 0, "False Colour", 0.0f );
	SetParamElementInfo( SC_ASSIST, 1, "Zebra", 1.0f );
	SetParamElementInfo( SC_ASSIST, 2, "Focus Peaking", 2.0f );

	SetParamInfof( SC_INTENSITY, "Intensity", FF_TYPE_STANDARD );
	SetParamInfof( SC_ZOOM, "Zoom", FF_TYPE_STANDARD );

	SetOptionParamInfo( SC_QUALITY, "Quality", 3, params[ SC_QUALITY ] );
	SetParamElementInfo( SC_QUALITY, 0, "Fast", 0.0f );
	SetParamElementInfo( SC_QUALITY, 1, "Good", 1.0f );
	SetParamElementInfo( SC_QUALITY, 2, "Full", 2.0f );

	SetOptionParamInfo( SC_LAYOUT, "Layout", 2, params[ SC_LAYOUT ] );
	SetParamElementInfo( SC_LAYOUT, 0, "Scope Only", 0.0f );
	SetParamElementInfo( SC_LAYOUT, 1, "Overlay", 1.0f );

	SetParamInfof( SC_SIZE, "Size", FF_TYPE_STANDARD );
	SetParamInfof( SC_X, "X", FF_TYPE_STANDARD );
	SetParamInfof( SC_Y, "Y", FF_TYPE_STANDARD );
	SetParamInfof( SC_OPACITY, "Opacity", FF_TYPE_STANDARD );

	SetParamInfof( SC_GRATICULE, "Graticule", FF_TYPE_STANDARD );

	SetOptionParamInfo( SC_BARS, "Bars", 2, params[ SC_BARS ] );
	SetParamElementInfo( SC_BARS, 0, "75%", 0.0f );
	SetParamElementInfo( SC_BARS, 1, "100%", 1.0f );

	SetParamInfof( SC_BACKGROUND, "Background", FF_TYPE_STANDARD );

	SetOptionParamInfo( SC_MATRIX, "Matrix", 3, params[ SC_MATRIX ] );
	SetParamElementInfo( SC_MATRIX, 0, "BT.601 SD", 0.0f );
	SetParamElementInfo( SC_MATRIX, 1, "BT.709 HD", 1.0f );
	SetParamElementInfo( SC_MATRIX, 2, "BT.2020 UHD", 2.0f );

	SetOptionParamInfo( SC_RANGE, "Range", 2, params[ SC_RANGE ] );
	SetParamElementInfo( SC_RANGE, 0, "Full", 0.0f );
	SetParamElementInfo( SC_RANGE, 1, "Limited 16-235", 1.0f );

	SetParamInfof( SC_UNPREMULTIPLY, "Unpremultiply", FF_TYPE_BOOLEAN );

	SetParamInfof( SC_ASSIST_LEVEL, "Assist Level", FF_TYPE_STANDARD );

	//Nineteen parameters is well past the point where an ungrouped list in
	//somebody else's inspector stops being readable.
	SetParamGroup( SC_SCOPE, "Scope" );
	SetParamGroup( SC_WAVEFORM, "Scope" );
	SetParamGroup( SC_HISTOGRAM, "Scope" );
	SetParamGroup( SC_ASSIST, "Scope" );
	SetParamGroup( SC_INTENSITY, "Scope" );
	SetParamGroup( SC_ZOOM, "Scope" );
	SetParamGroup( SC_QUALITY, "Scope" );

	SetParamGroup( SC_LAYOUT, "Layout" );
	SetParamGroup( SC_SIZE, "Layout" );
	SetParamGroup( SC_X, "Layout" );
	SetParamGroup( SC_Y, "Layout" );
	SetParamGroup( SC_OPACITY, "Layout" );

	SetParamGroup( SC_GRATICULE, "Graticule" );
	SetParamGroup( SC_BARS, "Graticule" );
	SetParamGroup( SC_BACKGROUND, "Graticule" );

	SetParamGroup( SC_MATRIX, "Signal" );
	SetParamGroup( SC_RANGE, "Signal" );
	SetParamGroup( SC_UNPREMULTIPLY, "Signal" );

	SetParamGroup( SC_ASSIST_LEVEL, "Assist" );

	FFGLLog::LogToHost( "Created Scopes effect" );

	scopes::diag::init();
}

// ---------------------------------------------------------------------------
// InitGL
// ---------------------------------------------------------------------------

FFResult Scopes::InitGL( const FFGLViewportStruct* vp )
{
	// The GL strings first, and unconditionally: when a shader will not compile
	// it is almost always the driver or the GL version, and knowing which
	// machine reported what is most of the diagnosis.
	scopes::diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	                    + " renderer=" + glStringOrUnknown( GL_RENDERER )
	                    + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader& shader;
		const std::string& vertex;
		const std::string& fragment;
		const char* name;
	} programs[] = {
		{ waveformShader, scopes::waveformVertex(), scopes::traceFragment(), "waveform" },
		{ vectorscopeShader, scopes::vectorscopeVertex(), scopes::traceFragment(), "vectorscope" },
		{ histogramAccumShader, scopes::histogramAccumVertex(), scopes::histogramAccumFragment(), "histogram accumulate" },
		{ histogramDrawShader, scopes::histogramDrawVertex(), scopes::histogramDrawFragment(), "histogram draw" },
		{ pictureShader, scopes::pictureVertex(), scopes::pictureFragment(), "picture" },
		{ lineShader, scopes::lineVertex(), scopes::lineFragment(), "graticule" },
		{ compositeShader, scopes::compositeVertex(), scopes::compositeFragment(), "composite" }
	};

	for( auto& program : programs )
	{
		if( !program.shader.Compile( program.vertex, program.fragment ) )
		{
			// Returning FF_FAIL is invisible to the operator: the effect simply
			// does nothing in Resolume, with no message anywhere. Naming the
			// stage here is the only record of which one it was.
			scopes::diag::error( std::string( "shader failed to compile: " ) + program.name
			                     + " - the effect will do nothing" );
			FFGLLog::LogToHost( "Scopes: shader failed to compile" );
			DeInitGL();
			return FF_FAIL;
		}
	}

	if( !quad.Initialise() )
	{
		scopes::diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Scopes: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	// The scatter passes read everything from gl_VertexID and source no
	// attributes at all, but a core profile still refuses to draw without a
	// vertex array object bound. This one stays empty on purpose.
	glGenVertexArrays( 1, &scatterVAO );

	glGenVertexArrays( 1, &lineVAO );
	glGenBuffers( 1, &lineVBO );
	glBindVertexArray( lineVAO );
	glBindBuffer( GL_ARRAY_BUFFER, lineVBO );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 3 * sizeof( float ), (void*)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 1, GL_FLOAT, GL_FALSE, 3 * sizeof( float ), (void*)( 2 * sizeof( float ) ) );
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	if( scatterVAO == 0 || lineVAO == 0 || lineVBO == 0 )
	{
		scopes::diag::error( "failed to create vertex arrays" );
		DeInitGL();
		return FF_FAIL;
	}

	scopes::diag::info( "initialised" );

	//Use the base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

// ---------------------------------------------------------------------------
// Uniform helpers
// ---------------------------------------------------------------------------

void Scopes::SetSignalUniforms( FFGLShader& shader )
{
	const Matrix matrix = optionFrom< Matrix >( params[ SC_MATRIX ], 3 );
	const Range range   = optionFrom< Range >( params[ SC_RANGE ], 2 );

	const LumaCoefficients k = coefficients( matrix );
	const AffineMapping map  = receivedToSignal( range );

	shader.Set( "LumaCoeff", static_cast< float >( k.kr ), static_cast< float >( k.kg ), static_cast< float >( k.kb ) );
	shader.Set( "RangeMap", static_cast< float >( map.scale ), static_cast< float >( map.offset ) );
	shader.Set( "Unpremultiply", params[ SC_UNPREMULTIPLY ] > 0.5f ? 1.0f : 0.0f );
}

void Scopes::SetSamplingUniforms( FFGLShader& shader,
                                  const FFGLTextureStruct& picture,
                                  const SampleGrid& grid,
                                  float pointSize )
{
	shader.Set( "InputTexture", 0 );
	shader.Set( "PictureSize", static_cast< float >( picture.Width ), static_cast< float >( picture.Height ) );
	shader.Set( "SampleGrid", static_cast< float >( grid.columns ), static_cast< float >( grid.rows ) );
	shader.Set( "PointSize", pointSize );
}

void Scopes::DrawGraticule( const LineVertices& vertices, float gain )
{
	if( vertices.empty() || gain <= 0.0f )
		return;

	ScopedShaderBinding shaderBinding( lineShader.GetGLID() );
	lineShader.Set( "LineColour", 0.65f, 0.72f, 0.78f );
	lineShader.Set( "LineGain", gain );

	glBindVertexArray( lineVAO );
	glBindBuffer( GL_ARRAY_BUFFER, lineVBO );
	// Re-uploaded every frame rather than cached against the parameters it
	// depends on. It is a few hundred vertices, and a cache keyed on "did any of
	// matrix, bars, zoom, aspect or mode change" is exactly the kind of
	// invalidation nobody gets right twice -- a stale graticule would be a
	// wrong reading that looks perfectly plausible.
	glBufferData( GL_ARRAY_BUFFER,
	              static_cast< GLsizeiptr >( vertices.size() * sizeof( float ) ),
	              vertices.data(),
	              GL_STREAM_DRAW );

	glDrawArrays( GL_LINES, 0, static_cast< GLsizei >( vertices.size() / 3 ) );

	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindVertexArray( 0 );
}

// ---------------------------------------------------------------------------
// ProcessOpenGL
// ---------------------------------------------------------------------------

FFResult Scopes::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const Scope scope         = optionFrom< Scope >( params[ SC_SCOPE ], 4 );
	const LayoutMode layout   = optionFrom< LayoutMode >( params[ SC_LAYOUT ], 2 );
	const Quality quality     = optionFrom< Quality >( params[ SC_QUALITY ], 3 );
	const Matrix matrix       = optionFrom< Matrix >( params[ SC_MATRIX ], 3 );
	const WaveformMode wfMode = optionFrom< WaveformMode >( params[ SC_WAVEFORM ], 3 );
	const HistogramMode hgMode = optionFrom< HistogramMode >( params[ SC_HISTOGRAM ], 3 );
	const AssistMode asMode   = optionFrom< AssistMode >( params[ SC_ASSIST ], 3 );

	SavedGLState saved;
	saved.Capture();

	const GLint outputWidth  = saved.viewport[ 2 ];
	const GLint outputHeight = saved.viewport[ 3 ];
	if( outputWidth <= 0 || outputHeight <= 0 )
		return FF_FAIL;

	const float outputAspect = static_cast< float >( outputWidth ) / static_cast< float >( outputHeight );
	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );

	ScopedSamplerActivation activateSampler( 0 );
	Scoped2DTextureBinding textureBinding( picture.Handle );

	// -----------------------------------------------------------------------
	// Picture Assist is not a scope with a rectangle -- it is the picture with
	// a different fragment shader on it. It shares nothing below.
	// -----------------------------------------------------------------------
	if( scope == Scope::PictureAssist )
	{
		glDisable( GL_BLEND );

		ScopedShaderBinding shaderBinding( pictureShader.GetGLID() );
		SetSignalUniforms( pictureShader );

		pictureShader.Set( "InputTexture", 0 );
		pictureShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		pictureShader.Set( "Texel",
		                   maxCoords.s / static_cast< float >( picture.Width ),
		                   maxCoords.t / static_cast< float >( picture.Height ) );
		pictureShader.Set( "Overlay", static_cast< float >( static_cast< int >( asMode ) + 1 ) );
		pictureShader.Set( "ZebraLevel", static_cast< float >( zebraIre( params[ SC_ASSIST_LEVEL ] ) ) );
		pictureShader.Set( "PeakThreshold", static_cast< float >( peakThreshold( params[ SC_ASSIST_LEVEL ] ) ) );

		const auto& bands = falseColourBands();
		const int count   = std::min( static_cast< int >( bands.size() ), kMaxFalseColourBands );
		pictureShader.Set( "BandCount", static_cast< float >( count ) );
		for( int i = 0; i < count; ++i )
		{
			// -1e9 for the bottom band would come through a float uniform as
			// -inf on some drivers and compare unhelpfully, so it is floored at
			// a value no IRE reading can go below.
			const std::string ireName    = "BandIre[" + std::to_string( i ) + "]";
			const std::string colourName = "BandColour[" + std::to_string( i ) + "]";
			pictureShader.Set( ireName.c_str(), static_cast< float >( std::max( bands[ i ].fromIre, -1000.0 ) ) );
			pictureShader.Set( colourName.c_str(), bands[ i ].colour[ 0 ], bands[ i ].colour[ 1 ], bands[ i ].colour[ 2 ] );
		}

		quad.Draw();

		saved.Restore();
		return FF_SUCCESS;
	}

	// -----------------------------------------------------------------------
	// Everything else: draw the scope into its own buffer, then composite.
	// -----------------------------------------------------------------------
	const Rect rect = scopeRect( layout, params[ SC_SIZE ], params[ SC_X ], params[ SC_Y ], outputAspect );

	const GLsizei scopeWidth  = std::max( 16, static_cast< int >( std::lround( rect.width * outputWidth ) ) );
	const GLsizei scopeHeight = std::max( 16, static_cast< int >( std::lround( rect.height * outputHeight ) ) );

	// RGBA16F, not 32F. The trace is a picture: half-float carries it with room
	// to spare, and the one place precision genuinely matters -- the histogram's
	// bins, where adding 1.0 to a bin holding 4096 in half-float rounds straight
	// back to 4096 and the tallest bins stop counting first -- is a separate
	// 32F buffer below.
	if( !scopeBuffer.Ensure( scopeWidth, scopeHeight, GL_RGBA16F ) )
	{
		scopes::diag::error( "could not allocate the scope buffer" );
		saved.Restore();
		return FF_FAIL;
	}

	const SampleGrid grid = sampleGridFor( static_cast< int >( picture.Width ),
	                                       static_cast< int >( picture.Height ),
	                                       quality );

	//The background is written premultiplied, like everything else that lands
	//in this buffer.
	const float background = params[ SC_BACKGROUND ];
	scopeBuffer.ClearTo( 0.0f, 0.0f, 0.0f, background );

	{
		ScopedFBOBinding fbo( scopeBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, scopeWidth, scopeHeight );
		setAdditiveBlend();

		// gl_PointSize is ignored in a desktop core profile unless this is on.
		// Without it every point is one pixel, which on a vectorscope means an
		// entire flat colour renders as a single pixel and reads as no trace at
		// all -- a scope that is numerically perfect and looks broken.
		glEnable( GL_PROGRAM_POINT_SIZE );

		const float graticuleGain = params[ SC_GRATICULE ];
		const double barAmplitude = params[ SC_BARS ] > 0.5f ? 1.0 : 0.75;
		const double zoom         = vectorZoom( params[ SC_ZOOM ] );
		const double scopeAspect  = static_cast< double >( scopeWidth ) / static_cast< double >( scopeHeight );

		switch( scope )
		{
		case Scope::Waveform:
		{
			DrawGraticule( waveformGraticule( wfMode ), graticuleGain );

			const int segments = ( wfMode == WaveformMode::Luma ) ? 1 : 3;

			// Wide enough to close the gaps a sparse sample grid leaves, in both
			// directions, because they arise for two unrelated reasons.
			//
			// Horizontally the spacing is fixed: one trace column per sample
			// column, spread across the tile. A source narrower than the tile --
			// any Quality below Full, or any overlay on a wide composition --
			// otherwise draws a combed trace that reads as aliasing rather than
			// as coarse sampling.
			//
			// Vertically there is no fixed pitch, because a sample's height is
			// its value rather than its row. But the worst case is bounded: a
			// column holding a smooth sweep of the whole range puts `rows`
			// samples across the tile's full height, so that spacing is as far
			// apart as they can ever get.
			const float columnPitch = static_cast< float >( scopeWidth )
			                          / static_cast< float >( segments * std::max( 1, grid.columns ) );
			const float valuePitch = static_cast< float >( scopeHeight )
			                         / static_cast< float >( std::max( 1, grid.rows ) );
			// Rounded up, because a driver is free to round gl_PointSize to an
			// integer and most do -- a pitch of 1.4 asks for a 1.4-pixel point,
			// gets a 1-pixel one, and closes nothing. Below a pitch of 1 this
			// stays at 1 and the trace keeps its full resolution, so the cost is
			// paid only in the sparse case that needs it.
			const float pointSize = std::max( 1.0f, std::ceil( std::max( columnPitch, valuePitch ) ) );

			ScopedShaderBinding shaderBinding( waveformShader.GetGLID() );
			SetSignalUniforms( waveformShader );
			SetSamplingUniforms( waveformShader, picture, grid, pointSize );
			waveformShader.Set( "Segments", static_cast< float >( segments ) );
			waveformShader.Set( "Mode", static_cast< float >( static_cast< int >( wfMode ) ) );
			waveformShader.Set( "IreRange", static_cast< float >( kWaveformIreMin ), static_cast< float >( kWaveformIreMax ) );
			waveformShader.Set( "Intensity", traceGain( params[ SC_INTENSITY ], grid.count() ) );

			glBindVertexArray( scatterVAO );
			glDrawArrays( GL_POINTS, 0, static_cast< GLsizei >( grid.count() * segments ) );
			glBindVertexArray( 0 );
			break;
		}

		case Scope::Vectorscope:
		{
			DrawGraticule( vectorscopeGraticule( matrix, barAmplitude, zoom, scopeAspect ), graticuleGain );

			// Bigger points than the waveform, deliberately. A waveform spreads
			// its samples over hundreds of columns; every sample of a flat
			// colour lands on one vectorscope coordinate, so at one pixel a
			// colour bar is a single dot.
			const float pointSize = std::max( 1.5f, static_cast< float >( scopeHeight ) / 300.0f );

			ScopedShaderBinding shaderBinding( vectorscopeShader.GetGLID() );
			SetSignalUniforms( vectorscopeShader );
			SetSamplingUniforms( vectorscopeShader, picture, grid, pointSize );
			vectorscopeShader.Set( "FullScale", static_cast< float >( kVectorFullScale ) );
			vectorscopeShader.Set( "Zoom", static_cast< float >( zoom ) );
			vectorscopeShader.Set( "Aspect", static_cast< float >( scopeAspect ) );
			vectorscopeShader.Set( "Intensity", traceGain( params[ SC_INTENSITY ], grid.count() ) );

			glBindVertexArray( scatterVAO );
			glDrawArrays( GL_POINTS, 0, static_cast< GLsizei >( grid.count() ) );
			glBindVertexArray( 0 );
			break;
		}

		case Scope::Histogram:
		{
			const HistogramChannels channels = histogramChannels( hgMode );

			// RGBA32F, and it has to be. A histogram accumulated in half-float
			// silently stops counting: ~11 mantissa bits means adding 1.0 to a
			// bin holding 4096 rounds back to 4096, and the bins that stall
			// first are the tallest -- which are exactly the clipped whites and
			// crushed blacks the histogram was opened to find.
			//
			// Exact filtering too: a linear tap across the bin texture averages
			// neighbouring bins and reports counts that were never measured.
			if( binBuffer.Ensure( kHistogramBins, 1, GL_RGBA32F, ScopeBuffer::Exact ) )
			{
				binBuffer.ClearTo( 0.0f, 0.0f, 0.0f, 0.0f );

				{
					ScopedFBOBinding binFbo( binBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
					glViewport( 0, 0, kHistogramBins, 1 );

					ScopedShaderBinding shaderBinding( histogramAccumShader.GetGLID() );
					SetSignalUniforms( histogramAccumShader );
					SetSamplingUniforms( histogramAccumShader, picture, grid, 1.0f );
					histogramAccumShader.Set( "Bins", static_cast< float >( kHistogramBins ) );
					histogramAccumShader.Set( "ChannelBase", static_cast< float >( channels.base ) );

					glBindVertexArray( scatterVAO );
					glDrawArrays( GL_POINTS, 0, static_cast< GLsizei >( grid.count() * channels.count ) );
					glBindVertexArray( 0 );
				}

				glViewport( 0, 0, scopeWidth, scopeHeight );
				DrawGraticule( histogramGraticule(), graticuleGain );

				ScopedShaderBinding shaderBinding( histogramDrawShader.GetGLID() );
				ScopedSamplerActivation activateBins( 1 );
				Scoped2DTextureBinding binTexture( binBuffer.TextureID() );
				histogramDrawShader.Set( "BinTexture", 1 );
				histogramDrawShader.Set( "Norm", histogramNorm( params[ SC_INTENSITY ], grid.count(), kHistogramBins ) );
				histogramDrawShader.Set( "ChannelMask",
				                         hgMode == HistogramMode::Luma ? 0.0f : 1.0f,
				                         hgMode == HistogramMode::Luma ? 0.0f : 1.0f,
				                         hgMode == HistogramMode::Luma ? 0.0f : 1.0f,
				                         hgMode == HistogramMode::Rgb ? 0.0f : 1.0f );

				quad.Draw();
			}
			break;
		}

		case Scope::PictureAssist:
			break;//handled above
		}
	}

	// -----------------------------------------------------------------------
	// Onto the output.
	// -----------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
	glViewport( saved.viewport[ 0 ], saved.viewport[ 1 ], saved.viewport[ 2 ], saved.viewport[ 3 ] );

	if( layout == LayoutMode::Overlay )
	{
		glDisable( GL_BLEND );

		ScopedShaderBinding shaderBinding( pictureShader.GetGLID() );
		SetSignalUniforms( pictureShader );
		pictureShader.Set( "InputTexture", 0 );
		pictureShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		pictureShader.Set( "Texel",
		                   maxCoords.s / static_cast< float >( picture.Width ),
		                   maxCoords.t / static_cast< float >( picture.Height ) );
		pictureShader.Set( "Overlay", 0.0f );
		pictureShader.Set( "BandCount", 0.0f );
		pictureShader.Set( "ZebraLevel", 1000.0f );
		pictureShader.Set( "PeakThreshold", 1000.0f );

		quad.Draw();
	}
	else
	{
		// Scope Only replaces the picture, so whatever the host left in this
		// buffer has to go. Cleared to transparent rather than to black: the
		// scope's own Background then decides whether the layer is opaque, and
		// at Background = 0 the trace composites over the layers underneath.
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
	}

	{
		setOverBlend();

		ScopedShaderBinding shaderBinding( compositeShader.GetGLID() );
		ScopedSamplerActivation activateScope( 1 );
		Scoped2DTextureBinding scopeTexture( scopeBuffer.TextureID() );
		compositeShader.Set( "ScopeTexture", 1 );
		compositeShader.Set( "Opacity", params[ SC_OPACITY ] );
		compositeShader.Set( "Placement", rect.x, rect.y, rect.width, rect.height );

		quad.Draw();
	}

	saved.Restore();

	return FF_SUCCESS;
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

FFResult Scopes::DeInitGL()
{
	waveformShader.FreeGLResources();
	vectorscopeShader.FreeGLResources();
	histogramAccumShader.FreeGLResources();
	histogramDrawShader.FreeGLResources();
	pictureShader.FreeGLResources();
	lineShader.FreeGLResources();
	compositeShader.FreeGLResources();

	quad.Release();

	scopeBuffer.Destroy();
	binBuffer.Destroy();

	if( lineVBO != 0 )
	{
		glDeleteBuffers( 1, &lineVBO );
		lineVBO = 0;
	}
	if( lineVAO != 0 )
	{
		glDeleteVertexArrays( 1, &lineVAO );
		lineVAO = 0;
	}
	if( scatterVAO != 0 )
	{
		glDeleteVertexArrays( 1, &scatterVAO );
		scatterVAO = 0;
	}

	return FF_SUCCESS;
}

FFResult Scopes::SetFloatParameter( unsigned int index, float value )
{
	if( index >= SC_COUNT )
		return FF_FAIL;

	//Deliberately not logged. A parameter change is not a diagnostic event: the
	//host already shows the value, and an operator animating a slider would put
	//a line in the log every frame. This log exists for the shader that will not
	//compile, and it is worth nothing if it is buried.
	params[ index ] = value;

	return FF_SUCCESS;
}

float Scopes::GetFloatParameter( unsigned int index )
{
	if( index >= SC_COUNT )
		return 0.0f;

	return params[ index ];
}
