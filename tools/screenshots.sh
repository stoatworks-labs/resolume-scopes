#!/usr/bin/env bash
#
# Regenerate the README screenshots from the real plugin.
#
#   tools/screenshots.sh
#
# Every image is one frame of Resolume's own bundled demo media pushed through
# `sctest --pipe`, which is the shipped shader chain driven through the real FFGL
# entry sequence. Nothing here is mocked, composited by hand, or rendered by
# anything the plugin does not use.
#
# The source clips are Resolume's, so the pictures in the README are ones any
# Resolume user already has and nothing personal is on screen.
set -euo pipefail

cd "$(dirname "$0")/.."

SCTEST=build/sctest
MEDIA="/Applications/Resolume Arena/media/Shop74"
W=1600
H=900

if [[ ! -x "$SCTEST" ]]; then
	echo "build/sctest not found. Run:"
	echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
	exit 1
fi
if [[ ! -d "$MEDIA" ]]; then
	echo "no Resolume demo media at $MEDIA"
	exit 1
fi

mkdir -p docs

# shot <name> <clip> <seek> <param=value>...
shot() {
	local name="$1"; shift
	local clip="$1"; shift
	local seek="$1"; shift

	local sets=()
	for kv in "$@"; do sets+=(--set "$kv"); done

	# One frame in, one frame out. rgba both ways so nothing is quantised to a
	# chroma-subsampled format on the way through a tool that measures chroma.
	ffmpeg -v error -y -ss "$seek" -i "$MEDIA/$clip" -frames:v 1 \
		-vf "scale=${W}:${H}:force_original_aspect_ratio=increase,crop=${W}:${H}" \
		-f rawvideo -pix_fmt rgba - 2>/dev/null \
	| "$SCTEST" --pipe --width "$W" --height "$H" "${sets[@]}" 2>/dev/null \
	| ffmpeg -v error -y -f rawvideo -pix_fmt rgba -s "${W}x${H}" -i - \
		-frames:v 1 "docs/$name.png"

	echo "  docs/$name.png"
}

echo "rendering screenshots through the real plugin..."

# Hero: the overlay, which is how most people will actually run it -- picture
# with a parade in the corner.
shot hero "IntoTheGlow_21.mov" 3 \
	"Scope=0" "Waveform=1" "Layout=1" "Size=0.42" "X=0.98" "Y=0.03" \
	"Opacity=0.9" "Background=0.75" "Graticule=0.45" "Quality=2"

# Waveform, full frame. Enter5 is high-contrast ruled geometry, so the trace has
# real structure rather than a single blob.
shot waveform "Enter5_12.mov" 4 \
	"Scope=0" "Waveform=2" "Layout=0" "Opacity=1" "Graticule=0.5" \
	"Background=1" "Intensity=0.62" "Quality=2"

# Vectorscope. Trinity is the most saturated thing in the folder, which is what
# puts a trace out near the target boxes instead of hugging the centre.
shot vectorscope "Trinity_09.mov" 5 \
	"Scope=1" "Layout=0" "Opacity=1" "Graticule=0.6" "Background=1" \
	"Intensity=0.55" "Quality=2"

shot histogram "IntoTheGlow_21.mov" 3 \
	"Scope=2" "Histogram=2" "Layout=0" "Opacity=1" "Graticule=0.4" \
	"Background=1" "Intensity=0.5" "Quality=2"

# False colour, which is the assist mode that reads at a glance in a still. The
# clip is picked for brightness, not for looks: false colour on a dark clip is
# an honest sheet of "crushed" blue and shows nothing about the scale.
shot assist "IntoTheGlow_02.mov" 3 \
	"Scope=3" "Assist=0" "Quality=2"

echo "done"
