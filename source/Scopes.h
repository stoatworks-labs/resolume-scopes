#pragma once

#include "Graticule.h"
#include "Layout.h"
#include "ScopeBuffer.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <string>

/**
    Scopes -- waveform, vectorscope, histogram and picture assist for Resolume.

    The effect measures the picture arriving on the layer and draws the
    measurement, either in place of the picture or over it.

    ---------------------------------------------------------------------------
    What a scope in a VJ host can and cannot honestly claim
    ---------------------------------------------------------------------------

    This is a *measurement* tool living inside a *performance* tool, and the
    seam between those two is the whole design problem.

    A hardware scope is fed a signal whose encoding is defined by a standard. An
    FFGL effect is handed a texture by a compositing engine, and by then three
    decisions have already been made and thrown away: which matrix took the clip
    from Y'CbCr to RGB, whether studio-range levels were expanded, and whether
    the colour has been multiplied by its alpha. Nothing in the pixels records
    any of them, and every one of them changes the reading.

    So Matrix, Range and Unpremultiply are parameters, and the plugin guesses at
    none of them. That is not a gap in the implementation -- it is the only
    honest arrangement, because a scope that silently picks an interpretation is
    a scope that is confidently wrong roughly half the time.

    ---------------------------------------------------------------------------
    The shape of it
    ---------------------------------------------------------------------------

    Everything except Picture Assist renders in two stages: the scope is drawn
    into its own buffer at the size it will occupy, then that buffer is
    composited onto the output. Two stages rather than one because the trace
    accumulates additively in floating point and has to be clamped once, at the
    end -- clamping as it accumulates would stop it building up at all, which is
    the entire mechanism by which a scatter plot becomes a scope.

    Picture Assist has no scope buffer. It is the picture with a different
    fragment shader on it, so Layout, Size, X, Y and Opacity do nothing while it
    is selected.

    See AGENTS.md for the traps.
*/
class Scopes : public CFFGLPlugin
{
public:
	Scopes();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters on
	/// a fresh instance and deletes the instance if one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure -- so without this override no real host can load the plugin,
	/// while every offline harness here carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// The order the host shows them in.
	enum ParamID : FFUInt32
	{
		//Scope -- what is being measured and how finely.
		SC_SCOPE,
		SC_WAVEFORM,
		SC_HISTOGRAM,
		SC_ASSIST,
		SC_INTENSITY,
		SC_ZOOM,
		SC_QUALITY,

		//Layout -- where it goes.
		SC_LAYOUT,
		SC_SIZE,
		SC_X,
		SC_Y,
		SC_OPACITY,

		//Graticule.
		SC_GRATICULE,
		SC_BARS,
		SC_BACKGROUND,

		//Signal -- how to read the pixels. None of this is detectable.
		SC_MATRIX,
		SC_RANGE,
		SC_UNPREMULTIPLY,

		//Assist.
		SC_ASSIST_LEVEL,

		//About. FFGL has no window and cannot make one, so the name, the
		//version, the maker and the links are parameters the host draws with
		//everything else. Last in the enum so no saved composition's parameter
		//ids shift. See StoatworksAboutParams.h.
		SC_ABOUT_FIRST,
		SC_COUNT = SC_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// Sets LumaCoeff, RangeMap and Unpremultiply on whichever program is bound.
	/// Every measuring shader takes them and none of them has its own copy.
	void SetSignalUniforms( ffglex::FFGLShader& shader );

	/// Binds the input texture and the sampling uniforms shared by the scatter
	/// passes.
	void SetSamplingUniforms( ffglex::FFGLShader& shader,
	                          const FFGLTextureStruct& picture,
	                          const scopes::SampleGrid& grid,
	                          float pointSize );

	/// Uploads and draws graticule geometry into whatever is currently bound.
	void DrawGraticule( const scopes::LineVertices& vertices, float gain );

	//Programs. Separate rather than one uber-shader because a scatter pass and a
	//screen-quad pass have genuinely different vertex stages.
	ffglex::FFGLShader waveformShader;
	ffglex::FFGLShader vectorscopeShader;
	ffglex::FFGLShader histogramAccumShader;
	ffglex::FFGLShader histogramDrawShader;
	ffglex::FFGLShader pictureShader;
	ffglex::FFGLShader lineShader;
	ffglex::FFGLShader compositeShader;

	ffglex::FFGLScreenQuad quad;

	scopes::ScopeBuffer scopeBuffer;
	scopes::ScopeBuffer binBuffer;

	/// A core profile refuses to draw with no vertex array bound, even when the
	/// vertex shader sources everything from gl_VertexID and touches no
	/// attribute at all. This exists solely to satisfy that.
	GLuint scatterVAO = 0;

	GLuint lineVAO = 0;
	GLuint lineVBO = 0;

	/// Zero-initialised: the constructor writes a default for every real
	/// control, but the About block's ids are never stored to -- pressing a
	/// button opens a browser and returns -- so without this GetFloatParameter
	/// hands the host whatever was on the stack for them.
	float params[ SC_COUNT ] = {};
};
