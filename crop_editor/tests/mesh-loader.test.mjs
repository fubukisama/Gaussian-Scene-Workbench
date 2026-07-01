import test from "node:test";
import assert from "node:assert/strict";

import { parsePlyMesh } from "../static/mesh-loader.mjs";

test("parse ascii triangular ply mesh", () => {
  const ply = `ply
format ascii 1.0
element vertex 3
property float x
property float y
property float z
property float nx
property float ny
property float nz
element face 1
property list uchar int vertex_indices
end_header
0 0 0 0 0 1
1 0 0 0 0 1
0 1 0 0 0 1
3 0 1 2
`;
  const parsed = parsePlyMesh(new TextEncoder().encode(ply).buffer);

  assert.equal(parsed.vertexCount, 3);
  assert.equal(parsed.faceCount, 1);
  assert.deepEqual(Array.from(parsed.indices), [0, 1, 2]);
  assert.deepEqual(Array.from(parsed.positions.slice(0, 6)), [0, 0, 0, 1, 0, 0]);
});

test("parse ascii ply mesh vertex colors", () => {
  const ply = `ply
format ascii 1.0
element vertex 3
property float x
property float y
property float z
property uchar red
property uchar green
property uchar blue
element face 1
property list uchar int vertex_indices
end_header
0 0 0 255 0 0
1 0 0 0 255 0
0 1 0 0 0 255
3 0 1 2
`;
  const parsed = parsePlyMesh(new TextEncoder().encode(ply).buffer);

  assert.equal(parsed.hasVertexColors, true);
  assert.deepEqual(Array.from(parsed.colors), [255, 0, 0, 0, 255, 0, 0, 0, 255]);
});
