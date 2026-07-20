import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

const appSource = fs.readFileSync(path.join(process.cwd(), "static", "app.js"), "utf8");
const indexSource = fs.readFileSync(path.join(process.cwd(), "static", "index.html"), "utf8");
const trainingEnvironmentSource = fs.readFileSync(
  path.join(process.cwd(), "static", "training-environment.mjs"),
  "utf8",
);

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

test("UI scale controls are available during app startup", () => {
  assert.notEqual(topLevelDeclarationIndex(appSource, "const UI_SCALE_CHOICES"), -1);
  assert.match(appSource, /cropEditorUiScale/);
  assert.match(appSource, /function computeAutoUiScale/);
});

test("legacy HTML workbench branding is present in the document shell", () => {
  assert.match(indexSource, /<title>Gaussian Scene Workbench \(Legacy HTML\)<\/title>/);
  assert.match(indexSource, /<strong class="brand">Gaussian Scene Workbench<\/strong>/);
});

test("Gaussian PLY export is available in the output ribbon", () => {
  assert.match(indexSource, /id="downloadPly"[^>]+data-i18n="button\.downloadPly"/);
  assert.match(appSource, /function downloadGaussianPly\(\)/);
  assert.match(appSource, /\/api\/splat\/ply\?scene=/);
  assert.match(appSource, /"button\.downloadPly": "导出 PLY"/);
  assert.match(appSource, /"button\.downloadPly": "PLY 保存"/);
});

test("training does not start when the runtime preflight fails", () => {
  assert.match(appSource, /const envReady = await checkTrainingEnvironment\(\{/);
  assert.match(appSource, /requireVideo: uploadFirst && files\.some\(isVideoFile\)/);
  assert.match(appSource, /if \(!envReady\)/);
  assert.match(trainingEnvironmentSource, /data\.runtime_ready/);
  assert.match(appSource, /smart_app_control_state/);
});

test("Metashape COLMAP camera projects have a lossless import entry point", () => {
  assert.match(indexSource, /id="metashapeFolder"[^>]+webkitdirectory/);
  assert.match(indexSource, /data-i18n="button\.metashapeCameras"/);
  assert.match(appSource, /function importMetashapeCameraProject\(\)/);
  assert.match(appSource, /\/api\/metashape\/import/);
  assert.match(appSource, /"button\.metashapeCameras": "Metashape Cameras"/);
  assert.match(appSource, /"button\.metashapeCameras": "Metashape 相机"/);
  assert.match(appSource, /"button\.metashapeCameras": "Metashape カメラ"/);
});
