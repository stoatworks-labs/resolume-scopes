#pragma once

#include <FFGLSDK.h>

namespace scopes
{
/**
    An off-screen target for one stage of the scope.

    Three things on top of the SDK's FFGLFBO, the first two shared with
    `old-cathode`'s PassBuffer and the third specific to measuring.

    **It reallocates only when it has to.** `Ensure()` runs every frame and is a
    no-op in almost all of them; it only rebuilds when the scope's rectangle or
    the composition size actually changes.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` --
    leaking the colour texture on every release (SDK b1afaf9, `FFGLFBO.cpp`). One
    leak would not matter. This buffer is rebuilt whenever the operator drags the
    Size slider, so it would be one leak per frame of the drag.

    **It can be told to sample NEAREST.** The SDK always sets `GL_LINEAR`. That
    is right for the scope image on its way to the output, and wrong for the
    histogram's bin texture, where a linear tap silently averages neighbouring
    bins and reports counts that were never measured.
*/
class ScopeBuffer : public ffglex::FFGLFBO
{
public:
	enum Filter
	{
		Smooth, ///< GL_LINEAR -- for anything being shown rather than read.
		Exact   ///< GL_NEAREST -- for anything whose texel values are data.
	};

	~ScopeBuffer();

	/// Allocate at this size and format, reusing what is already there if it
	/// matches. A newly allocated buffer is cleared: a framebuffer's initial
	/// contents are undefined, and undefined here is whatever texture memory the
	/// driver handed back.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Filter filter = Smooth );

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	/// Bind, set the viewport to the whole buffer, and clear it to this colour.
	/// Restores the previous binding and viewport before returning.
	void ClearTo( float r, float g, float b, float a );

	bool IsValid() const
	{
		return GetGLID() != 0;
	}

	GLuint TextureID() const
	{
		return colorTextureID;
	}

private:
	Filter currentFilter = Smooth;
};

} // namespace scopes
