import argparse
import os

import numpy as np
from plyfile import PlyData, PlyElement


def read_vertices(path):
    ply = PlyData.read(path)
    if "vertex" not in ply:
        raise SystemExit(f"No vertex element found in {path}")
    vertices = ply["vertex"].data
    for field in ("x", "y", "z"):
        if field not in vertices.dtype.names:
            raise SystemExit(f"Missing {field} field in {path}")
    return ply, vertices


def print_bounds(vertices):
    xyz = np.column_stack([vertices["x"], vertices["y"], vertices["z"]])
    mins = xyz.min(axis=0)
    maxs = xyz.max(axis=0)
    print(f"count: {len(vertices)}")
    print(f"x: {mins[0]:.6f} to {maxs[0]:.6f}")
    print(f"y: {mins[1]:.6f} to {maxs[1]:.6f}")
    print(f"z: {mins[2]:.6f} to {maxs[2]:.6f}")


def main():
    parser = argparse.ArgumentParser(description="Crop a 3DGS PLY by XYZ bounds while preserving Gaussian fields.")
    parser.add_argument("--input", required=True)
    parser.add_argument("--output")
    parser.add_argument("--min-x", type=float)
    parser.add_argument("--max-x", type=float)
    parser.add_argument("--min-y", type=float)
    parser.add_argument("--max-y", type=float)
    parser.add_argument("--min-z", type=float)
    parser.add_argument("--max-z", type=float)
    parser.add_argument("--inspect", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    ply, vertices = read_vertices(args.input)

    if args.inspect:
        print_bounds(vertices)
        return

    required = [args.min_x, args.max_x, args.min_y, args.max_y, args.min_z, args.max_z]
    if any(value is None for value in required):
        raise SystemExit("Crop bounds are required unless --inspect is used.")
    if args.output is None:
        raise SystemExit("--output is required when cropping.")

    mask = (
        (vertices["x"] >= args.min_x)
        & (vertices["x"] <= args.max_x)
        & (vertices["y"] >= args.min_y)
        & (vertices["y"] <= args.max_y)
        & (vertices["z"] >= args.min_z)
        & (vertices["z"] <= args.max_z)
    )

    cropped = vertices[mask]
    print(f"input count: {len(vertices)}")
    print(f"kept count:  {len(cropped)}")
    print(f"removed:     {len(vertices) - len(cropped)}")

    if len(cropped) == 0:
        raise SystemExit("Crop would remove all Gaussians. Check the bounds.")
    if args.dry_run:
        return

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    elements = []
    for element in ply.elements:
        if element.name == "vertex":
            elements.append(PlyElement.describe(cropped, "vertex"))
        else:
            elements.append(element)
    PlyData(
        elements,
        text=ply.text,
        byte_order=ply.byte_order,
        comments=ply.comments,
        obj_info=ply.obj_info,
    ).write(args.output)
    print(f"wrote: {args.output}")


if __name__ == "__main__":
    main()
