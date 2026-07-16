const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const mainSource = fs.readFileSync(path.join(__dirname, "..", "main.js"), "utf8");

test("desktop window identifies the Legacy HTML build", () => {
  assert.match(
    mainSource,
    /const LEGACY_APP_TITLE = "Gaussian Scene Workbench \(Legacy HTML\)";/
  );
  assert.match(mainSource, /title: LEGACY_APP_TITLE/);
});

test("Windows uses a distinct AppUserModelID for the Legacy HTML build", () => {
  assert.match(
    mainSource,
    /const LEGACY_APP_USER_MODEL_ID = "Fubukisama\.GaussianSceneWorkbench\.LegacyHtml";/
  );
  assert.match(mainSource, /app\.setAppUserModelId\(LEGACY_APP_USER_MODEL_ID\)/);
});
