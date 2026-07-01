import test from "node:test";
import assert from "node:assert/strict";

import { parseObjTexturedMesh } from "../static/textured-mesh-loader.mjs";

test("parse textured obj triangles with uv coordinates", () => {
  const obj = `
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f 1/1 2/2 3/3 4/4
`;

  const parsed = parseObjTexturedMesh(obj);

  assert.equal(parsed.vertexCount, 4);
  assert.equal(parsed.faceCount, 2);
  assert.deepEqual(Array.from(parsed.indices), [0, 1, 2, 0, 2, 3]);
  assert.deepEqual(Array.from(parsed.uvs), [0, 0, 1, 0, 1, 1, 0, 1]);
});
