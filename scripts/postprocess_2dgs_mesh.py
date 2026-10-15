#!/usr/bin/env python3
"""Safely filter disconnected components from a 2DGS mesh."""

import argparse
import os
from pathlib import Path

import numpy as np
import open3d as o3d


def cluster_triangle_threshold(cluster_n_triangles, cluster_to_keep, min_triangles=50):
    triangle_counts = np.asarray(cluster_n_triangles).reshape(-1)
    if triangle_counts.size == 0:
        return None, 0

    requested_clusters = max(1, int(cluster_to_keep))
    clusters_to_keep = min(requested_clusters, triangle_counts.size)
    ranked_counts = np.sort(triangle_counts)
    threshold = int(ranked_counts[-clusters_to_keep])
    threshold = min(max(threshold, int(min_triangles)), int(ranked_counts[-1]))
    return threshold, clusters_to_keep


def postprocess_mesh(input_path, output_path, cluster_to_keep, min_triangles=50):
    input_path = Path(input_path).resolve()
    output_path = Path(output_path).resolve()
    if not input_path.is_file():
        raise FileNotFoundError(f"Raw 2DGS mesh was not found: {input_path}")

    mesh = o3d.io.read_triangle_mesh(str(input_path))
    source_vertices = len(mesh.vertices)
    source_triangles = len(mesh.triangles)
    if source_vertices == 0 or source_triangles == 0:
        raise RuntimeError(
            f"Raw 2DGS mesh is empty: {source_vertices} vertices, {source_triangles} triangles"
        )

    triangle_clusters, cluster_n_triangles, _ = mesh.cluster_connected_triangles()
    triangle_clusters = np.asarray(triangle_clusters)
    threshold, clusters_to_keep = cluster_triangle_threshold(
        cluster_n_triangles,
        cluster_to_keep,
        min_triangles,
    )
    if threshold is None:
        raise RuntimeError("Raw 2DGS mesh has no connected triangle clusters")

    cluster_n_triangles = np.asarray(cluster_n_triangles)
    triangles_to_remove = cluster_n_triangles[triangle_clusters] < threshold
    mesh.remove_triangles_by_mask(triangles_to_remove)
    mesh.remove_unreferenced_vertices()
    mesh.remove_degenerate_triangles()
    mesh.remove_duplicated_triangles()
    mesh.remove_duplicated_vertices()
    if len(mesh.vertices) == 0 or len(mesh.triangles) == 0:
        raise RuntimeError("2DGS mesh post-processing removed all geometry")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(f"{output_path.stem}.tmp{output_path.suffix}")
    try:
        if not o3d.io.write_triangle_mesh(str(temporary_path), mesh):
            raise RuntimeError(f"Open3D could not write the processed mesh: {temporary_path}")
        os.replace(temporary_path, output_path)
    finally:
        temporary_path.unlink(missing_ok=True)

    print(
        "2DGS mesh post-process complete: "
        f"{source_vertices} -> {len(mesh.vertices)} vertices, "
        f"{source_triangles} -> {len(mesh.triangles)} triangles, "
        f"{len(cluster_n_triangles)} connected clusters, "
        f"keeping up to {clusters_to_keep} with threshold {threshold}."
    )
    print(output_path)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--num-cluster", type=int, default=50)
    parser.add_argument("--min-triangles", type=int, default=50)
    return parser.parse_args()


def main():
    args = parse_args()
    postprocess_mesh(args.input, args.output, args.num_cluster, args.min_triangles)


if __name__ == "__main__":
    main()
