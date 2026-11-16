#!/usr/bin/env python3
"""A car model for the smoke lab, from the game's own car as it was drawn.

Run the game (Metal) with ray traced reflections on and XBE_TRACE_DUMP=<frame>: it writes that frame's traced scene,
every car draw's triangles in the world, to trace_scene_<frame>.txt beside the exe (sdk/runtime/gpu/metal.mm). This
cuts one car out of it, turns it into the car's own frame (x forward, y up, z to its left, metres, wheels on y = 0) and
writes local/car.json and local/car.js, which index.html loads when they are there ("Load car model" otherwise).

local/ is git-ignored: the model is the game's, made from your own disc, never committed or shared.

  tools/smoke-lab/car_from_scene.py .local/replay/run/trace_scene_4000.txt           # the car with the most triangles
  tools/smoke-lab/car_from_scene.py SCENE --list                                     # the cars in it
  tools/smoke-lab/car_from_scene.py SCENE --car 2 --flip                             # another, turned round
"""
import argparse
import json
import math
import os
import sys


def read_scene(path):
    draws, cur = [], None
    with open(path) as f:
        for line in f:
            if line.startswith('v '):
                cur['verts'].append([float(x) for x in line.split()[1:]])
            elif line.startswith('d '):
                t = line.split()
                cur = {'draw': int(t[1]), 'glass': t[2] == '1', 'colour': [float(x) for x in t[3:7]],
                       'm': [float(x) for x in t[7:19]], 'verts': []}
                draws.append(cur)
    return [d for d in draws if d['verts']]


def bounds(vs):
    lo = [min(v[k] for v in vs) for k in range(3)]
    hi = [max(v[k] for v in vs) for k in range(3)]
    return lo, hi


def group_cars(draws):
    # as BuildScene groups them: the largest draw first, each other joining the first whose bounds (30 cm wider) hold its centre
    for d in draws:
        d['lo'], d['hi'] = bounds(d['verts'])
        d['mid'] = [(d['lo'][k] + d['hi'][k]) / 2 for k in range(3)]
    order = sorted(draws, key=lambda d: -len(d['verts']))
    cars = []
    for d in order:
        for c in cars:
            h = c[0]
            if all(h['lo'][k] - 0.3 <= d['mid'][k] <= h['hi'][k] + 0.3 for k in range(3)):
                c.append(d)
                break
        else:
            cars.append([d])
    return sorted(cars, key=lambda c: -sum(len(d['verts']) for d in c))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('scene')
    ap.add_argument('--car', type=int, default=0, help='which car, by size (0 the largest)')
    ap.add_argument('--flip', action='store_true', help='turn the car round (if its front came out at the back)')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'local', 'car.json'))
    a = ap.parse_args()
    cars = group_cars(read_scene(a.scene))
    if a.list or not cars:
        for i, c in enumerate(cars):
            lo, hi = bounds([v for d in c for v in d['verts']])
            print(f'car {i}: {len(c)} draws, {sum(len(d["verts"]) for d in c) // 3} triangles, '
                  f'size {hi[0]-lo[0]:.2f} x {hi[1]-lo[1]:.2f} x {hi[2]-lo[2]:.2f} m, at {(lo[0]+hi[0])/2:.1f} {(lo[2]+hi[2])/2:.1f}')
        return 0 if cars else 1
    car = cars[a.car]
    body = car[0]
    # the body's frame: its transform's columns (scaled), normalised; the car's local axes in the world
    m = body['m']
    cols = [[m[0 + k], m[4 + k], m[8 + k]] for k in range(3)]
    t = [m[3], m[7], m[11]]
    axes = []
    for c in cols:
        n = math.sqrt(sum(x * x for x in c))
        axes.append([x / n for x in c])
    # which local axis points up, and of the other two the longer across the car's triangles is its length
    up = max(range(3), key=lambda k: abs(axes[k][1]))
    pts = [v for d in car for v in d['verts']]
    def local(p):
        r = [p[0] - t[0], p[1] - t[1], p[2] - t[2]]
        return [sum(r[j] * axes[k][j] for j in range(3)) for k in range(3)]
    loc = [local(p) for p in pts]
    others = [k for k in range(3) if k != up]
    ext = {k: max(q[k] for q in loc) - min(q[k] for q in loc) for k in others}
    fwd = max(others, key=lambda k: ext[k])
    side = [k for k in others if k != fwd][0]
    # the lab's frame: x forward, y up, z to the car's left (up x forward), right handed
    f = [0, 0, 0]
    f[fwd] = -1 if a.flip else 1
    u = [0, 0, 0]
    u[up] = 1 if axes[up][1] > 0 else -1
    zl = [u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]]
    def to_lab(q):  # a place or a direction in the body's frame
        return [sum(q[k] * f[k] for k in range(3)), sum(q[k] * u[k] for k in range(3)), sum(q[k] * zl[k] for k in range(3))]
    def dir_local(n):
        return [sum(n[j] * axes[k][j] for j in range(3)) for k in range(3)]
    groups, all_lab = [], []
    for d in car:
        pos, nor = [], []
        for v in d['verts']:
            p = to_lab(local(v[:3]))
            n = dir_local(v[3:6])
            ln = math.sqrt(sum(x * x for x in n)) or 1
            pos += p
            nor += to_lab([x / ln for x in n])
            all_lab.append(p)
        groups.append({'glass': d['glass'], 'colour': d['colour'][:3], 'positions': pos, 'normals': nor})
    # the front: a car's cabin (its glass) sits behind its middle, so if the glass is ahead, the model is the wrong way
    # round; turned (and --flip turns it again)
    # (the windows only: glass whose lowest point is in the upper half, not the lamps' lenses)
    lo0, hi0 = bounds(all_lab)
    windows = [g for g in groups if g['glass'] and min(g['positions'][1::3]) > (lo0[1] + hi0[1]) / 2]
    glass = [g['positions'][i] for g in windows for i in range(0, len(g['positions']), 3)]
    if glass and sum(glass) / len(glass) > (lo0[0] + hi0[0]) / 2:
        for g in groups:
            for key in ('positions', 'normals'):
                q = g[key]
                for i in range(0, len(q), 3):
                    q[i], q[i + 2] = -q[i], -q[i + 2]  # half a turn about y
        all_lab = [[-p[0], p[1], -p[2]] for p in all_lab]
        print('the glass was ahead of the middle: turned round')
    lo, hi = bounds(all_lab)
    # centred along and across, wheels on y = 0, as the lab's place for a car (its +0x44 is about 0.3 m up)
    shift = [-(lo[0] + hi[0]) / 2, -lo[1], -(lo[2] + hi[2]) / 2]
    for g in groups:
        p = g['positions']
        for i in range(0, len(p), 3):
            p[i] = round(p[i] + shift[0], 4); p[i + 1] = round(p[i + 1] + shift[1], 4); p[i + 2] = round(p[i + 2] + shift[2], 4)
        g['normals'] = [round(x, 3) for x in g['normals']]
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    model = {'source': os.path.basename(a.scene), 'car': a.car, 'flip': a.flip,
             'size': [hi[k] - lo[k] for k in range(3)], 'groups': groups}
    with open(a.out, 'w') as f:
        json.dump(model, f, separators=(',', ':'))
    # the same as a script beside it (car.js), which the lab loads opened straight from disk too (file:// cannot fetch)
    with open(os.path.splitext(a.out)[0] + '.js', 'w') as f:
        f.write('window.SMOKE_LAB_CAR = ')
        json.dump(model, f, separators=(',', ':'))
        f.write(';\n')
    print(f'{a.out}: car {a.car}, {len(groups)} draws, {len(all_lab) // 3} triangles, '
          f'{hi[0]-lo[0]:.2f} long, {hi[1]-lo[1]:.2f} tall, {hi[2]-lo[2]:.2f} wide')
    return 0


if __name__ == '__main__':
    sys.exit(main())
