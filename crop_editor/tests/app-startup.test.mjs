import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

const appSource = fs.readFileSync(path.join(process.cwd(), "static", "app.js"), "utf8");

function topLevelDeclarationIndex(source, declaration) {
  let depth = 0;
  for (let i = 0; i < source.length; i++) {
    const ch = source[i];
    if (ch === "{") depth++;
    else if (ch === "}") depth = Math.max(0, depth - 1);
    if (depth === 0 && source.startsWith(declaration, i)) return i;
  }
  return -1;
}

test("training UI presets are available during app startup", () => {
  assert.notEqual(topLevelDeclarationIndex(appSource, "const TRAINING_UI_PRESETS"), -1);
});
