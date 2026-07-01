import test from "node:test";
import assert from "node:assert/strict";

import {
  compactMeshByDeletedFaces,
  facesTouchingVertices,
  meshToAsciiPly,
  selectableVertices,
} from "../static/mesh-edit.mjs";

const meshData = {
  positions: new Float32Array([
    0, 0, 0,
    1, 0, 0,
    0, 1, 0,
    1, 1, 0,
  ]),
  indices: new Uint32Array([0, 1, 2, 1, 3, 2]),
  vertexCount: 4,
  faceCount: 2,
};

test("find faces touching selected mesh vertices", () => {
  assert.deepEqual(Array.from(facesTouchingVertices(meshData.indices, new Set([0]))), [0]);
  assert.deepEqual(Array.from(facesTouchingVertices(meshData.indices, new Set([3]))), [1]);
});

test("compact mesh removes deleted faces", () => {
  const compact = compactMeshByDeletedFaces(meshData, new Set([0]));

  assert.equal(compact.faceCount, 1);
  assert.equal(compact.vertexCount, 3);
  assert.deepEqual(Array.from(compact.positions), [1, 0, 0, 1, 1, 0, 0, 1, 0]);
  assert.deepEqual(Array.from(compact.indices), [0, 1, 2]);
});

test("compact textured mesh preserves uv coordinates", () => {
  const texturedMesh = {
    ...meshData,
    uvs: new Float32Array([
      0, 0,
      1, 0,
      0, 1,
      1, 1,
    ]),
  };

  const compact = compactMeshByDeletedFaces(texturedMesh, new Set([0]));

  assert.equal(compact.faceCount, 1);
  assert.deepEqual(Array.from(compact.indices), [0, 1, 2]);
  assert.deepEqual(Array.from(compact.uvs), [1, 0, 1, 1, 0, 1]);
});

test("selectable vertices ignores deleted faces", () => {
  assert.deepEqual(Array.from(selectableVertices(meshData, new Set([0]))).sort(), [1, 2, 3]);
});

test("serialize trimmed mesh to ascii ply", () => {
  const ply = meshToAsciiPly(meshData, new Set([1]));

  assert.match(ply, /element vertex 3/);
  assert.match(ply, /element face 1/);
  assert.match(ply, /3 0 1 2/);
  assert.doesNotMatch(ply, /3 1 3 2/);
});

test("serialize trimmed mesh removes unreferenced vertices and remaps sparse indices", () => {
  const sparseMesh = {
    positions: new Float32Array([
      0, 0, 0,
      10, 0, 0,
      0, 10, 0,
      9, 9, 9,
    ]),
    indices: new Uint32Array([1, 3, 2]),
  };

  const ply = meshToAsciiPly(sparseMesh, new Set());

  assert.match(ply, /element vertex 3/);
  assert.match(ply, /10 0 0/);
  assert.match(ply, /9 9 9/);
  assert.doesNotMatch(ply, /^0 0 0$/m);
  assert.match(ply, /3 0 1 2/);
  assert.doesNotMatch(ply, /3 1 3 2/);
});

test("serialize textured mesh to ascii ply with vertex colors", () => {
  const mesh = {
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
    colors: new Uint8Array([255, 0, 0, 0, 255, 0, 0, 0, 255]),
    indices: new Uint32Array([0, 1, 2]),
  };

  const ply = meshToAsciiPly(mesh, new Set());

  assert.match(ply, /property uchar red/);
  assert.match(ply, /property uchar green/);
  assert.match(ply, /property uchar blue/);
  assert.match(ply, /0 0 0 255 0 0/);
});
