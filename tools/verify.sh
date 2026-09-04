#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh
#
# Three things get checked, and they fail in different ways:
#
#   --verify      that each scope reports the right number, against 75% bars
#                 whose expected readings come out of Colorimetry.cpp. Swept
#                 over every matrix and both bar amplitudes, because the whole
#                 point of deriving the graticule is that it tracks the matrix.
#   --probe       the GLSL's false-colour banding against the C++'s, which is
#                 the one place the same logic is written twice.
#   sweep.py      that no control is silently dead.
#
# What none of this can check is whether the *interpretation* is right -- that
# the composition really is BT.709 full range. Nothing can; it is not in the
# pixels. See the top of source/Colorimetry.h.
set -uo pipefail

cd "$(dirname "$0")/.."

if [[ ! -x build/sctest ]]; then
	echo "build/sctest not found. Run:"
	echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
	exit 1
fi

verify_pass=0; verify_fail=0
probe_pass=0; probe_fail=0
failures=()

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# Shaders the plugin assembles at run time.
# Mirrors assemble( sampling, body ) and its call sites in Shaders.cpp.
ASSEMBLED = {
	"waveformVertex":       [ "kPrelude", "kSampling", "kWaveformVertexBody" ],
	"vectorscopeVertex":    [ "kPrelude", "kSampling", "kVectorscopeVertexBody" ],
	"histogramAccumVertex": [ "kPrelude", "kSampling", "kHistogramAccumVertexBody" ],
	"traceFragment":        [ "kPrelude", "kTraceFragmentBody" ],
	"pictureFragment":      [ "kPrelude", "kPictureFragmentBody" ],
}

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	# An int indexes the raw strings that are not assigned to a name, in source
	# order. A literal starts with #version. Anything else names a constant
	# above -- and a name that has moved is a KeyError here, not a silent skip.
	if isinstance( p, int ):       return unnamed[ p ]
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

echo "== shaders: every one through a real GLSL compiler"
if ! shaders_compile; then
	failures+=("shaders")
fi
echo

echo "== verify: every scope against 75% and 100% bars, in all three matrices"
for matrix in 0 1 2; do
	for amplitude in 0.75 1.0; do
		if ./build/sctest --verify --amplitude "$amplitude" \
			--set "Matrix=$matrix" >/dev/null 2>&1; then
			verify_pass=$((verify_pass + 1))
		else
			verify_fail=$((verify_fail + 1))
			failures+=("verify matrix=$matrix amplitude=$amplitude")
		fi
	done
done
echo "   $verify_pass passed, $verify_fail failed"

echo "== probe: the GLSL false-colour bands against falseColourBandFor()"
for matrix in 0 1 2; do
	for range in 0 1; do
		if ./build/sctest --probe \
			--set "Matrix=$matrix" --set "Range=$range" >/dev/null 2>&1; then
			probe_pass=$((probe_pass + 1))
		else
			probe_fail=$((probe_fail + 1))
			failures+=("probe matrix=$matrix range=$range")
		fi
	done
done
echo "   $probe_pass passed, $probe_fail failed"

echo "== sweep: no control silently dead"
if python3 tools/sweep.py > /tmp/resolume-scopes-sweep.txt 2>&1; then
	echo "   all parameters affect the output"
else
	echo "   *** dead controls, see /tmp/resolume-scopes-sweep.txt"
	tail -4 /tmp/resolume-scopes-sweep.txt
	failures+=("sweep")
fi

echo
if (( ${#failures[@]} == 0 )); then
	echo "all checks passed"
	exit 0
fi

echo "FAILURES:"
printf '  %s\n' "${failures[@]}"
exit 1
