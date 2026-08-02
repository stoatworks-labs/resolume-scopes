"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that -- `--verify` exercises four of these nineteen parameters.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

Three things about this plugin in particular that will fool you:

  * **The baseline must be a 2D gradient, not bars.** Colour bars are flat down
    every column, so all of a column's samples land on one waveform row and one
    vectorscope point, and the trace saturates whatever Intensity and Quality
    are set to. Against bars both controls read stone dead while working
    perfectly. `--gradient` spreads each column over half the range.

  * **Most parameters belong to one scope.** Zoom does nothing unless the
    vectorscope is showing, Assist Level does nothing unless Picture Assist is,
    and so on -- correctly. Each one gets the context it needs below; a
    parameter that reads dead is only interesting once you have checked that
    its context actually switches it on.

  * **Unpremultiply is dead against an opaque source, and should be.** It only
    does anything when alpha is below 1, so it is swept against a source the
    harness has premultiplied at 50%, the way Resolume hands over a layer that
    is not at full opacity.
"""
import subprocess, zlib, struct, sys, tempfile

SC = tempfile.mkdtemp(prefix="scsweep")

# A baseline where the scope is genuinely drawing something, so that nothing
# reads dead merely because the thing it modifies is switched off.
BASE = {
    "Scope": 0,          # waveform
    "Waveform": 0,       # luma
    "Histogram": 0,
    "Assist": 0,
    "Intensity": 0.5,
    "Zoom": 0,
    "Quality": 1,
    "Layout": 1,         # overlay, so Size/X/Y are live
    "Size": 0.45,
    "X": 0.02,
    "Y": 0.02,
    "Opacity": 0.85,
    "Graticule": 0.5,
    "Bars": 0,
    "Background": 0.8,
    "Matrix": 1,         # BT.709
    "Range": 0,
    "Unpremultiply": 1,
    "Assist Level": 0.5,
}

# Options are discrete; sweep them across their real element range. Everything
# else is a plain 0..1 float.
DISCRETE = {
    "Scope": (0, 3),
    "Waveform": (0, 2),
    "Histogram": (0, 2),
    "Assist": (0, 2),
    "Quality": (0, 2),
    "Layout": (0, 1),
    "Bars": (0, 1),
    "Matrix": (0, 2),
    "Range": (0, 1),
    "Unpremultiply": (0, 1),
}

# Parameters that only mean anything under a particular scope. Each entry is
# what has to be true for the control to be doing its job at all.
CONTEXT = {
    "Histogram": {"Scope": 2},
    "Assist": {"Scope": 3},
    "Assist Level": {"Scope": 3, "Assist": 1},   # zebra threshold
    "Zoom": {"Scope": 1},
    "Bars": {"Scope": 1, "Graticule": 1.0},      # Bars only moves the graticule
}

# Extra command-line arguments, for the one control that needs a different
# source picture rather than different parameters.
EXTRA = {
    "Unpremultiply": ["--alpha", "0.5"],
}


def render(path, overrides, extra):
    args = ["./build/sctest", "--gradient", "--out", path, "--width", "1280", "--height", "720"]
    args += extra
    merged = dict(BASE)
    merged.update(overrides)
    for k, v in merged.items():
        args += ["--set", f"{k}={v}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", r.stdout, r.stderr)
        sys.exit(1)
    return open(path, "rb").read()


def pixels(png):
    i = 8
    idat = b""
    w = h = 0
    while i < len(png):
        ln = struct.unpack(">I", png[i:i + 4])[0]
        t = png[i + 4:i + 8]
        d = png[i + 8:i + 8 + ln]
        if t == b"IHDR":
            w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT":
            idat += d
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h))


def diff(a, b):
    pa, pb = pixels(a), pixels(b)
    n = len(pa)
    changed = 0
    total = 0
    for i in range(0, n, 4):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 2:
            changed += 1
        total += d
    return changed / (n / 4) * 100, total / (n / 4)


names = subprocess.run(["./build/sctest", "--list"], capture_output=True, text=True).stdout
params = [" ".join(l.split()[1:-1]) for l in names.strip().splitlines()]

print(f"{'parameter':<16} {'pixels changed':>15} {'mean delta':>11}   verdict")
dead = []
for p in params:
    lo, hi = DISCRETE.get(p, (0.0, 1.0))
    context = CONTEXT.get(p, {})
    extra = EXTRA.get(p, [])
    a = render(f"{SC}/a.png", {**context, p: lo}, extra)
    b = render(f"{SC}/b.png", {**context, p: hi}, extra)
    pct, mean = diff(a, b)
    ok = pct > 0.05
    if not ok:
        dead.append(p)
    print(f"{p:<16} {pct:14.2f}% {mean:11.3f}   {'ok' if ok else '*** NO EFFECT ***'}")

print()
if dead:
    print("DEAD CONTROLS:", ", ".join(dead))
    sys.exit(1)
print(f"all {len(params)} parameters affect the output")
