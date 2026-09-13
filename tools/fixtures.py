"""Generate synthetic STL fixtures, never touching source collections.

python tools/fixtures.py test-output/gallery
python tools/fixtures.py test-output/large --large-mb 1000  # requires numpy
"""
import argparse
import math
from pathlib import Path
import struct

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
parser.add_argument("--large-mb", type=int, default=0)
args = parser.parse_args()
args.directory.mkdir(parents=True, exist_ok=True)


def write(path, triangles):
    with path.open("wb") as f:
        f.write(b"STL Inspector synthetic fixture".ljust(80, b"\0"))
        f.write(struct.pack("<I", len(triangles)))
        for triangle in triangles:
            f.write(struct.pack("<12fH", 0, 0, 0, *sum((list(v) for v in triangle), []), 0))


def surface(u_count, v_count, point):
    triangles = []
    for u in range(u_count):
        for v in range(v_count):
            a, b, c, d = (point(i / u_count, j / v_count)
                          for i, j in ((u, v), (u + 1, v), (u + 1, v + 1), (u, v + 1)))
            triangles.extend(((a, b, c), (a, c, d)))
    return triangles


def box(x, y, z):
    vertices = [(a*x, b*y, c*z) for c in (0, 1) for b in (0, 1) for a in (0, 1)]
    faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
    return [tuple(vertices[i] for i in face) for a, b, c, d in faces for face in ((a, b, c), (a, c, d))]


if args.large_mb:
    import numpy as np
    count = max(4, args.large_mb * 1000000 // 50)
    count -= count % 2
    segments = 4096
    rings = math.ceil(count / (segments * 2))
    dtype = np.dtype([("normal", "<f4", (3,)), ("vertices", "<f4", (3, 3)), ("attribute", "<u2")])
    path = args.directory / "large_sphere.stl"
    with path.open("wb") as f:
        f.write(b"STL Inspector large streamed sphere".ljust(80, b"\0"))
        f.write(struct.pack("<I", count))
        for start in range(0, count, 262144):
            indices = np.arange(start, min(start + 262144, count), dtype=np.int64)
            cells, side = indices // 2, indices % 2
            u, v = cells % segments, cells // segments
            records = np.zeros(len(indices), dtype=dtype)
            for vertex in range(3):
                du = np.where(side == 0, (0, 1, 1)[vertex], (0, 1, 0)[vertex])
                dv = np.where(side == 0, (0, 0, 1)[vertex], (0, 1, 1)[vertex])
                longitude, latitude = (u + du) * (2*math.pi/segments), (v + dv) * (math.pi/rings)
                records["vertices"][:, vertex, 0] = np.cos(longitude)*np.sin(latitude)*50
                records["vertices"][:, vertex, 1] = np.sin(longitude)*np.sin(latitude)*50
                records["vertices"][:, vertex, 2] = np.cos(latitude)*50
            f.write(records.tobytes())
    print(f"Generated {path}: {path.stat().st_size:,} bytes, {count:,} triangles")
else:
    write(args.directory / "Calibration cube.stl", box(20, 20, 20))
    write(args.directory / "Thin build plate.stl", box(120, 80, 2))
    write(args.directory / "Tall tower.stl", box(12, 12, 110))
    sphere = surface(96, 48, lambda u,v: (30*math.cos(2*math.pi*u)*math.sin(math.pi*v),
                                        30*math.sin(2*math.pi*u)*math.sin(math.pi*v), 30*math.cos(math.pi*v)))
    write(args.directory / "Smooth sphere.stl", sphere)
    torus = surface(128, 48, lambda u,v: ((30+9*math.cos(2*math.pi*v))*math.cos(2*math.pi*u),
                                        (30+9*math.cos(2*math.pi*v))*math.sin(2*math.pi*u), 9*math.sin(2*math.pi*v)))
    write(args.directory / "Ring fitting.stl", torus)
    write(args.directory / "Two components.stl", box(20, 20, 20) + [tuple((x+35,y,z) for x,y,z in t) for t in box(15,15,35)])
    nested = args.directory / "Mechanical parts"
    nested.mkdir(exist_ok=True)
    write(nested / "Spacer.stl", torus)
    (args.directory / "Broken export.stl").write_bytes(b"solid broken\nfacet normal 0 0 1\n")
    print(f"Generated gallery in {args.directory}")
