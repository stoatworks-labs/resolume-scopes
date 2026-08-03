/**
 * Scopes — browser demo.
 *
 * All seven shader programs below are assembled from the same pieces
 * `source/Shaders.cpp` assembles them from — the signal-model prelude, the
 * shared sampling block, and one body per pass — copied across unedited.
 * `source/Colorimetry.cpp`, `source/Graticule.cpp` and `source/Layout.cpp` are
 * ported, and `ProcessOpenGL` in `source/Scopes.cpp` is the shape of the
 * renderer.
 *
 * **The design rule, which is the whole reason this plugin is careful:** Matrix,
 * Range and Unpremultiply are parameters, never heuristics. By the time an FFGL
 * effect sees the texture, the engine has already chosen a matrix, decided about
 * studio levels, and premultiplied by alpha — and recorded none of it. Each one
 * guessed wrong is *plausibly* wrong rather than obviously wrong: 709-as-601
 * throws the vectorscope targets 5.7° off, unexpanded studio levels put white at
 * 92 IRE, and a 50%-opacity layer reads a stop down.
 *
 * No colorimetry constant is in the GLSL. The coefficients and the range mapping
 * are uniforms out of the ported `Colorimetry`, and the graticule is built from
 * the same module — which is what makes "the trace lands in the box" a real
 * check rather than a tautology. Put the Colour bars clip up, switch Matrix, and
 * watch the boxes and the trace move together.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, Quad, bindTexture } from './vendor/gl.js';

//===========================================================================
// Ports of source/Colorimetry.{h,cpp}
//===========================================================================

const STUDIO_BLACK_8BIT = 16;
const STUDIO_WHITE_8BIT = 235;
const STUDIO_LUMA_SWING = STUDIO_WHITE_8BIT - STUDIO_BLACK_8BIT; // 219

const WAVEFORM_IRE_MIN = -10;
const WAVEFORM_IRE_MAX = 110;
const VECTOR_FULL_SCALE = 0.5;
const HISTOGRAM_BINS = 256;
const MAX_SAMPLES = 1920 * 1080;
const MAX_FALSE_COLOUR_BANDS = 16;

/// Only kr and kb are written down. kg is derived.
const MATRIX_CONSTANTS = [
  { kr: 0.299, kb: 0.114 },    // BT.601
  { kr: 0.2126, kb: 0.0722 },  // BT.709
  { kr: 0.2627, kb: 0.0593 },  // BT.2020
];

function coefficients(matrix) {
  const k = MATRIX_CONSTANTS[matrix] ?? MATRIX_CONSTANTS[1];
  return { kr: k.kr, kg: 1 - k.kr - k.kb, kb: k.kb };
}

function rgbToYCbCr(r, g, b, matrix) {
  const k = coefficients(matrix);
  const y = k.kr * r + k.kg * g + k.kb * b;
  return { y, cb: (b - y) / (2 * (1 - k.kb)), cr: (r - y) / (2 * (1 - k.kr)) };
}

function yCbCrToRgb(y, cb, cr, matrix) {
  const k = coefficients(matrix);
  const r = y + cr * 2 * (1 - k.kr);
  const b = y + cb * 2 * (1 - k.kb);
  const g = (y - k.kr * r - k.kb * b) / k.kg;
  return [r, g, b];
}

function degreesOf(cb, cr) {
  const deg = (Math.atan2(cr, cb) * 180) / Math.PI;
  return deg < 0 ? deg + 360 : deg;
}

/// Identity for full range; undoes 16-235 for studio range. Recovered as
/// (f(1) - f(0), f(0)) from the one place the anchors are written down, rather
/// than restated as a second pair of magic numbers.
function receivedToSignal(range) {
  if (range === 0) return { scale: 1, offset: 0 };
  const f = (v) => (v * 255 - STUDIO_BLACK_8BIT) / STUDIO_LUMA_SWING;
  return { scale: f(1) - f(0), offset: f(0) };
}

const WAVEFORM_GRATICULE_IRE = [-10, 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110];

/// R'G'B' switch pattern for each bar, at unit amplitude, in the order a
/// vectorscope's targets are conventionally read.
const BAR_ORDER = [
  [1, 0, 0], // red
  [1, 0, 1], // magenta
  [0, 0, 1], // blue
  [0, 1, 1], // cyan
  [0, 1, 0], // green
  [1, 1, 0], // yellow
];

function vectorTargets(matrix, amplitude) {
  return BAR_ORDER.map((rgb) => {
    const c = rgbToYCbCr(rgb[0] * amplitude, rgb[1] * amplitude, rgb[2] * amplitude, matrix);
    return { cb: c.cb, cr: c.cr, deg: degreesOf(c.cb, c.cr), mag: Math.hypot(c.cb, c.cr) };
  });
}

/**
 * The skin-tone line, at whatever angle this matrix puts the NTSC I axis.
 *
 * 123° is a BT.601 number, so it cannot simply be drawn at 123° on a scope
 * plotting BT.709 chroma — the same physical colour lands somewhere else. Taken
 * here as a *pure chroma* direction, y = 0, because chroma alone does not
 * determine an RGB colour and "the same direction under a different matrix" is
 * only well defined on the zero-luma plane. The RGB below has negative
 * components and is not a colour; it is a direction.
 */
function skinToneAxisDeg(matrix) {
  const rad = (123 * Math.PI) / 180;
  const direction = yCbCrToRgb(0, Math.cos(rad), Math.sin(rad), 0);
  const c = rgbToYCbCr(direction[0], direction[1], direction[2], matrix);
  return degreesOf(c.cb, c.cr);
}

const FALSE_COLOUR_BANDS = [
  { fromIre: -1e9, colour: [0.25, 0.0, 0.5], name: 'sub-black' },
  { fromIre: 0.0, colour: [0.0, 0.0, 0.75], name: 'crushed' },
  { fromIre: 5.0, colour: [0.0, 0.55, 0.85], name: 'shadow' },
  { fromIre: 20.0, colour: [0.35, 0.35, 0.35], name: 'low mid' },
  { fromIre: 45.0, colour: [0.55, 0.55, 0.55], name: 'mid' },
  { fromIre: 55.0, colour: [0.75, 0.75, 0.75], name: 'high mid' },
  { fromIre: 70.0, colour: [0.95, 0.85, 0.0], name: 'highlight' },
  { fromIre: 90.0, colour: [0.95, 0.45, 0.0], name: 'near clip' },
  { fromIre: 100.0, colour: [0.9, 0.0, 0.0], name: 'clip' },
];

//===========================================================================
// Ports of source/Layout.cpp
//===========================================================================

function sampleGridFor(pictureWidth, pictureHeight, quality) {
  if (pictureWidth <= 0 || pictureHeight <= 0) return { columns: 1, rows: 1 };

  let stride = quality === 0 ? 4 : quality === 1 ? 2 : 1;

  // Grow the stride until the budget is met. Doubling rather than solving for
  // it keeps the grid on the same lattice the requested stride was on, so the
  // samples stay evenly spread instead of landing on a near-resonant pattern
  // with the source's own structure.
  for (;;) {
    const columns = Math.max(1, Math.floor(pictureWidth / stride));
    const rows = Math.max(1, Math.floor(pictureHeight / stride));
    if (columns * rows <= MAX_SAMPLES || stride >= 64) return { columns, rows };
    stride *= 2;
  }
}

function traceGain(param, sampleCount) {
  if (sampleCount <= 0) return 0;
  // Perceptually the useful range is wide — a flat grey field puts every one of
  // its samples on one waveform row, a busy picture spreads them over hundreds
  // — so the control is exponential rather than linear.
  const density = 10 ** (-1 + 2 * param); // 0.1 .. 10
  // Normalised against a 1080p-worth of samples so that Quality changes the
  // measurement's resolution without changing how bright it looks.
  return (density * 1920 * 1080) / sampleCount * 0.02;
}

/// 1x at 0, 8x at 1, exponentially — so 2x sits at 1/3 and 4x at 2/3 and the
/// useful low end is not crammed into the first few pixels of the slider.
const vectorZoom = (param) => 8 ** param;

/**
 * Calibrated against real pictures, not against the arithmetic.
 *
 * A bin's height is (its share of the samples) × bins × gain, so a *flat*
 * distribution reaches exactly `gain` — and gain 1 therefore means a flat
 * histogram already fills the tile. Real footage is nowhere near flat: a
 * picture typically puts 2-5% of its pixels in its busiest bin against the 0.4%
 * a flat one would. So the useful gains are well *below* 1.
 */
function histogramNorm(param, sampleCount, bins) {
  if (sampleCount <= 0 || bins <= 0) return 0;
  const gain = 10 ** (-2.2 + 2.4 * param); // 0.006 .. 1.6
  return (gain * bins) / sampleCount;
}

const histogramChannels = (mode) =>
  mode === 1 ? { base: 3, count: 1 } : mode === 2 ? { base: 0, count: 4 } : { base: 0, count: 3 };

const zebraIre = (param) => 50 + 60 * param;
/// A Sobel on 0..1 luma maxes out around 4 for a hard black-to-white edge, and
/// the response of interest is well down at the bottom of that.
const peakThreshold = (param) => 10 ** (-1.7 + 2 * param);

function scopeRect(layout, size, x, y, outputAspect) {
  if (layout === 0) return { x: 0, y: 0, width: 1, height: 1 };
  if (outputAspect <= 0) outputAspect = 1;

  // Size spans the shorter dimension, so a given setting looks the same on a
  // 16:9 composition and on a square one.
  const span = 0.15 + 0.7 * Math.min(1, Math.max(0, size));
  let width = span;
  let height = span;
  if (outputAspect > 1) width = span / outputAspect;
  else height = span * outputAspect;

  // X and Y run edge to edge over what is left, so the overlay is always wholly
  // on screen: there is no setting that parks the scope off the frame.
  const px = Math.min(1, Math.max(0, x));
  const py = Math.min(1, Math.max(0, y));
  return { x: px * (1 - width), y: py * (1 - height), width, height };
}

//===========================================================================
// Ports of source/Graticule.cpp
//===========================================================================

/// The two weights the graticule speaks in. There is no text on it, so a line's
/// brightness is the only thing that can say whether it is a reference or a
/// ruling.
const REFERENCE = 1.0;
const RULING = 0.35;

const segment = (out, x0, y0, x1, y1, brightness) => {
  out.push(x0, y0, brightness, x1, y1, brightness);
};

/// Aspect correction, kept identical to the vectorscope vertex shader. If these
/// two ever disagree the trace slides under a stationary graticule — which reads
/// as a badly calibrated scope rather than as a bug.
function applyAspect(p, aspect) {
  if (aspect > 1) p[0] /= aspect;
  else p[1] *= aspect;
  return p;
}

function circle(out, radius, aspect, brightness, steps = 128) {
  let px = 0;
  let py = 0;
  for (let i = 0; i <= steps; i += 1) {
    const theta = (2 * Math.PI * i) / steps;
    const [x, y] = applyAspect([radius * Math.cos(theta), radius * Math.sin(theta)], aspect);
    if (i > 0) segment(out, px, py, x, y, brightness);
    px = x;
    py = y;
  }
}

/// A target box, drawn in the aspect-corrected space so it stays square on
/// screen rather than being stretched with the plot.
function box(out, cx, cy, half, aspect, brightness) {
  let hx = half;
  let hy = half;
  if (aspect > 1) hx /= aspect;
  else hy *= aspect;

  segment(out, cx - hx, cy - hy, cx + hx, cy - hy, brightness);
  segment(out, cx + hx, cy - hy, cx + hx, cy + hy, brightness);
  segment(out, cx + hx, cy + hy, cx - hx, cy + hy, brightness);
  segment(out, cx - hx, cy + hy, cx - hx, cy - hy, brightness);
}

/// IRE to the tile's NDC, using the same range the waveform vertex shader maps
/// with.
const ireToNdc = (ire) =>
  ((ire - WAVEFORM_IRE_MIN) / (WAVEFORM_IRE_MAX - WAVEFORM_IRE_MIN)) * 2 - 1;

function waveformGraticule(mode) {
  const out = [];

  for (const ire of WAVEFORM_GRATICULE_IRE) {
    // 0 and 100 IRE are what the operator actually reads against; the rest are
    // there to make a level estimable, not to be looked at.
    const isReference = ire === 0 || ire === 100;
    const y = ireToNdc(ire);
    segment(out, -1, y, 1, y, isReference ? REFERENCE : RULING);
  }

  if (mode !== 0) {
    for (let i = 1; i < 3; i += 1) {
      const x = (i / 3) * 2 - 1;
      segment(out, x, -1, x, 1, REFERENCE);
    }
  }

  if (mode === 2) {
    // The chroma panels are bipolar and centred on 50 IRE, so on those two the
    // centre line means chroma zero — a different thing from the 50 IRE ruling
    // it coincides with. Drawn across the two right-hand panels only, which is
    // the only way to say so without text.
    const y = ireToNdc(50);
    segment(out, -1 / 3, y, 1, y, REFERENCE);
  }

  return out;
}

function vectorscopeGraticule(matrix, amplitude, zoom, aspect) {
  const out = [];

  // The ring the targets sit on. At 75% bars that is the 75% circle, so it
  // lands on the boxes rather than somewhere they can be compared against by eye
  // and found not to match.
  circle(out, amplitude * zoom, aspect, RULING);
  // Full scale: anything outside this ring is chroma the signal has no business
  // containing.
  circle(out, 1 * zoom, aspect, REFERENCE);

  {
    const r = 1.05 * zoom;
    const a = applyAspect([-r, 0], aspect);
    const b = applyAspect([r, 0], aspect);
    segment(out, a[0], a[1], b[0], b[1], RULING);
    const c = applyAspect([0, -r], aspect);
    const d = applyAspect([0, r], aspect);
    segment(out, c[0], c[1], d[0], d[1], RULING);
  }

  {
    const rad = (skinToneAxisDeg(matrix) * Math.PI) / 180;
    const p = applyAspect([Math.cos(rad) * zoom, Math.sin(rad) * zoom], aspect);
    segment(out, 0, 0, p[0], p[1], REFERENCE);
  }

  // The six boxes, from the same matrix the shader plots with.
  for (const target of vectorTargets(matrix, amplitude)) {
    const p = applyAspect(
      [(target.cb / VECTOR_FULL_SCALE) * zoom, (target.cr / VECTOR_FULL_SCALE) * zoom],
      aspect,
    );
    // A real scope's boxes are its stated tolerance. This one has no calibration
    // to state, so the box is simply a legible target — 2.5% of full scale.
    box(out, p[0], p[1], 0.025 * zoom, aspect, REFERENCE);
  }

  return out;
}

function histogramGraticule() {
  const out = [];
  for (let i = 0; i <= 4; i += 1) {
    const x = (i / 4) * 2 - 1;
    // The ends are black and nominal white; the quarters are only a ruler.
    segment(out, x, -1, x, 1, i === 0 || i === 4 ? REFERENCE : RULING);
  }
  return out;
}

//===========================================================================
// Shaders — verbatim from source/Shaders.cpp, assembled the same way
//===========================================================================

const PRELUDE = `#version 410 core

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
`;

const SAMPLING = `
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
`;

const WAVEFORM_VERTEX_BODY = `
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
`;

const VECTORSCOPE_VERTEX_BODY = `
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
`;

const TRACE_FRAGMENT_BODY = `
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
`;

const HISTOGRAM_ACCUM_VERTEX_BODY = `
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
`;

const HISTOGRAM_ACCUM_FRAGMENT = `#version 410 core

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
`;

const HISTOGRAM_DRAW_VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	uv          = vUV;
	gl_Position = vPosition;
}
`;

const HISTOGRAM_DRAW_FRAGMENT = `#version 410 core

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
	//single most useful thing an RGB histogram can tell you.
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
`;

const PICTURE_VERTEX = `#version 410 core

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
`;

const PICTURE_FRAGMENT_BODY = `
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
`;

const LINE_VERTEX = `#version 410 core

layout( location = 0 ) in vec2 vPosition;
layout( location = 1 ) in float vBrightness;

out float brightness;

void main()
{
	brightness  = vBrightness;
	gl_Position = vec4( vPosition, 0.0, 1.0 );
}
`;

const LINE_FRAGMENT = `#version 410 core

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
`;

const COMPOSITE_VERTEX = `#version 410 core

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
`;

const COMPOSITE_FRAGMENT = `#version 410 core

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
`;

const assemble = (sampling, body) => PRELUDE + (sampling ?? '') + body;

//===========================================================================
// The renderer — a port of ProcessOpenGL in source/Scopes.cpp
//===========================================================================

function createRenderer(gl, quad) {
  const waveformShader = new Program(gl, assemble(SAMPLING, WAVEFORM_VERTEX_BODY), assemble(null, TRACE_FRAGMENT_BODY), 'waveform');
  const vectorscopeShader = new Program(gl, assemble(SAMPLING, VECTORSCOPE_VERTEX_BODY), assemble(null, TRACE_FRAGMENT_BODY), 'vectorscope');
  const histogramAccumShader = new Program(gl, assemble(SAMPLING, HISTOGRAM_ACCUM_VERTEX_BODY), HISTOGRAM_ACCUM_FRAGMENT, 'histogram accumulate');
  const histogramDrawShader = new Program(gl, HISTOGRAM_DRAW_VERTEX, HISTOGRAM_DRAW_FRAGMENT, 'histogram draw');
  const pictureShader = new Program(gl, PICTURE_VERTEX, assemble(null, PICTURE_FRAGMENT_BODY), 'picture');
  const lineShader = new Program(gl, LINE_VERTEX, LINE_FRAGMENT, 'graticule', {
    attribs: { vPosition: 0, vBrightness: 1 },
  });
  const compositeShader = new Program(gl, COMPOSITE_VERTEX, COMPOSITE_FRAGMENT, 'composite');

  const scopeBuffer = new PassBuffer(gl, { filter: 'linear' });
  const binBuffer = new PassBuffer(gl, { filter: 'nearest' });

  // The scatter passes read everything from gl_VertexID and source no
  // attributes at all, but a core profile still refuses to draw without a
  // vertex array object bound. This one stays empty on purpose.
  const scatterVAO = gl.createVertexArray();

  const lineVAO = gl.createVertexArray();
  const lineVBO = gl.createBuffer();
  gl.bindVertexArray(lineVAO);
  gl.bindBuffer(gl.ARRAY_BUFFER, lineVBO);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 12, 0);
  gl.enableVertexAttribArray(1);
  gl.vertexAttribPointer(1, 1, gl.FLOAT, false, 12, 8);
  gl.bindVertexArray(null);
  gl.bindBuffer(gl.ARRAY_BUFFER, null);

  // The false-colour bands, uploaded once. -1e9 for the bottom band would come
  // through a float uniform as -inf on some drivers and compare unhelpfully, so
  // it is floored at a value no IRE reading can go below.
  const bandCount = Math.min(FALSE_COLOUR_BANDS.length, MAX_FALSE_COLOUR_BANDS);
  const bandIre = new Float32Array(MAX_FALSE_COLOUR_BANDS);
  const bandColour = new Float32Array(MAX_FALSE_COLOUR_BANDS * 3);
  for (let i = 0; i < bandCount; i += 1) {
    bandIre[i] = Math.max(FALSE_COLOUR_BANDS[i].fromIre, -1000);
    bandColour.set(FALSE_COLOUR_BANDS[i].colour, i * 3);
  }

  function setSignalUniforms(shader, params) {
    const matrix = Math.round(params.get('matrix'));
    const range = Math.round(params.get('range'));
    const k = coefficients(matrix);
    const map = receivedToSignal(range);
    shader.set('LumaCoeff', k.kr, k.kg, k.kb);
    shader.set('RangeMap', map.scale, map.offset);
    shader.set('Unpremultiply', params.get('unpremultiply') > 0.5 ? 1 : 0);
  }

  function setSamplingUniforms(shader, input, grid, pointSize) {
    shader.setSampler('InputTexture', 0);
    shader.set('PictureSize', input.width, input.height);
    shader.set('SampleGrid', grid.columns, grid.rows);
    shader.set('PointSize', pointSize);
  }

  function drawGraticule(vertices, gain) {
    if (!vertices.length || gain <= 0) return;

    lineShader.use();
    lineShader.set('LineColour', 0.65, 0.72, 0.78);
    lineShader.set('LineGain', gain);

    gl.bindVertexArray(lineVAO);
    gl.bindBuffer(gl.ARRAY_BUFFER, lineVBO);
    // Re-uploaded every frame rather than cached against the parameters it
    // depends on. It is a few hundred vertices, and a cache keyed on "did any of
    // matrix, bars, zoom, aspect or mode change" is exactly the kind of
    // invalidation nobody gets right twice — a stale graticule would be a wrong
    // reading that looks perfectly plausible.
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(vertices), gl.STREAM_DRAW);
    gl.drawArrays(gl.LINES, 0, vertices.length / 3);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
    gl.bindVertexArray(null);
  }

  return {
    render({ input, params, width, height }) {
      const scope = Math.round(params.get('scope'));
      const layout = Math.round(params.get('layout'));
      const quality = Math.round(params.get('quality'));
      const matrix = Math.round(params.get('matrix'));
      const wfMode = Math.round(params.get('waveform'));
      const hgMode = Math.round(params.get('histogram'));
      const asMode = Math.round(params.get('assist'));

      const outputAspect = width / height;

      bindTexture(gl, 0, input.texture);

      // ---------------------------------------------------------------------
      // Picture Assist is not a scope with a rectangle — it is the picture with
      // a different fragment shader on it. It shares nothing below.
      // ---------------------------------------------------------------------
      if (scope === 3) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, width, height);
        gl.disable(gl.BLEND);

        pictureShader.use();
        setSignalUniforms(pictureShader, params);
        pictureShader.setSampler('InputTexture', 0);
        pictureShader.set('MaxUV', 1, 1);
        pictureShader.set('Texel', 1 / input.width, 1 / input.height);
        pictureShader.set('Overlay', asMode + 1);
        pictureShader.set('ZebraLevel', zebraIre(params.get('assistLevel')));
        pictureShader.set('PeakThreshold', peakThreshold(params.get('assistLevel')));
        pictureShader.set('BandCount', bandCount);
        pictureShader.setArray('BandIre', bandIre, 1);
        pictureShader.setArray('BandColour', bandColour, 3);

        quad.draw();
        return;
      }

      // ---------------------------------------------------------------------
      // Everything else: draw the scope into its own buffer, then composite.
      // ---------------------------------------------------------------------
      const rect = scopeRect(layout, params.get('size'), params.get('x'), params.get('y'), outputAspect);
      const scopeWidth = Math.max(16, Math.round(rect.width * width));
      const scopeHeight = Math.max(16, Math.round(rect.height * height));

      // RGBA16F, not 32F. The trace is a picture: half-float carries it with
      // room to spare, and the one place precision genuinely matters — the
      // histogram's bins — is a separate 32F buffer below.
      scopeBuffer.ensure(scopeWidth, scopeHeight, gl.RGBA16F);

      const grid = sampleGridFor(input.width, input.height, quality);
      const sampleCount = grid.columns * grid.rows;

      // The background is written premultiplied, like everything else that
      // lands in this buffer.
      scopeBuffer.clearTo(0, 0, 0, params.get('background'));

      scopeBuffer.bind();
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.ONE, gl.ONE);

      const graticuleGain = params.get('graticule');
      const barAmplitude = params.get('bars') > 0.5 ? 1.0 : 0.75;
      const zoom = vectorZoom(params.get('zoom'));
      const scopeAspect = scopeWidth / scopeHeight;

      if (scope === 0) {
        drawGraticule(waveformGraticule(wfMode), graticuleGain);

        const segments = wfMode === 0 ? 1 : 3;

        // Wide enough to close the gaps a sparse sample grid leaves, in both
        // directions, because they arise for two unrelated reasons: horizontally
        // the spacing is fixed at one trace column per sample column, and
        // vertically the worst case is a column holding a smooth sweep of the
        // whole range. Rounded up, because a driver is free to round
        // gl_PointSize to an integer and most do — a pitch of 1.4 asks for a
        // 1.4-pixel point, gets a 1-pixel one, and closes nothing.
        const columnPitch = scopeWidth / (segments * Math.max(1, grid.columns));
        const valuePitch = scopeHeight / Math.max(1, grid.rows);
        const pointSize = Math.max(1, Math.ceil(Math.max(columnPitch, valuePitch)));

        waveformShader.use();
        setSignalUniforms(waveformShader, params);
        setSamplingUniforms(waveformShader, input, grid, pointSize);
        waveformShader.set('Segments', segments);
        waveformShader.set('Mode', wfMode);
        waveformShader.set('IreRange', WAVEFORM_IRE_MIN, WAVEFORM_IRE_MAX);
        waveformShader.set('Intensity', traceGain(params.get('intensity'), sampleCount));

        gl.bindVertexArray(scatterVAO);
        gl.drawArrays(gl.POINTS, 0, sampleCount * segments);
        gl.bindVertexArray(null);
      } else if (scope === 1) {
        drawGraticule(vectorscopeGraticule(matrix, barAmplitude, zoom, scopeAspect), graticuleGain);

        // Bigger points than the waveform, deliberately. A waveform spreads its
        // samples over hundreds of columns; every sample of a flat colour lands
        // on one vectorscope coordinate, so at one pixel a colour bar is a
        // single dot.
        const pointSize = Math.max(1.5, scopeHeight / 300);

        vectorscopeShader.use();
        setSignalUniforms(vectorscopeShader, params);
        setSamplingUniforms(vectorscopeShader, input, grid, pointSize);
        vectorscopeShader.set('FullScale', VECTOR_FULL_SCALE);
        vectorscopeShader.set('Zoom', zoom);
        vectorscopeShader.set('Aspect', scopeAspect);
        vectorscopeShader.set('Intensity', traceGain(params.get('intensity'), sampleCount));

        gl.bindVertexArray(scatterVAO);
        gl.drawArrays(gl.POINTS, 0, sampleCount);
        gl.bindVertexArray(null);
      } else if (scope === 2) {
        const channels = histogramChannels(hgMode);

        // RGBA32F, and it has to be. A histogram accumulated in half-float
        // silently stops counting: ~11 mantissa bits means adding 1.0 to a bin
        // holding 4096 rounds back to 4096, and the bins that stall first are
        // the tallest — which are exactly the clipped whites and crushed blacks
        // the histogram was opened to find. Exact filtering too: a linear tap
        // across the bin texture averages neighbouring bins and reports counts
        // that were never measured.
        binBuffer.ensure(HISTOGRAM_BINS, 1, gl.RGBA32F);
        binBuffer.clearTo(0, 0, 0, 0);

        gl.enable(gl.BLEND);
        gl.blendFunc(gl.ONE, gl.ONE);

        histogramAccumShader.use();
        setSignalUniforms(histogramAccumShader, params);
        setSamplingUniforms(histogramAccumShader, input, grid, 1);
        histogramAccumShader.set('Bins', HISTOGRAM_BINS);
        histogramAccumShader.set('ChannelBase', channels.base);

        gl.bindVertexArray(scatterVAO);
        gl.drawArrays(gl.POINTS, 0, sampleCount * channels.count);
        gl.bindVertexArray(null);

        scopeBuffer.bind();
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.ONE, gl.ONE);
        drawGraticule(histogramGraticule(), graticuleGain);

        histogramDrawShader.use();
        bindTexture(gl, 1, binBuffer.texture);
        histogramDrawShader.setSampler('BinTexture', 1);
        histogramDrawShader.set('Norm', histogramNorm(params.get('intensity'), sampleCount, HISTOGRAM_BINS));
        histogramDrawShader.set(
          'ChannelMask',
          hgMode === 1 ? 0 : 1,
          hgMode === 1 ? 0 : 1,
          hgMode === 1 ? 0 : 1,
          hgMode === 0 ? 0 : 1,
        );
        quad.draw();
        bindTexture(gl, 1, null);
      }

      // ---------------------------------------------------------------------
      // Onto the output.
      // ---------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);

      if (layout === 1) {
        gl.disable(gl.BLEND);

        bindTexture(gl, 0, input.texture);
        pictureShader.use();
        setSignalUniforms(pictureShader, params);
        pictureShader.setSampler('InputTexture', 0);
        pictureShader.set('MaxUV', 1, 1);
        pictureShader.set('Texel', 1 / input.width, 1 / input.height);
        pictureShader.set('Overlay', 0);
        pictureShader.set('BandCount', 0);
        pictureShader.set('ZebraLevel', 1000);
        pictureShader.set('PeakThreshold', 1000);
        quad.draw();
      } else {
        // Scope Only replaces the picture. Cleared to transparent rather than to
        // black: the scope's own Background then decides whether the layer is
        // opaque, and at Background = 0 the trace composites over the layers
        // underneath.
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
      }

      gl.enable(gl.BLEND);
      gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);

      compositeShader.use();
      bindTexture(gl, 1, scopeBuffer.texture);
      compositeShader.setSampler('ScopeTexture', 1);
      compositeShader.set('Opacity', params.get('opacity'));
      compositeShader.set('Placement', rect.x, rect.y, rect.width, rect.height);
      quad.draw();

      bindTexture(gl, 1, null);
      gl.disable(gl.BLEND);
    },
  };
}

//===========================================================================

const pct = (v) => `${Math.round(v * 100)}%`;

mountDemo({
  name: 'Scopes',
  pluginId: 'SC01',
  tagline:
    'Waveform, vectorscope, histogram and picture assist in one effect, either replacing the picture on a monitor layer or sitting over it in a corner. It measures what is arriving at that point in the layer’s chain — a different question from what the clip file contains, and the plugin is careful to say so.',
  repo: 'https://github.com/stoatworks-labs/resolume-scopes',
  page: 'https://stoatworks-labs.com/software/resolume-scopes/',
  video: 'https://www.youtube.com/watch?v=5fIK9vonhas',

  needFloat: true,
  needFloatBlend: true,
  showBackdrop: true,

  params: [
    {
      id: 'scope', name: 'Scope', type: 'option', default: 0, group: 'Scope',
      elements: ['Waveform', 'Vectorscope', 'Histogram', 'Picture Assist'],
    },
    { id: 'waveform', name: 'Waveform', type: 'option', default: 0, group: 'Scope', elements: ['Luma', 'RGB Parade', 'Y Cb Cr Parade'] },
    { id: 'histogram', name: 'Histogram', type: 'option', default: 0, group: 'Scope', elements: ['RGB', 'Luma', 'RGB + Luma'] },
    { id: 'assist', name: 'Assist', type: 'option', default: 0, group: 'Scope', elements: ['False Colour', 'Zebra', 'Focus Peaking'] },
    {
      id: 'intensity', name: 'Intensity', type: 'standard', default: 0.5, group: 'Scope',
      display: (v) => `${(10 ** (-1 + 2 * v)).toFixed(2)}× density`,
      hint: 'A density control rather than a brightness: the trace is additive, so one sample barely marks the tile and a thousand on the same spot saturate it.',
    },
    {
      id: 'zoom', name: 'Zoom', type: 'standard', default: 0.0, group: 'Scope',
      display: (v) => `${vectorZoom(v).toFixed(1)}×`,
      hint: 'Vectorscope only. 1× at the bottom, 8× at the top, exponentially.',
    },
    {
      id: 'quality', name: 'Quality', type: 'option', default: 1, group: 'Scope',
      elements: ['Fast', 'Good', 'Full'],
      hint: 'The sampling stride. It changes the measurement’s resolution, not how bright the trace looks — Intensity is normalised against the sample count.',
    },

    { id: 'layout', name: 'Layout', type: 'option', default: 1, group: 'Layout', elements: ['Scope Only', 'Overlay'] },
    { id: 'size', name: 'Size', type: 'standard', default: 0.45, group: 'Layout', display: (v) => pct(0.15 + 0.7 * v) },
    { id: 'x', name: 'X', type: 'standard', default: 0.02, group: 'Layout', display: pct },
    { id: 'y', name: 'Y', type: 'standard', default: 0.02, group: 'Layout', display: pct },
    { id: 'opacity', name: 'Opacity', type: 'standard', default: 0.85, group: 'Layout', display: pct },

    { id: 'graticule', name: 'Graticule', type: 'standard', default: 0.5, group: 'Graticule', display: pct },
    {
      id: 'bars', name: 'Bars', type: 'option', default: 0, group: 'Graticule', elements: ['75%', '100%'],
      hint: 'Which bar amplitude the vectorscope’s target boxes are drawn for.',
    },
    {
      id: 'background', name: 'Background', type: 'standard', default: 0.8, group: 'Graticule', display: pct,
      hint: 'At 0 the tile is transparent and the trace still reads over whatever is underneath, because the trace writes alpha in step with its colour.',
    },

    {
      id: 'matrix', name: 'Matrix', type: 'option', default: 1, group: 'Signal',
      elements: ['BT.601 SD', 'BT.709 HD', 'BT.2020 UHD'],
      hint: 'Not recoverable from the pixels. Guessed wrong it is plausibly wrong: 709 read as 601 throws the vectorscope targets 5.7° off.',
    },
    {
      id: 'range', name: 'Range', type: 'option', default: 0, group: 'Signal',
      elements: ['Full', 'Limited 16-235'],
      hint: 'Also not recoverable. Unexpanded studio levels put white at 92 IRE rather than 100.',
    },
    {
      id: 'unpremultiply', name: 'Unpremultiply', type: 'boolean', default: 1, group: 'Signal',
      hint: 'Resolume composites premultiplied, so a clip at 50% opacity arrives with its colour already halved and reads a stop down without this.',
    },

    {
      id: 'assistLevel', name: 'Assist Level', type: 'standard', default: 0.5, group: 'Assist',
      display: (v) => `${zebraIre(v).toFixed(0)} IRE`,
      hint: 'Zebra threshold in IRE, and — on the same control — the focus-peaking gradient threshold.',
    },
  ],

  sources: ['bars', 'bars100', 'ramp', 'scene', 'detail', 'spot', 'grid', 'alpha'],

  presets: {
    'Luma waveform (defaults)': {},
    'RGB parade over bars': { waveform: 1, size: 0.6 },
    'Y Cb Cr parade': { waveform: 2, size: 0.6 },
    'Vectorscope, bars': { scope: 1, size: 0.55, intensity: 0.55 },
    'Vectorscope zoomed 4×': { scope: 1, zoom: 0.667, size: 0.55 },
    'Histogram RGB + luma': { scope: 2, histogram: 2, size: 0.6 },
    'False colour, full frame': { scope: 3, assist: 0 },
    'Zebras at 95 IRE': { scope: 3, assist: 1, assistLevel: 0.75 },
    'Focus peaking': { scope: 3, assist: 2, assistLevel: 0.35 },
    'Scope only, full screen': { layout: 0, background: 0.9 },
    'Wrong matrix, on purpose': { scope: 1, matrix: 0, size: 0.6, intensity: 0.6 },
  },

  differences: [
    'The claim worth testing here: put Colour bars up, choose the Vectorscope, and change Matrix. The target boxes and the trace move **together**, because the graticule is built from the same Colorimetry module the shader plots with — no colorimetry constant is in the GLSL. Then try the "Wrong matrix, on purpose" preset to see what a mismatch actually looks like: plausible, not obviously broken.',
    'Desktop GL ignores gl_PointSize in a core profile unless GL_PROGRAM_POINT_SIZE is enabled, and the plugin has to enable it — a vectorscope whose points are one pixel renders an entire flat colour as a single dot and reads as no trace at all. WebGL2 has no such switch and honours gl_PointSize always, so that whole class of failure is absent here rather than handled.',
    'The histogram accumulates into a 32-bit float target by additive blending, which needs EXT_float_blend in a browser and is core on the desktop. If your browser lacks it the page says so rather than dropping to 16-bit and letting the tallest bins silently stop counting.',
    'The plugin’s numbers come from tools/verify.sh on one M4 Max: waveform within 0.08 IRE of each bar’s luma, vectorscope within 1.2 px of each derived target, histogram in the right bin and no other, across 75% and 100% bars in all three matrices. That harness is in the repository. This page measures nothing — it draws the same trace, but a reading here is not evidence about a reading there.',
    'What the plugin itself has never confirmed, and this page cannot either: whether Resolume’s input really is premultiplied. That is the one assumption its offline harness structurally cannot test, and the reason Unpremultiply is a parameter.',
  ],

  createRenderer,
});
