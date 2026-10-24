#!/usr/bin/env python3
"""Measure, through a camera, how much of each pixel near a corner of the LCD's glass shows.

The board runs user/lcd/ with scene=probe, booted by the command after --, the build's runner;
this lights pixels through the runner's console, see user/lcd/root.c, and watches them through the camera:
a V4L2 device, or the URL of an MJPEG stream, such as relay.py serves.
Corner 0 is the panel's top left, 1 its top right, 2 its bottom right and 3 its bottom left,
and a corner's pixels are counted inward from it along both edges.

Every frame carries three marks, pixels well inside the glass lit for good, which map the panel to the frame,
so that the camera and the board may shake; out of focus a pixel is a ring, so a mark is found as a ring's centre.
A pixel's share is its light, lit less dark, over a mark's light in the same frames:
1 for a pixel the glass shows whole, 0 for one it hides, whatever the camera's exposure did.
A take whose marks were lost, or moved further than SHAKE, is taken again, and given up as uncertain after TRIES.

It takes a band of --band pixels either side of where a quarter circle of --radius cuts each row,
each pixel twice, and a third time when the two differ by more than AGREE,
widening the band by WIDEN at most until its edges are hidden and whole,
and the corner's own pixel each row as a control; it writes the map, a row a line, to --out.
With --check map it checks the corner against another's map, mirrored, taking only each row's boundary pixels,
and measures a row off the map by more than TOLERANCE as the band would.

Usage: probe.py [--corner c] [--camera /dev/videoN|url] [--exposure n] [--settle seconds] [--out file]
                [--radius r] [--band k] [--check map] -- runner...
"""

import argparse
import math
import signal
import subprocess
import sys
import threading
import time

LCD_W = 172
LCD_H = 320


def panel(corner, x, y):
    """The panel's pixel at x, y counted from the corner."""
    px = LCD_W - 1 - x if corner in (1, 2) else x
    py = LCD_H - 1 - y if corner in (2, 3) else y
    return px, py


class Board:
    """The runner, with the probe's answers collected as they come."""

    def __init__(self, command):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.lines = []
        self.ready = threading.Event()
        self.cond = threading.Condition()
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.process.stdout:
            line = line.rstrip("\n")
            if not line.startswith("lcd: probe "):
                print(line, flush=True)
            if line == "lcd: probing":
                self.ready.set()
            with self.cond:
                self.lines.append(line)
                self.cond.notify_all()
        # the runner ended: whoever waits on it is woken, to find it gone
        self.ready.set()
        with self.cond:
            self.cond.notify_all()

    def send(self, line, answer, timeout=5.0):
        """Send a line and wait for the probe's answer to it."""
        with self.cond:
            start = len(self.lines)
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()
        deadline = time.monotonic() + timeout
        with self.cond:
            while answer not in self.lines[start:]:
                if self.process.poll() is not None or not self.cond.wait(deadline - time.monotonic()):
                    sys.exit(f"probe: no {answer!r} from the board after {line!r}")

    def light(self, px, py):
        self.send(f"{px} {py}", f"lcd: probe {px} {py}")

    def end(self):
        try:
            self.process.stdin.write("end\n")
            self.process.stdin.flush()
            self.process.wait(timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            self.process.kill()


STALL = 10      # how long, in seconds, the camera may send nothing, as a phone does while its stream is taken up again, before a run ends


class Camera:
    """The camera as a stream of grey frames, the latest kept with its count and when it came."""

    def __init__(self, device, exposure):
        if device.startswith(("http://", "https://")):
            self.url(device)
        else:
            self.v4l2(device, exposure)
        self.process = subprocess.Popen(
            ["ffmpeg", "-loglevel", "error"] + self.input + ["-f", "rawvideo", "-pix_fmt", "gray", "-"],
            stdout=subprocess.PIPE)
        self.count = 0
        self.frame = None
        self.when = 0.0
        self.cond = threading.Condition()
        threading.Thread(target=self.read, daemon=True).start()

    def url(self, url):
        tls = ["-tls_verify", "0"] if url.startswith("https://") else []
        probe = subprocess.run(["ffprobe", "-v", "error"] + tls + ["-show_entries", "stream=width,height",
                                "-of", "csv=p=0", url], capture_output=True, text=True, timeout=30)
        try:
            self.W, self.H = (int(v) for v in probe.stdout.split()[0].split(","))
        except (IndexError, ValueError):
            sys.exit(f"probe: no stream at {url}: {probe.stderr.strip()}")
        self.input = tls + ["-fflags", "nobuffer", "-i", url]

    def v4l2(self, device, exposure):
        self.W, self.H = 1280, 720
        self.input = ["-f", "v4l2", "-input_format", "mjpeg", "-video_size", f"{self.W}x{self.H}", "-framerate", "30",
                      "-i", device]
        # A camera that takes no fixed exposure, as a phone's may not, still serves, if its exposure holds still;
        # the controls in the same run show whether it did.
        set_up = subprocess.run(["v4l2-ctl", "-d", device, "-c", f"auto_exposure=1,exposure_time_absolute={exposure}",
                                 "-c", "backlight_compensation=0,white_balance_automatic=0,white_balance_temperature=6500"],
                                capture_output=True, text=True)
        if set_up.returncode != 0:
            print(f"probe: {device} takes no fixed exposure: {set_up.stderr.strip()}", flush=True)

    def read(self):
        import numpy
        size = self.W * self.H
        while True:
            data = self.process.stdout.read(size)
            if len(data) < size:
                return
            frame = numpy.frombuffer(data, dtype=numpy.uint8).reshape(self.H, self.W).astype(numpy.float32)
            with self.cond:
                self.frame = frame
                self.count += 1
                self.when = time.monotonic()
                self.cond.notify_all()

    def frames(self, settle, n):
        """The mean of n frames that came settle seconds or more from now, which lets a change reach the glass and the camera."""
        import numpy
        after = time.monotonic() + settle
        with self.cond:
            while self.when < after:
                if not self.cond.wait(STALL if self.count else 15):
                    sys.exit("probe: the camera stopped")
            total, taken, last = numpy.zeros((self.H, self.W), numpy.float32), 0, self.count - 1
            while taken < n:
                while self.count == last:
                    if not self.cond.wait(STALL):
                        sys.exit("probe: the camera stopped")
                last = self.count
                total += self.frame
                taken += 1
        return total / n

    def grab(self, settle, n):
        """n frames that came settle seconds or more from now, each as it came."""
        after = time.monotonic() + settle
        frames = []
        with self.cond:
            while self.when < after:
                if not self.cond.wait(STALL if self.count else 15):
                    sys.exit("probe: the camera stopped")
            last = self.count - 1
            while len(frames) < n:
                while self.count == last:
                    if not self.cond.wait(STALL):
                        sys.exit("probe: the camera stopped")
                last = self.count
                frames.append(self.frame)
        return frames

    def close(self):
        self.process.kill()



FRAMES = 4      # frames taken lit and frames taken dark, for each pixel measured
MARKS = [(45, 45), (45, 5), (5, 45)]  # pixels from the corner lit for good, well inside the glass, which every frame shows
FIND = 8        # how far, in the camera's pixels, a mark is looked for from where it was in the frame before
RING = 1.6      # a pixel's ring's radius out of focus, over the spread of its light, which calibrate measures
BLUR = 3.0      # the spread of a pixel's light, in the camera's pixels, until calibrate measures it
LOST = 0.4      # the share of a mark's first peak under which a frame is taken to have lost it
SHAKE = 1.5     # how far, in the camera's pixels, a mark may move within one pixel's frames before they are taken again
TRIES = 3       # takes of a pixel before it is given up as uncertain


def difference(board, camera, px, py):
    """What lighting the panel's pixel px, py changes in the camera's frame: lit less dark."""
    board.light(px, py)
    lit = camera.frames(camera.settle, FRAMES)
    board.send("off", "lcd: probe off")
    dark = camera.frames(camera.settle, FRAMES)
    return lit - dark


def window_sum(frame, cx, cy, r):
    """The light in the square of radius r around cx, cy, less the median of the ring of width r around it."""
    import numpy
    x0, y0 = int(round(cx)), int(round(cy))
    outer = frame[y0 - 2 * r:y0 + 2 * r + 1, x0 - 2 * r:x0 + 2 * r + 1]
    inner = frame[y0 - r:y0 + r + 1, x0 - r:x0 + r + 1]
    if outer.shape != (4 * r + 1, 4 * r + 1):
        return 0.0
    ring = numpy.ones(outer.shape, bool)
    ring[r:3 * r + 1, r:3 * r + 1] = False
    return float(inner.sum() - numpy.median(outer[ring]) * inner.size)


def find(frame, cx, cy):
    """
    A mark near cx, cy in a frame: the centre of its light, and its light averaged over its disc above the window's median;
    0 if none.
    Out of focus a pixel is a ring, brightest at its rim, so its brightest camera pixel lies anywhere on the rim;
    the window is averaged over discs of the ring's size, whose average is highest at the ring's centre,
    and the centre is then that of the light within the ring, taken again from where it lands.
    """
    import numpy
    ring = max(2, int(round(RING * BLUR)))
    x0, y0 = int(round(cx)), int(round(cy))
    h = FIND + 2 * ring
    win = frame[y0 - h:y0 + h + 1, x0 - h:x0 + h + 1]
    if win.shape != (2 * h + 1, 2 * h + 1):
        return cx, cy, 0.0
    win = win - numpy.median(win)
    offsets = [(dy, dx) for dy in range(-ring, ring + 1) for dx in range(-ring, ring + 1) if dy * dy + dx * dx <= ring * ring]
    inner = numpy.zeros((2 * FIND + 1, 2 * FIND + 1))
    for dy, dx in offsets:
        inner += win[2 * ring + dy:2 * ring + dy + 2 * FIND + 1, 2 * ring + dx:2 * ring + dx + 2 * FIND + 1]
    inner /= len(offsets)
    py, px = numpy.unravel_index(numpy.argmax(inner), inner.shape)
    level = float(inner.max())
    if level <= 0:
        return cx, cy, 0.0
    ys, xs = numpy.mgrid[0:2 * h + 1, 0:2 * h + 1]
    light = numpy.clip(win, 0, None)
    my, mx = float(py + 2 * ring), float(px + 2 * ring)
    for _ in range(3):
        near = numpy.where((xs - mx) ** 2 + (ys - my) ** 2 <= (ring + 1) ** 2, light, 0)
        total = near.sum()
        if total <= 0:
            return cx, cy, 0.0
        mx, my = float((xs * near).sum() / total), float((ys * near).sum() / total)
    return x0 - h + mx, y0 - h + my, level


def spread(d, cx, cy):
    """How far the light around cx, cy spreads: the standard deviation of where it lies, in the camera's pixels."""
    import numpy
    x0, y0 = int(round(cx)), int(round(cy))
    win = numpy.clip(d[y0 - FIND:y0 + FIND + 1, x0 - FIND:x0 + FIND + 1], 0, None)
    win = numpy.where(win > win.max() / 10, win, 0)
    ys, xs = numpy.mgrid[-FIND:FIND + 1, -FIND:FIND + 1]
    total = win.sum()
    mx, my = (xs * win).sum() / total, (ys * win).sum() / total
    return float(numpy.sqrt((((xs - mx) ** 2 + (ys - my) ** 2) * win).sum() / total / 2))


def calibrate(board, camera, corner):
    """
    Where each mark lands in the camera's frame, found by lighting it alone, before the marks are lit for good,
    its peak then, and how far a pixel's light spreads.
    """
    import numpy
    global BLUR
    diffs, spreads = [], []
    for x, y in MARKS:
        d = difference(board, camera, *panel(corner, x, y))
        cy, cx = numpy.unravel_index(numpy.argmax(d), d.shape)
        noise = 1.4826 * numpy.median(numpy.abs(d - numpy.median(d)))
        if d.max() < max(8 * noise, 10):
            sys.exit(f"probe: the camera saw nothing of the mark {x}, {y} from the corner over its noise, {noise:.1f}; "
                     "is the board in view?")
        diffs.append((d, cx, cy, noise))
        spreads.append(spread(d, cx, cy))
    BLUR = float(numpy.median(spreads))
    seen, peaks = [], []
    for (x, y), (d, cx, cy, noise), s in zip(MARKS, diffs, spreads):
        fx, fy, peak = find(d, cx, cy)
        seen.append((fx, fy))
        peaks.append(peak)
        print(f"probe: the mark {x}, {y} from the corner lands at {fx:.1f}, {fy:.1f}, peak {peak:.0f}, "
              f"noise {noise:.1f}, spread {s:.2f}", flush=True)
    for x, y in MARKS:
        px, py = panel(corner, x, y)
        board.send(f"mark {px} {py}", f"lcd: probe mark {px} {py}")
    pitch = numpy.hypot(seen[0][0] - seen[1][0], seen[0][1] - seen[1][1]) / numpy.hypot(
        MARKS[0][0] - MARKS[1][0], MARKS[0][1] - MARKS[1][1])
    print(f"probe: a pixel of the panel is {pitch:.2f} of the camera's", flush=True)
    global FLOORS
    FLOORS = [LOST * p for p in peaks]
    return seen, pitch, BLUR


def take(board, camera, corner, marks, r, x, y):
    """
    One take of the corner's pixel x, y: its light over a mark's, from frames lit and dark,
    each frame mapped by where its own marks are, and the marks where the last frame had them;
    None for the share if a mark was lost or moved too far within the take.
    """
    import numpy
    board.light(*panel(corner, x, y))
    lit = camera.grab(camera.settle, FRAMES)
    board.send("off", "lcd: probe off")
    dark = camera.grab(camera.settle, FRAMES)
    a = numpy.array([[mx, my, 1.0] for mx, my in MARKS])
    sums, refs, tracks = [], [], []
    for frame in lit + dark:
        found = [find(frame, mx, my) for mx, my in marks]
        if any(f[2] < floor for f, floor in zip(found, FLOORS)):
            print(f"  take of {x}, {y} dropped: a mark was lost", flush=True)
            return None, marks
        marks = [(f[0], f[1]) for f in found]
        tracks.append(marks)
        affine = numpy.linalg.solve(a, numpy.array(marks))
        cx, cy = numpy.array([x, y, 1.0]) @ affine
        sums.append(window_sum(frame, cx, cy, r))
        refs.append(numpy.mean([window_sum(frame, mx, my, r) for mx, my in marks]))
    t = numpy.array(tracks)
    if (numpy.abs(t - t.mean(axis=0)).max()) > SHAKE:
        print(f"  take of {x}, {y} dropped: the marks moved {numpy.abs(t - t.mean(axis=0)).max():.1f}", flush=True)
        return None, marks
    ref = float(numpy.mean(refs))
    if ref <= 0:
        return None, marks
    return (numpy.mean(sums[:FRAMES]) - numpy.mean(sums[FRAMES:])) / ref, marks


def share(board, camera, corner, marks, r, x, y):
    """The corner's pixel x, y's share of a whole pixel's light, taken again while the view shook; None if it kept shaking."""
    for _ in range(TRIES):
        s, marks = take(board, camera, corner, marks, r, x, y)
        if s is not None:
            return s, marks
    return None, marks


AGREE = 0.3     # how far two takes of a pixel may differ before a third is taken
REACH = 38      # the furthest column from the corner a band goes, short of the marks
WIDEN = 3       # how many columns a band may grow by on either side before its edge is left uncertain
HIDDEN, WHOLE = 0.25, 0.6   # a share at most HIDDEN is a hidden pixel, at least WHOLE a whole one


def steady(board, camera, corner, marks, r, x, y):
    """A pixel's share from two takes, or the median of three when the two differ; None if no take held."""
    import numpy
    takes = []
    for _ in range(3):
        v, marks = share(board, camera, corner, marks, r, x, y)
        if v is not None:
            takes.append(v)
        if len(takes) == 2 and abs(takes[0] - takes[1]) <= AGREE:
            break
    v = float(numpy.median(takes)) if takes else None
    print(f"  pixel {x}, {y}: " + " ".join(f"{t:.2f}" for t in takes) + (f" -> {v:.2f}" if takes else " -> ?"), flush=True)
    return v, marks


def band_row(board, camera, corner, marks, r, y, radius, band):
    """A row's pixels in question, by column: a band around the circle's cut, widened until its edges are settled."""
    import math
    c = radius - 0.5 - y
    cut = radius - math.sqrt(radius * radius - c * c) if abs(c) < radius else 0.0
    lo, hi = max(0, int(math.floor(cut)) - band), min(REACH, int(math.ceil(cut)) + band)
    row = {}
    for x in range(lo, hi + 1):
        row[x], marks = steady(board, camera, corner, marks, r, x, y)
    hidden = lambda x: row.get(x) is not None and row[x] <= HIDDEN
    whole = lambda x: row.get(x) is not None and row[x] >= WHOLE
    first, last = lo, hi
    while lo > max(0, first - WIDEN) and not (hidden(lo) and hidden(lo + 1)):
        lo -= 1
        row[lo], marks = steady(board, camera, corner, marks, r, lo, y)
    while hi < min(REACH, last + WIDEN) and not (whole(hi) and whole(hi - 1)):
        hi += 1
        row[hi], marks = steady(board, camera, corner, marks, r, hi, y)
    return row, lo, hi, marks


def measure_band(board, camera, args, marks, r):
    width = min(REACH + 1, int(args.radius) + args.band + 4)
    with open(args.out, "w") as out:
        out.write(f"# corner {args.corner}, a row a line from the corner, each pixel's share of a whole one;\n"
                  f"# measured in a band around a cut of radius {args.radius}: - assumed hidden, = assumed whole, ? uncertain\n")
        for y in range(int(math.ceil(args.radius)) + 3):
            hidden_control, marks = share(board, camera, args.corner, marks, r, 0, 0)
            row, lo, hi, marks = band_row(board, camera, args.corner, marks, r, y, args.radius, args.band)
            cells, shown = [], []
            for x in range(max(width, hi + 1)):
                if x < lo:
                    cells.append("-")
                    shown.append("-")
                elif x > hi:
                    cells.append("=")
                    shown.append("=")
                else:
                    v = row[x]
                    cells.append("?" if v is None else f"{v:.2f}")
                    shown.append("?" if v is None else "#" if v >= WHOLE else "+" if v > HIDDEN else ".")
            h = "?" if hidden_control is None else f"{hidden_control:.2f}"
            print(f"probe: row {y:2d} {''.join(shown)}  the corner's own pixel {h}", flush=True)
            out.write(" ".join(cells) + "\n")
            out.flush()


TOLERANCE = 0.2  # how far a pixel's share may lie from what the map says before its row is measured again


def read_map(path):
    """A map measure wrote: each row's shares by column, each over the row's whole pixels, None where uncertain."""
    rows = []
    for line in open(path):
        if line.startswith("#"):
            continue
        cells = line.split()
        taken = [float(c) for c in cells if c not in "-=?"]
        whole = sorted(v for v in taken if v >= WHOLE)
        gain = whole[len(whole) // 2] if whole else 1.0
        rows.append([0.0 if c == "-" else 1.0 if c == "=" else None if c == "?" else min(1.0, max(0.0, float(c) / gain))
                     for c in cells])
    return rows


def check(board, camera, args, marks, r):
    """
    Whether the corner's glass is as a map of another corner says, mirrored:
    in each row, only the pixels the map has partly hidden, the last hidden one before them and the first whole one after,
    each taken once, and again only when it lies off the map; a row off the map is measured as measure_band would.
    """
    expected = read_map(args.check)
    control, marks = share(board, camera, args.corner, marks, r, 0, 0)
    print(f"probe: the corner's own pixel {'?' if control is None else f'{control:.2f}'}", flush=True)
    off = 0
    with open(args.out, "w") as out:
        out.write(f"# corner {args.corner}, checked against {args.check}: each row's pixels taken, as column:share\n")
        for y, row in enumerate(expected):
            known = [x for x, v in enumerate(row) if v is not None]
            hidden = [x for x in known if row[x] <= HIDDEN / 2]
            whole = [x for x in known if row[x] >= 1 - HIDDEN / 2]
            if not whole:
                continue
            if not hidden and whole[0] == 0 and all(row[x] >= 1 - HIDDEN / 2 for x in known):
                break
            lo = hidden[-1] if hidden else 0
            hi = min(x for x in whole if x > lo) if any(x > lo for x in whole) else whole[-1]
            got = {}
            for x in range(lo, hi + 1):
                v, marks = share(board, camera, args.corner, marks, r, x, y)
                got[x] = v
            gain = got[hi] if got[hi] is not None and got[hi] >= WHOLE else 1.0
            far = [x for x in got if got[x] is None or abs(got[x] / gain - row[x]) > TOLERANCE]
            for x in far:
                v, marks = steady(board, camera, args.corner, marks, r, x, y)
                got[x] = v
            far = [x for x in got if got[x] is None or abs(got[x] / gain - row[x]) > TOLERANCE]
            cells = " ".join(f"{x}:{'?' if got[x] is None else f'{got[x] / gain:.2f}'}/{row[x]:.2f}" for x in sorted(got))
            if far:
                off += 1
                print(f"probe: row {y:2d} off the map at {far}: {cells}", flush=True)
                band, blo, bhi, marks = band_row(board, camera, args.corner, marks, r, y, args.radius or 22.0, args.band)
                cells = " ".join(f"{x}:{'?' if band[x] is None else f'{band[x]:.2f}'}" for x in sorted(band))
                print(f"probe: row {y:2d} measured: {cells}", flush=True)
            else:
                print(f"probe: row {y:2d} as the map: {cells}", flush=True)
            out.write(f"{y} {cells}\n")
            out.flush()
    print(f"probe: {off} rows off the map", flush=True)


def measure(board, args):
    camera = Camera(args.camera, args.exposure)
    camera.settle = args.settle if args.settle is not None else 0.4 if args.camera.startswith("http") else 0.15
    try:
        marks, pitch, blur = calibrate(board, camera, args.corner)
        r = max(2, int(math.ceil(3 * blur)))
        print(f"probe: light is summed within {r} of the camera's pixels of where a pixel lands", flush=True)
        if args.check is not None:
            check(board, camera, args, marks, r)
            print(f"probe: what was taken is in {args.out}", flush=True)
            return
        measure_band(board, camera, args, marks, r)
        print(f"probe: the map is in {args.out}", flush=True)
    finally:
        camera.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--corner", type=int, choices=range(4), default=0)
    parser.add_argument("--camera", default="/dev/video0")
    parser.add_argument("--exposure", type=int, default=150)
    parser.add_argument("--radius", type=float, default=22.0, help="the radius of the cut the band lies around")
    parser.add_argument("--band", type=int, default=4)
    parser.add_argument("--settle", type=float, default=None,
                        help="seconds from a change on the glass to the first frame of it; 0.15, or 0.4 for a URL")
    parser.add_argument("--out", default="lcd-corner.txt")
    parser.add_argument("--check", default=None, help="check the corner against a map measure wrote of another")
    parser.add_argument("runner", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    runner = args.runner[1:] if args.runner[:1] == ["--"] else args.runner
    if not runner:
        parser.error("no runner after --")

    # a probe told to end ends the board's runner too, which otherwise keeps the board's port
    signal.signal(signal.SIGTERM, lambda *_: sys.exit("probe: ended"))
    board = Board(runner)
    if not board.ready.wait(120):
        sys.exit("probe: the board never said it was probing")
    if "lcd: probing" not in board.lines:
        sys.exit(f"probe: the runner ended, with {board.process.wait()}, before the board was probing")
    try:
        measure(board, args)
    finally:
        board.end()


if __name__ == "__main__":
    main()
