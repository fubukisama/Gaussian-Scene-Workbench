const fs = require("fs");
const path = require("path");

function projectRootCandidates({ appDir, execPath, desktopPath, isPackaged = false, env = process.env }) {
  const packagedRoot = path.dirname(execPath);
  const bundledRoot = path.resolve(appDir, "..", "..");
  const legacyWorkspaceRoot = isPackaged
    ? path.resolve(packagedRoot, "..", "..", "..")
    : "";

  return [
    env.GS_EDITOR_ROOT,
    bundledRoot,
    packagedRoot,
    legacyWorkspaceRoot,
    path.join(desktopPath, "3dgs")
  ].filter(Boolean);
}

function resolveProjectRoot(options) {
  const existsSync = options.existsSync || fs.existsSync;
  const candidates = projectRootCandidates(options);
  const seen = new Set();

  for (const candidate of candidates) {
    const normalized = path.resolve(candidate);
    const key = normalized.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    if (existsSync(path.join(normalized, "crop_editor", "server.py"))) {
      return normalized;
    }
  }

  throw new Error(
    `Could not find Gaussian Scene Workbench project root. Set GS_EDITOR_ROOT or keep the project at ${path.join(options.desktopPath, "3dgs")}`
  );
}

function resolveWorkspaceRoot({ projectRoot, desktopPath, env = process.env, existsSync = fs.existsSync }) {
  if (env.GS_EDITOR_WORKSPACE_ROOT) {
    return path.resolve(env.GS_EDITOR_WORKSPACE_ROOT);
  }

  const legacyRoot = path.join(desktopPath, "3dgs");
  const legacyHasData = existsSync(path.join(legacyRoot, "output"))
    || existsSync(path.join(legacyRoot, "datasets"));
  return legacyHasData ? path.resolve(legacyRoot) : path.resolve(projectRoot);
}

module.exports = {
  projectRootCandidates,
  resolveProjectRoot,
  resolveWorkspaceRoot
};
