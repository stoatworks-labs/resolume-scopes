/**
    sctest -- render the Scopes plugin offline, and check what it measured.

    A scope is not judged by eye. What it claims is a number, and a number can
    be checked -- so this harness is not a previewer with an assert bolted on,
    it is where the plugin's correctness actually lives. It builds a headless
    GL 4.1 core context, drives the real `Scopes` class through the real FFGL
    entry sequence, and reads the result back off the framebuffer.

        sctest --out /tmp/frame.png     a picture, on 75% bars
        sctest --verify                 do the scopes report the right numbers?
        sctest --probe                  does the GPU agree with the C++?
        sctest --list                   every parameter and its default

    `--verify` is the one that matters. It generates 75% colour bars, whose
    luma and chroma are *computed from the same Colorimetry.cpp the shaders are
    handed uniforms from*, then asserts that the waveform trace lands on each
    bar's luma, that the vectorscope trace lands in each bar's box, and that the
    histogram spikes at each bar's level. That the pattern and the expectation
    share a derivation is the point: the trace has to land in the box, so if it
    does not, something between the parameter and the pixel is wrong.

    What it cannot check is whether the *interpretation* is right -- whether
    this composition really is BT.709 full range. Nothing can; see the note at
    the top of Colorimetry.h.
*/

#include "Colorimetry.h"
#include "Graticule.h"
#include "Layout.h"
#include "Scopes.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace scopes;

namespace
{
constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Test pictures.
//
// The bars are built from the same BAR pattern Colorimetry.cpp derives the
// vectorscope targets from, at the same amplitude. That is deliberate: it means
// the expected reading and the picture cannot disagree about what "75% bars"
// is, and the only thing left under test is the path from the pixel to the
// plot.
//---------------------------------------------------------------------------
struct Bar
{
	double r;
	double g;
	double b;
	const char* name;
};

/// The classic seven, left to right, at the given amplitude.
std::vector< Bar > bars( double amplitude )
{
	return {
		{ amplitude, amplitude, amplitude, "white" },
		{ amplitude, amplitude, 0.0, "yellow" },
		{ 0.0, amplitude, amplitude, "cyan" },
		{ 0.0, amplitude, 0.0, "green" },
		{ amplitude, 0.0, amplitude, "magenta" },
		{ amplitude, 0.0, 0.0, "red" },
		{ 0.0, 0.0, amplitude, "blue" }
	};
}

unsigned char toByte( double v )
{
	const double scaled = std::lround( v * 255.0 );
	return static_cast< unsigned char >( std::min( 255.0, std::max( 0.0, scaled ) ) );
}

/**
    The bar as the GPU actually receives it.

    Every expectation below is computed from this rather than from the nominal
    amplitude. The test picture is an 8-bit texture, so 75% white is not 0.75 --
    it is 191/255, which is 74.90 IRE. Comparing the trace against 75.00 would
    be testing the harness's arithmetic against the picture's quantisation and
    calling the difference a plugin error; on the histogram, where a bin is one
    255th wide, it is the difference between the right bin and its neighbour.
*/
Bar quantised( const Bar& bar )
{
	return { toByte( bar.r ) / 255.0, toByte( bar.g ) / 255.0, toByte( bar.b ) / 255.0, bar.name };
}

/// Bottom-up, ready for glTexImage2D.
std::vector< unsigned char > barsPicture( int width, int height, double amplitude )
{
	const std::vector< Bar > pattern = bars( amplitude );
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const size_t index = ( static_cast< size_t >( y ) * width + x ) * 4;
			const int bar      = std::min( static_cast< int >( pattern.size() ) - 1,
			                               x * static_cast< int >( pattern.size() ) / width );
			pixels[ index + 0 ] = toByte( pattern[ bar ].r );
			pixels[ index + 1 ] = toByte( pattern[ bar ].g );
			pixels[ index + 2 ] = toByte( pattern[ bar ].b );
			pixels[ index + 3 ] = 255;
		}
	}
	return pixels;
}

/// A horizontal neutral ramp, 0 at the left and 1 at the right.
std::vector< unsigned char > rampPicture( int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const size_t index = ( static_cast< size_t >( y ) * width + x ) * 4;
			// Quantised to 8 bits on purpose: this is what the shader will get,
			// so the expectation has to be computed from the same value.
			const unsigned char v = static_cast< unsigned char >( x * 255 / std::max( 1, width - 1 ) );
			pixels[ index + 0 ]   = v;
			pixels[ index + 1 ]   = v;
			pixels[ index + 2 ]   = v;
			pixels[ index + 3 ]   = 255;
		}
	}
	return pixels;
}

/**
    A two-dimensional gradient.

    Bars and a horizontal ramp are both flat down every column, so all of a
    column's samples land on one waveform row and one vectorscope point. That is
    exactly what makes them good for `--verify` and useless for `sweep.py`:
    against them the trace saturates whatever Intensity and Quality are set to,
    and both controls read dead when they are working perfectly.

    This spreads each column over half the range, so density becomes visible.
*/
std::vector< unsigned char > gradientPicture( int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const size_t index = ( static_cast< size_t >( y ) * width + x ) * 4;
			const double fx    = static_cast< double >( x ) / std::max( 1, width - 1 );
			const double fy    = static_cast< double >( y ) / std::max( 1, height - 1 );

			// Not neutral: a grey gradient puts the whole vectorscope at the
			// origin, and Zoom then reads dead too.
			pixels[ index + 0 ] = toByte( fx );
			pixels[ index + 1 ] = toByte( fy );
			pixels[ index + 2 ] = toByte( 1.0 - fx * 0.5 - fy * 0.5 );
			pixels[ index + 3 ] = 255;
		}
	}
	return pixels;
}

/**
    Premultiplies the picture by a constant alpha, the way Resolume hands an
    effect a layer that is not at full opacity.

    Without this there is no way to exercise Unpremultiply at all: against an
    opaque source it correctly does nothing, and a sweep would call it dead.
*/
void premultiply( std::vector< unsigned char >& pixels, double alpha )
{
	const unsigned char a = toByte( alpha );
	for( size_t i = 0; i < pixels.size(); i += 4 )
	{
		for( int c = 0; c < 3; ++c )
			pixels[ i + c ] = static_cast< unsigned char >( pixels[ i + c ] * a / 255 );
		pixels[ i + 3 ] = a;
	}
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without a
	//GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

bool runPass( Scopes& plugin, GLuint source, int sourceWidth, int sourceHeight,
              GLuint targetFBO, int width, int height )
{
	FFGLTextureStruct inputStruct = {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( sourceWidth );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( sourceHeight );
	inputStruct.Handle                              = source;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process = {};
	process.numInputTextures    = 1;
	process.inputTextures       = inputs;
	process.HostFBO             = targetFBO;

	glBindFramebuffer( GL_FRAMEBUFFER, targetFBO );
	glViewport( 0, 0, width, height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
}

/// Top-down RGBA. GL hands back bottom-up, so this flips -- and everything
/// below indexes rows from the top.
std::vector< unsigned char > readBack( GLuint fbo, int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );

	std::vector< unsigned char > flipped( pixels.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             pixels.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

double brightnessAt( const std::vector< unsigned char >& image, int width, int x, int y )
{
	const size_t index = ( static_cast< size_t >( y ) * width + x ) * 4;
	return ( image[ index ] + image[ index + 1 ] + image[ index + 2 ] ) / 3.0 / 255.0;
}

//---------------------------------------------------------------------------
// Reporting
//---------------------------------------------------------------------------
struct Report
{
	int checks  = 0;
	int failures = 0;

	void check( bool ok, const char* what, const std::string& detail )
	{
		++checks;
		if( !ok )
			++failures;
		std::printf( "  %s  %-34s %s\n", ok ? "ok  " : "FAIL", what, detail.c_str() );
	}
};

std::string formatted( const char* format, ... )
{
	char buffer[ 256 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

//---------------------------------------------------------------------------
// The checks
//---------------------------------------------------------------------------

/// Maps an IRE reading to the row it should appear on, top-down, using exactly
/// the mapping the waveform vertex shader uses.
double rowForIre( double ire, int height )
{
	const double t = ( ire - kWaveformIreMin ) / ( kWaveformIreMax - kWaveformIreMin );
	// t is the fraction up the tile; GL row t*height, read back top-down.
	return ( 1.0 - t ) * height - 0.5;
}

/**
    The brightest row near a column -- for a flat bar every sample in the column
    lands on one row, so this is the reading.

    A few columns wide rather than one. The trace has as many columns as the
    source has sample columns, which is not the same as the number of pixels the
    tile is wide, so a single output column is not guaranteed to contain a
    sample at all. Bars here are 180 pixels wide, so a handful either side of the
    centre cannot stray into the neighbouring bar.
*/
int brightestRow( const std::vector< unsigned char >& image, int width, int height, int centreX )
{
	constexpr int kSpan = 3;

	int best         = -1;
	double bestValue = 0.0;
	for( int dx = -kSpan; dx <= kSpan; ++dx )
	{
		const int x = centreX + dx;
		if( x < 0 || x >= width )
			continue;
		for( int y = 0; y < height; ++y )
		{
			const double v = brightnessAt( image, width, x, y );
			if( v > bestValue )
			{
				bestValue = v;
				best      = y;
			}
		}
	}
	return bestValue > 0.15 ? best : -1;
}

bool verifyWaveform( Report& report, Scopes& plugin, GLuint source, int sourceWidth, int sourceHeight,
                     GLuint fbo, int width, int height, Matrix matrix, double amplitude )
{
	plugin.SetFloatParameter( Scopes::SC_SCOPE, 0.0f );     //waveform
	plugin.SetFloatParameter( Scopes::SC_WAVEFORM, 0.0f );  //luma
	plugin.SetFloatParameter( Scopes::SC_LAYOUT, 0.0f );    //scope only
	plugin.SetFloatParameter( Scopes::SC_GRATICULE, 0.0f ); //nothing to confuse the peak finder
	plugin.SetFloatParameter( Scopes::SC_BACKGROUND, 0.0f );
	plugin.SetFloatParameter( Scopes::SC_OPACITY, 1.0f );
	plugin.SetFloatParameter( Scopes::SC_QUALITY, 2.0f );   //full

	if( !runPass( plugin, source, sourceWidth, sourceHeight, fbo, width, height ) )
	{
		std::printf( "  FAIL  waveform render\n" );
		++report.checks;
		++report.failures;
		return false;
	}

	const std::vector< unsigned char > image = readBack( fbo, width, height );
	const std::vector< Bar > pattern         = bars( amplitude );

	for( size_t i = 0; i < pattern.size(); ++i )
	{
		// The middle of the bar, so a boundary column cannot be sampled.
		const int x = static_cast< int >( ( i + 0.5 ) / pattern.size() * width );

		const Bar bar            = quantised( pattern[ i ] );
		const double expectedIre = luma( bar.r, bar.g, bar.b, matrix ) * 100.0;
		const double expectedRow = rowForIre( expectedIre, height );
		const int foundRow       = brightestRow( image, width, height, x );

		if( foundRow < 0 )
		{
			report.check( false, pattern[ i ].name, "no trace in this column" );
			continue;
		}

		// Back out what IRE the trace actually landed on, so the failure message
		// is in the units the scope claims to report rather than in pixels.
		const double foundIre = kWaveformIreMax
		                        - ( foundRow + 0.5 ) / height * ( kWaveformIreMax - kWaveformIreMin );

		const double error = std::fabs( foundRow - expectedRow );
		report.check( error <= 2.0, pattern[ i ].name,
		              formatted( "%6.2f IRE, expected %6.2f  (row %d, expected %.1f)",
		                         foundIre, expectedIre, foundRow, expectedRow ) );
	}

	return true;
}

bool verifyVectorscope( Report& report, Scopes& plugin, GLuint source, int sourceWidth, int sourceHeight,
                        GLuint fbo, int width, int height, Matrix matrix, double amplitude )
{
	plugin.SetFloatParameter( Scopes::SC_SCOPE, 1.0f );     //vectorscope
	plugin.SetFloatParameter( Scopes::SC_LAYOUT, 0.0f );
	plugin.SetFloatParameter( Scopes::SC_GRATICULE, 0.0f );
	plugin.SetFloatParameter( Scopes::SC_BACKGROUND, 0.0f );
	plugin.SetFloatParameter( Scopes::SC_OPACITY, 1.0f );
	plugin.SetFloatParameter( Scopes::SC_ZOOM, 0.0f );      //1x
	plugin.SetFloatParameter( Scopes::SC_QUALITY, 2.0f );

	if( !runPass( plugin, source, sourceWidth, sourceHeight, fbo, width, height ) )
	{
		std::printf( "  FAIL  vectorscope render\n" );
		++report.checks;
		++report.failures;
		return false;
	}

	const std::vector< unsigned char > image = readBack( fbo, width, height );
	const double aspect                      = static_cast< double >( width ) / height;

	// The picture's amplitude, not the nominal one. The plugin's graticule is
	// drawn at the nominal 75% because that is what the standard says a box is;
	// the trace can only be where the 8-bit picture put it, 0.05% short of it.
	// Both are right, and the check has to be against the second.
	const double actualAmplitude = toByte( amplitude ) / 255.0;

	for( const VectorTarget& target : vectorTargets( matrix, actualAmplitude ) )
	{
		// The same aspect correction the shader and the graticule apply.
		double nx = target.cb / kVectorFullScale;
		double ny = target.cr / kVectorFullScale;
		if( aspect > 1.0 )
			nx /= aspect;
		else
			ny *= aspect;

		const double expectedX = ( nx * 0.5 + 0.5 ) * width;
		//Cr runs up in NDC and rows are counted from the top.
		const double expectedY = ( 0.5 - ny * 0.5 ) * height;

		// Centroid of whatever is bright near the target, so the reading is the
		// trace's actual position rather than "is this one pixel lit".
		const int radius = 12;
		double sumX = 0.0, sumY = 0.0, sum = 0.0;
		for( int dy = -radius; dy <= radius; ++dy )
		{
			for( int dx = -radius; dx <= radius; ++dx )
			{
				const int x = static_cast< int >( expectedX ) + dx;
				const int y = static_cast< int >( expectedY ) + dy;
				if( x < 0 || y < 0 || x >= width || y >= height )
					continue;
				const double v = brightnessAt( image, width, x, y );
				if( v < 0.2 )
					continue;
				sumX += x * v;
				sumY += y * v;
				sum += v;
			}
		}

		const char* name = "target";
		switch( target.colour )
		{
		case BarColour::Red: name = "red"; break;
		case BarColour::Magenta: name = "magenta"; break;
		case BarColour::Blue: name = "blue"; break;
		case BarColour::Cyan: name = "cyan"; break;
		case BarColour::Green: name = "green"; break;
		case BarColour::Yellow: name = "yellow"; break;
		}

		if( sum <= 0.0 )
		{
			report.check( false, name, formatted( "no trace within %d px of (%.0f, %.0f)",
			                                      radius, expectedX, expectedY ) );
			continue;
		}

		const double foundX = sumX / sum;
		const double foundY = sumY / sum;
		const double error  = std::hypot( foundX - expectedX, foundY - expectedY );

		report.check( error <= 3.0, name,
		              formatted( "%5.1f deg, %.1f px from its box", target.angleDeg, error ) );
	}

	return true;
}

bool verifyHistogram( Report& report, Scopes& plugin, GLuint source, int sourceWidth, int sourceHeight,
                      GLuint fbo, int width, int height, Matrix matrix, double amplitude )
{
	plugin.SetFloatParameter( Scopes::SC_SCOPE, 2.0f );     //histogram
	plugin.SetFloatParameter( Scopes::SC_HISTOGRAM, 1.0f ); //luma
	plugin.SetFloatParameter( Scopes::SC_LAYOUT, 0.0f );
	plugin.SetFloatParameter( Scopes::SC_GRATICULE, 0.0f );
	plugin.SetFloatParameter( Scopes::SC_BACKGROUND, 0.0f );
	plugin.SetFloatParameter( Scopes::SC_OPACITY, 1.0f );
	plugin.SetFloatParameter( Scopes::SC_INTENSITY, 0.5f );
	plugin.SetFloatParameter( Scopes::SC_QUALITY, 2.0f );

	if( !runPass( plugin, source, sourceWidth, sourceHeight, fbo, width, height ) )
	{
		std::printf( "  FAIL  histogram render\n" );
		++report.checks;
		++report.failures;
		return false;
	}

	const std::vector< unsigned char > image = readBack( fbo, width, height );
	const std::vector< Bar > pattern         = bars( amplitude );

	/// The height of the drawn column at this luma, as a fraction of the tile.
	auto columnHeight = [ & ]( double signal ) {
		const int bin = static_cast< int >( std::lround( std::min( 1.0, std::max( 0.0, signal ) ) * ( kHistogramBins - 1 ) ) );
		const int x   = std::min( width - 1, static_cast< int >( ( bin + 0.5 ) / kHistogramBins * width ) );

		int lit = 0;
		for( int y = 0; y < height; ++y )
			if( brightnessAt( image, width, x, y ) > 0.15 )
				++lit;
		return static_cast< double >( lit ) / height;
	};

	for( size_t i = 0; i < pattern.size(); ++i )
	{
		const Bar bar       = quantised( pattern[ i ] );
		const double signal = luma( bar.r, bar.g, bar.b, matrix );
		const double filled = columnHeight( signal );

		// Every bar is a seventh of the frame, so each of the seven distinct
		// lumas should hold about a seventh of the samples. Against a flat
		// distribution that is 256/7 = 36 times as tall, and the default gain
		// saturates it -- so the check is that the column is there and tall,
		// not what it measures.
		report.check( filled > 0.5, pattern[ i ].name,
		              formatted( "bin at %5.1f IRE is %3.0f%% of full height", signal * 100.0, filled * 100.0 ) );
	}

	// And the converse: a level no bar contains must be empty. Without this the
	// test above passes just as well on a histogram that fills every column.
	const double emptyAt = 0.95;//nothing in 75% bars is at 95 IRE
	report.check( columnHeight( emptyAt ) < 0.02, "empty at 95 IRE",
	              formatted( "%3.0f%% of full height", columnHeight( emptyAt ) * 100.0 ) );

	return true;
}

//---------------------------------------------------------------------------
// --probe: the GPU's false-colour banding against the C++
//---------------------------------------------------------------------------
int probe( Scopes& plugin, GLuint source, int sourceWidth, int sourceHeight,
           GLuint fbo, int width, int height, Matrix matrix, Range range )
{
	// The picture pass samples with GL_LINEAR, which is right -- it is showing
	// the picture, not measuring it, and nothing here is read off it. But it
	// means that at any scale other than 1:1 an output pixel is a blend of two
	// source texels, and this probe's expectation is built from one of them. So
	// the probe renders 1:1 and refuses to run otherwise, rather than
	// quietly comparing the band logic against the resampler.
	if( width != sourceWidth || height != sourceHeight )
	{
		std::fprintf( stderr, "sctest: --probe must render 1:1 (%dx%d)\n", sourceWidth, sourceHeight );
		return 2;
	}

	plugin.SetFloatParameter( Scopes::SC_SCOPE, 3.0f );  //picture assist
	plugin.SetFloatParameter( Scopes::SC_ASSIST, 0.0f ); //false colour

	if( !runPass( plugin, source, sourceWidth, sourceHeight, fbo, width, height ) )
	{
		std::fprintf( stderr, "sctest: probe render failed\n" );
		return 1;
	}

	const std::vector< unsigned char > image = readBack( fbo, width, height );

	int mismatches = 0;
	int compared   = 0;
	const int y    = height / 2;

	for( int x = 0; x < width; ++x )
	{
		// The ramp's value at this output column, recovered exactly as the
		// source built it: 8-bit quantised, so the expectation matches what the
		// shader was actually handed.
		const unsigned char v = static_cast< unsigned char >( x * 255 / std::max( 1, sourceWidth - 1 ) );

		// Through the same two steps the shader takes, in the same order:
		// received -> signal, then signal -> luma. Skipping the range mapping
		// would make this a check that only ever ran at full range, and pass
		// vacuously everywhere else.
		const AffineMapping map = receivedToSignal( range );
		const double received   = v / 255.0;
		const double level      = received * map.scale + map.offset;
		const double signal     = luma( level, level, level, matrix );
		const double ire        = signal * 100.0;

		// Skip the columns within half a band boundary: there the two sides
		// disagree over a rounding difference of one 8-bit level rather than
		// over the banding logic, and counting those would be measuring the
		// ramp's quantisation instead of the shader.
		bool nearBoundary = false;
		for( const FalseColourBand& band : falseColourBands() )
			if( band.fromIre > -1000.0 && std::fabs( ire - band.fromIre ) < 0.25 )
				nearBoundary = true;
		if( nearBoundary )
			continue;

		const FalseColourBand& expected = falseColourBandFor( ire );

		const size_t index = ( static_cast< size_t >( y ) * width + x ) * 4;
		const double dr    = image[ index + 0 ] / 255.0 - expected.colour[ 0 ];
		const double dg    = image[ index + 1 ] / 255.0 - expected.colour[ 1 ];
		const double db    = image[ index + 2 ] / 255.0 - expected.colour[ 2 ];
		const double error = std::sqrt( dr * dr + dg * dg + db * db );

		++compared;
		if( error > 0.01 )
		{
			if( mismatches < 8 )
				std::printf( "  x=%4d  %6.2f IRE  expected %-10s error %.3f\n",
				             x, ire, expected.label, error );
			++mismatches;
		}
	}

	std::printf( "\n  %d of %d columns disagree with falseColourBandFor()\n", mismatches, compared );
	if( mismatches == 0 )
		std::printf( "  the GLSL band loop and the C++ agree everywhere they were asked\n" );

	return mismatches == 0 ? 0 : 1;
}

void usage()
{
	std::printf(
		"sctest -- render and check the Scopes plugin\n"
		"\n"
		"  --verify          check every scope against 75%% bars, then exit\n"
		"  --probe           check the GPU's false colour against the C++\n"
		"  --list            print every parameter and its default, then exit\n"
		"  --out PATH        where to write (default /tmp/scopes.png)\n"
		"  --width N         output width (default 1280)\n"
		"  --height N        output height (default 720)\n"
		"  --ramp            render the neutral ramp instead of bars\n"
		"  --gradient        render a 2D gradient (what sweep.py needs)\n"
		"  --alpha V         premultiply the source by this alpha, as Resolume would\n"
		"  --amplitude V     bar amplitude, 0.75 or 1.0 (default 0.75)\n"
		"  --set \"Name=V\"    set a parameter by its display name, 0..1\n" );
}

} // namespace

int main( int argc, char** argv )
{
	std::string outputPath = "/tmp/scopes.png";
	int width              = 1280;
	int height             = 720;
	double amplitude       = 0.75;
	double sourceAlpha     = 1.0;
	bool useRamp           = false;
	bool useGradient       = false;
	bool doVerify          = false;
	bool doProbe           = false;
	bool listOnly          = false;
	std::vector< std::pair< std::string, float > > overrides;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		if( arg == "--out" && i + 1 < argc )
			outputPath = argv[ ++i ];
		else if( arg == "--width" && i + 1 < argc )
			width = std::atoi( argv[ ++i ] );
		else if( arg == "--height" && i + 1 < argc )
			height = std::atoi( argv[ ++i ] );
		else if( arg == "--amplitude" && i + 1 < argc )
			amplitude = std::atof( argv[ ++i ] );
		else if( arg == "--ramp" )
			useRamp = true;
		else if( arg == "--gradient" )
			useGradient = true;
		else if( arg == "--alpha" && i + 1 < argc )
			sourceAlpha = std::atof( argv[ ++i ] );
		else if( arg == "--verify" )
			doVerify = true;
		else if( arg == "--probe" )
			doProbe = true;
		else if( arg == "--list" )
			listOnly = true;
		else if( arg == "--set" && i + 1 < argc )
		{
			const std::string assignment = argv[ ++i ];
			const size_t equals          = assignment.find( '=' );
			if( equals == std::string::npos )
			{
				std::fprintf( stderr, "sctest: --set wants \"Name=value\"\n" );
				return 2;
			}
			overrides.emplace_back( assignment.substr( 0, equals ),
			                        static_cast< float >( std::atof( assignment.c_str() + equals + 1 ) ) );
		}
		else if( arg == "--help" || arg == "-h" )
		{
			usage();
			return 0;
		}
		else
		{
			std::fprintf( stderr, "sctest: unrecognised argument '%s'\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 )
	{
		std::fprintf( stderr, "sctest: width and height must be positive\n" );
		return 2;
	}

	// 7 bars over a width divisible by 7, so no bar boundary lands mid-pixel and
	// the expected reading is not being compared against a blend of two bars.
	const int sourceWidth  = 896;
	const int sourceHeight = 504;

	if( doProbe )
	{
		width  = sourceWidth;
		height = sourceHeight;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "sctest: could not create a GL 4.1 core context\n" );
		return 1;
	}

	Scopes plugin;

	auto indexOfParameter = [ & ]( const std::string& name ) -> int {
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
		{
			const char* declared = plugin.GetParamName( i );
			if( declared != nullptr && name == declared )
				return static_cast< int >( i );
		}
		return -1;
	};

	if( listOnly )
	{
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
			std::printf( "%2u  %-16s %.3f\n", i, plugin.GetParamName( i ), plugin.GetFloatParameter( i ) );
		return 0;
	}

	FFGLViewportStruct viewport = {};
	viewport.x                  = 0;
	viewport.y                  = 0;
	viewport.width              = static_cast< GLuint >( width );
	viewport.height             = static_cast< GLuint >( height );
	if( plugin.InitGL( &viewport ) != FF_SUCCESS )
	{
		std::fprintf( stderr, "sctest: InitGL failed -- a shader did not compile.\n"
		                      "        The diagnostics log names which one.\n" );
		return 1;
	}

	// 7 bars over a width divisible by 7, so no bar boundary lands mid-pixel and
	// the expected reading is not being compared against a blend of two bars.
	std::vector< unsigned char > picture;
	if( doProbe || useRamp )
		picture = rampPicture( sourceWidth, sourceHeight );
	else if( useGradient )
		picture = gradientPicture( sourceWidth, sourceHeight );
	else
		picture = barsPicture( sourceWidth, sourceHeight, amplitude );

	if( sourceAlpha < 1.0 )
		premultiply( picture, sourceAlpha );

	const GLuint sourceTexture = makeTexture( sourceWidth, sourceHeight, picture.data() );
	const GLuint targetTexture = makeTexture( width, height, nullptr );
	const GLuint targetFBO     = makeFramebuffer( targetTexture );

	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		std::fprintf( stderr, "sctest: target framebuffer is incomplete\n" );
		return 1;
	}

	for( const auto& override : overrides )
	{
		const int index = indexOfParameter( override.first );
		if( index < 0 )
		{
			std::fprintf( stderr, "sctest: no parameter named '%s' (try --list)\n", override.first.c_str() );
			return 2;
		}
		plugin.SetFloatParameter( static_cast< unsigned int >( index ), override.second );
	}

	const Matrix matrix = optionFrom< Matrix >( plugin.GetFloatParameter( Scopes::SC_MATRIX ), 3 );

	if( doProbe )
	{
		const Range range = optionFrom< Range >( plugin.GetFloatParameter( Scopes::SC_RANGE ), 2 );
		std::printf( "probe: the GLSL false-colour bands against falseColourBandFor()\n"
		             "  matrix %s, %s range, neutral ramp at 1:1\n\n",
		             matrixName( matrix ), range == Range::Full ? "full" : "limited" );
		const int result = probe( plugin, sourceTexture, sourceWidth, sourceHeight,
		                          targetFBO, width, height, matrix, range );
		plugin.DeInitGL();
		return result;
	}

	if( doVerify )
	{
		std::printf( "verify: %.0f%% bars, %s, full range\n"
		             "  the expected readings are derived from Colorimetry.cpp, which is also\n"
		             "  where the shaders' uniforms and the graticule come from.\n\n",
		             amplitude * 100.0, matrixName( matrix ) );

		Report report;

		std::printf( "waveform (luma) -- does the trace land on each bar's luma?\n" );
		verifyWaveform( report, plugin, sourceTexture, sourceWidth, sourceHeight,
		                targetFBO, width, height, matrix, amplitude );

		std::printf( "\nvectorscope -- does the trace land in each bar's box?\n" );
		verifyVectorscope( report, plugin, sourceTexture, sourceWidth, sourceHeight,
		                   targetFBO, width, height, matrix, amplitude );

		std::printf( "\nhistogram (luma) -- does it spike at each bar's level?\n" );
		verifyHistogram( report, plugin, sourceTexture, sourceWidth, sourceHeight,
		                 targetFBO, width, height, matrix, amplitude );

		std::printf( "\n%d checks, %d failed\n", report.checks, report.failures );
		plugin.DeInitGL();
		return report.failures == 0 ? 0 : 1;
	}

	if( !runPass( plugin, sourceTexture, sourceWidth, sourceHeight, targetFBO, width, height ) )
	{
		std::fprintf( stderr, "sctest: ProcessOpenGL failed\n" );
		return 1;
	}

	std::vector< unsigned char > out = readBack( targetFBO, width, height );

	// Composited over black so the PNG shows what the operator sees on a layer
	// rather than a mostly transparent image that a viewer renders as white.
	for( size_t i = 0; i < out.size(); i += 4 )
	{
		const double a = out[ i + 3 ] / 255.0;
		for( int c = 0; c < 3; ++c )
			out[ i + c ] = static_cast< unsigned char >( out[ i + c ] * a + 0.5 );
		out[ i + 3 ] = 255;
	}

	if( !writePng( outputPath, width, height, out ) )
	{
		std::fprintf( stderr, "sctest: could not write %s\n", outputPath.c_str() );
		return 1;
	}

	std::printf( "wrote %s (%dx%d)\n", outputPath.c_str(), width, height );

	plugin.DeInitGL();
	return 0;
}
