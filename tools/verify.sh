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
