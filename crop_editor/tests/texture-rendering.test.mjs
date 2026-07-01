import test from "node:test";
import assert from "node:assert/strict";

import { configureOpenMvsTexture } from "../static/texture-rendering.mjs";

test("OpenMVS OBJ textures use the atlas orientation expected by Three.js", () => {
  const texture = { flipY: false, needsUpdate: false, anisotropy: 1, colorSpace: "" };
  const renderer = { capabilities: { getMaxAnisotropy: () => 16 } };
  const THREERef = { SRGBColorSpace: "srgb" };

  configureOpenMvsTexture(texture, renderer, THREERef);

  assert.equal(texture.flipY, true);
  assert.equal(texture.colorSpace, "srgb");
  assert.equal(texture.anisotropy, 8);
  assert.equal(texture.needsUpdate, true);
});
