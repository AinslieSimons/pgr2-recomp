#!/usr/bin/env python3
"""Compare the cost of the night car shadows across performance recordings (XBE_PERF=1, frames.csv).

  tools/perf/shadow_report.py LABEL=perf/<folder> [LABEL=perf/<folder> ...] [--out report.md]

Each recording is one run, ideally of the same pad recording (scripts/replay-test.sh or a replay script) with one thing changed
(PGR2_STREET_SHADOWS, PGR2_STREET_REACH, ...). Only race frames at night count: the night pass (0x32D10) ran in them
(state_night_cars > 0) or, for a run with it off, the race's own frames (state_cars > 0).

For each run: frame time (interval, and work: the frame without its waits), the GPU's time, the game thread's capture, the
night pass and per-light zones (src/game/night_shadows.cpp), the shadow-map and street-shadow passes' draws, triangles and
sampled GPU time (src/game/draw_filter.cpp), and the shadows, cars and lights per frame. Then each run's frames split by how
many cars the night pass looked at, which shows what each car on screen costs.
"""
import csv
import statistics
import sys


def load(path):
    rows = []
    with open(path.rstrip("/") + "/frames.csv") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def num(r, k, default=0.0):
    v = r.get(k)
    if v in (None, ""):
        return default
    try:
        return float(v)
    except ValueError:
        return default


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    return s[min(len(s) - 1, int(p / 100.0 * len(s)))]


def mean(values):
    return statistics.fmean(values) if values else 0.0


def race_frames(rows):
    night = [r for r in rows if num(r, "state_night_cars") > 0]
    if night:
        # the stretch from the first to the last night frame (frames between with no car in view count too)
        first = rows.index(night[0])
        last = rows.index(night[-1])
        return [r for r in rows[first:last + 1] if num(r, "state_cars") > 0]
    return [r for r in rows if num(r, "state_cars") > 0 and num(r, "state_view_mode", -1) >= 0]


def summary(label, rows):
    work = [num(r, "work_ms") for r in rows]
    interval = [num(r, "interval_ms") for r in rows]
    gpu = [num(r, "gpu_ms") for r in rows]
    tagged = [r for r in rows if num(r, "gpuf_main") > 0]
    out = {
        "run": label,
        "frames": len(rows),
        "fps": "%.1f" % (1000.0 / mean(interval) if interval else 0),
        "interval p50/p95/p99": "%.1f / %.1f / %.1f" % (pct(interval, 50), pct(interval, 95), pct(interval, 99)),
        "work mean/p95": "%.2f / %.2f" % (mean(work), pct(work, 95)),
        "gpu mean/p95": "%.2f / %.2f" % (mean(gpu), pct(gpu, 95)),
        "draw cpu": "%.2f" % mean([num(r, "draw_cpu_ms") for r in rows]),
        "game capture": "%.2f" % mean([num(r, "game_capture_ms") for r in rows]),
        "zone night_pass": "%.2f" % mean([num(r, "zone_night_pass_ms") for r in rows]),
        "zone street_light": "%.2f" % mean([num(r, "zone_street_light_ms") for r in rows]),
        "draws shadow_map": "%.0f" % mean([num(r, "draws_shadow_map") for r in rows]),
        "draws street_shadow": "%.0f" % mean([num(r, "draws_street_shadow") for r in rows]),
        "tris shadow_map": "%.0f" % mean([num(r, "tris_shadow_map") for r in rows]),
        "draws all": "%.0f" % mean([num(r, "draws") for r in rows]),
        "gpu shadow_map (v+f, sampled)": "%.2f" % mean([num(r, "gpuv_shadow_map") + num(r, "gpuf_shadow_map") for r in tagged]),
        "gpu street_shadow (v+f, sampled)": "%.2f" % mean([num(r, "gpuv_street_shadow") + num(r, "gpuf_street_shadow") for r in tagged]),
        "gpu main (v+f, sampled)": "%.2f" % mean([num(r, "gpuv_main") + num(r, "gpuf_main") for r in tagged]),
        "shadows/frame": "%.1f" % mean([num(r, "state_street_shadows") for r in rows]),
        "cars/frame": "%.2f" % mean([num(r, "state_night_cars") for r in rows]),
        "lights/frame": "%.1f" % mean([num(r, "state_night_lights") for r in rows]),
    }
    return out


def by_cars(rows):
    buckets = {}
    for r in rows:
        n = int(num(r, "state_night_cars"))
        buckets.setdefault(min(n, 6), []).append(r)
    lines = []
    for n in sorted(buckets):
        b = buckets[n]
        lines.append(
            (
                "%d%s" % (n, "+" if n == 6 else ""),
                len(b),
                mean([num(r, "work_ms") for r in b]),
                pct([num(r, "work_ms") for r in b], 95),
                mean([num(r, "gpu_ms") for r in b]),
                mean([num(r, "zone_street_light_ms") for r in b]),
                mean([num(r, "state_street_shadows") for r in b]),
                mean([num(r, "draws_shadow_map") for r in b]),
            )
        )
    return lines


def table(rows, keys):
    head = "| " + " | ".join(keys) + " |"
    sep = "|" + "|".join("---" for _ in keys) + "|"
    body = ["| " + " | ".join(str(r[k]) for k in keys) + " |" for r in rows]
    return "\n".join([head, sep] + body)


def main(argv):
    out_path = None
    runs = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--out":
            out_path = argv[i + 1]
            i += 2
            continue
        label, _, path = a.partition("=")
        if not path:
            label, path = a.rstrip("/").split("/")[-1], a
        runs.append((label, path))
        i += 1
    if not runs:
        print(__doc__)
        return 1
    text = ["# Night car shadows: cost by run", ""]
    sums, split = [], []
    for label, path in runs:
        rows = race_frames(load(path))
        sums.append(summary(label, rows))
        split.append((label, by_cars(rows)))
    keys = list(sums[0].keys())
    # one table per group of columns, so it stays readable
    groups = [keys[:6], keys[6:12], keys[12:]]
    for g in groups:
        text += [table(sums, ["run"] + [k for k in g if k != "run"]), ""]
    text += ["## By cars the night pass looked at in the frame", ""]
    for label, lines in split:
        text += ["### " + label, "", "| cars | frames | work mean | work p95 | gpu mean | street_light zone | shadows | map draws |", "|---|---|---|---|---|---|---|---|"]
        text += ["| %s | %d | %.2f | %.2f | %.2f | %.2f | %.1f | %.0f |" % l for l in lines]
        text.append("")
    report = "\n".join(text)
    print(report)
    if out_path:
        with open(out_path, "w") as f:
            f.write(report + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
