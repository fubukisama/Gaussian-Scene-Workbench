import test from "node:test";
import assert from "node:assert/strict";

import { parseMeshChunk } from "../static/mesh-chunk-loader.mjs";

test("parse binary mesh chunk payload", () => {
  const header = new TextEncoder().encode(JSON.stringify({
    format: "3dgs-editor-mesh-chunk-v1",
    vertex_count: 3,
    face_count: 1,
    has_vertex_colors: true,
  }));
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
  const colors = new Uint8Array([255, 0, 0, 0, 255, 0, 0, 0, 255]);
  const indices = new Uint32Array([0, 1, 2]);
  const payload = new Uint8Array(4 + header.byteLength + positions.byteLength + colors.byteLength + indices.byteLength);
  new DataView(payload.buffer).setUint32(0, header.byteLength, true);
  payload.set(header, 4);
  payload.set(new Uint8Array(positions.buffer), 4 + header.byteLength);
  payload.set(colors, 4 + header.byteLength + positions.byteLength);
  payload.set(new Uint8Array(indices.buffer), 4 + header.byteLength + positions.byteLength + colors.byteLength);

  const parsed = parseMeshChunk(payload.buffer);

  assert.equal(parsed.vertexCount, 3);
  assert.equal(parsed.faceCount, 1);
  assert.equal(parsed.hasVertexColors, true);
  assert.deepEqual(Array.from(parsed.indices), [0, 1, 2]);
  assert.deepEqual(Array.from(parsed.colors), Array.from(colors));
});

test("parse binary textured mesh chunk payload", () => {
  const header = new TextEncoder().encode(JSON.stringify({
    format: "3dgs-editor-textured-mesh-chunk-v1",
    vertex_count: 3,
    face_count: 1,
    has_uvs: true,
  }));
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
  const uvs = new Float32Array([0, 0, 1, 0, 0, 1]);
  const indices = new Uint32Array([0, 1, 2]);
  const payload = new Uint8Array(4 + header.byteLength + positions.byteLength + uvs.byteLength + indices.byteLength);
  new DataView(payload.buffer).setUint32(0, header.byteLength, true);
  payload.set(header, 4);
  payload.set(new Uint8Array(positions.buffer), 4 + header.byteLength);
  payload.set(new Uint8Array(uvs.buffer), 4 + header.byteLength + positions.byteLength);
  payload.set(new Uint8Array(indices.buffer), 4 + header.byteLength + positions.byteLength + uvs.byteLength);

  const parsed = parseMeshChunk(payload.buffer);

  assert.equal(parsed.vertexCount, 3);
  assert.equal(parsed.faceCount, 1);
  assert.equal(parsed.hasTextureMap, true);
  assert.deepEqual(Array.from(parsed.indices), [0, 1, 2]);
  assert.deepEqual(Array.from(parsed.uvs), Array.from(uvs));
});

test("parse padded binary mesh chunk payload", () => {
  const header = new TextEncoder().encode(JSON.stringify({
    format: "3dgs-editor-mesh-chunk-v1",
    vertex_count: 3,
    face_count: 1,
    has_vertex_colors: false,
  }));
  const padding = (4 - ((4 + header.byteLength) % 4)) % 4;
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
  const indices = new Uint32Array([0, 1, 2]);
  const payload = new Uint8Array(4 + header.byteLength + padding + positions.byteLength + indices.byteLength);
  const dataOffset = 4 + header.byteLength + padding;
  new DataView(payload.buffer).setUint32(0, header.byteLength, true);
  payload.set(header, 4);
  payload.set(new Uint8Array(positions.buffer), dataOffset);
  payload.set(new Uint8Array(indices.buffer), dataOffset + positions.byteLength);

  const parsed = parseMeshChunk(payload.buffer);

  assert.equal(parsed.vertexCount, 3);
  assert.equal(parsed.faceCount, 1);
  assert.deepEqual(Array.from(parsed.positions), Array.from(positions));
  assert.deepEqual(Array.from(parsed.indices), [0, 1, 2]);
});
