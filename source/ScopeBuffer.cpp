#include "ScopeBuffer.h"

namespace scopes
{

ScopeBuffer::~ScopeBuffer()
{
	//Nothing GL can happen here -- a destructor may well run with no current
	//context. DeInitGL is where the release has to be; this is only a reminder.
}

bool ScopeBuffer::Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Filter filter )
{
	if( requestedWidth <= 0 || requestedHeight <= 0 )
		return false;

	if( fboID != 0 && width == requestedWidth && height == requestedHeight
	    && internalColorFormat == format && currentFilter == filter )
		return true;

	Destroy();

	// Allocating disturbs the 2D texture binding on the active unit, so it is
	// saved and put back around everything below.
	//
	// This is not defensive tidiness, it is a real bug otherwise, and a
	// beautifully quiet one. `FFGLFBO::Initialise` sizes its new colour texture
	// under an `ffglex::Scoped2DTextureBinding`, and that helper's destructor
	// binds **0** rather than restoring what was there before -- every "Scoped"
	// binding in the SDK clears instead of reverting (SDK b1afaf9,
	// `FFGLScopedTextureBinding.cpp`). So a plugin that binds its input texture
	// once at the top of ProcessOpenGL and then allocates a buffer has silently
	// lost that input.
	//
	// The symptom is what makes it worth this comment: it only happens on the
	// frame where a buffer is actually allocated. Every other frame is correct.
	// So the scope reads zero for exactly one frame after loading, and one frame
	// each time the operator's drag on Size changes the buffer -- and a scope
	// that is right except during a drag is one nobody reports and everybody
	// half-trusts.
	GLint previousTexture = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &previousTexture );

	const bool created = Initialise( requestedWidth, requestedHeight, format );

	if( created )
	{
		currentFilter = filter;

		if( filter == Exact )
		{
			glBindTexture( GL_TEXTURE_2D, colorTextureID );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		}
	}

	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previousTexture ) );

	if( !created )
		return false;

	ClearTo( 0.0f, 0.0f, 0.0f, 0.0f );

	return true;
}

void ScopeBuffer::ClearTo( float r, float g, float b, float a )
{
	if( fboID == 0 )
		return;

	GLint previousFBO         = 0;
	GLint previousViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &previousFBO );
	glGetIntegerv( GL_VIEWPORT, previousViewport );

	glBindFramebuffer( GL_FRAMEBUFFER, fboID );
	glViewport( 0, 0, width, height );
	glClearColor( r, g, b, a );
	glClear( GL_COLOR_BUFFER_BIT );

	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previousFBO ) );
	glViewport( previousViewport[ 0 ], previousViewport[ 1 ], previousViewport[ 2 ], previousViewport[ 3 ] );
}

void ScopeBuffer::Destroy()
{
	//The SDK's Release() leaves this behind. Delete it first, then let the base
	//class deal with the framebuffer and the depth buffer.
	if( colorTextureID != 0 )
	{
		glDeleteTextures( 1, &colorTextureID );
		colorTextureID = 0;
	}

	Release();
}

} // namespace scopes
