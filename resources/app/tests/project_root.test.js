const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");

const { resolveProjectRoot, resolveWorkspaceRoot } = require("../project_root");

function fakeExists(...roots) {
  const servers = new Set(roots.map((root) => path.join(root, "crop_editor", "server.py").toLowerCase()));
  return (candidate) => servers.has(candidate.toLowerCase());
}

test("development launch prefers the repository containing resources/app", () => {
  const repositoryRoot = "C:\\work\\Gaussian-Scene-Workbench";
  const legacyRoot = "C:\\Users\\tester\\Desktop\\3dgs";

  const resolved = resolveProjectRoot({
    appDir: path.join(repositoryRoot, "resources", "app"),
    execPath: "C:\\electron\\electron.exe",
    desktopPath: "C:\\Users\\tester\\Desktop",
    isPackaged: false,
    env: {},
    existsSync: fakeExists(repositoryRoot, legacyRoot)
  });

  assert.equal(resolved, repositoryRoot);
});

test("packaged launch prefers crop_editor shipped beside the executable", () => {
  const packageRoot = "C:\\apps\\Gaussian Scene Workbench-win32-x64";
  const legacyRoot = "C:\\Users\\tester\\Desktop\\3dgs";

  const resolved = resolveProjectRoot({
    appDir: path.join(packageRoot, "resources", "app"),
    execPath: path.join(packageRoot, "Gaussian Scene Workbench.exe"),
    desktopPath: "C:\\Users\\tester\\Desktop",
    isPackaged: true,
    env: {},
    existsSync: fakeExists(packageRoot, legacyRoot)
  });

  assert.equal(resolved, packageRoot);
});

test("GS_EDITOR_ROOT remains the highest-priority override", () => {
  const explicitRoot = "D:\\datasets\\workbench";
  const repositoryRoot = "C:\\work\\Gaussian-Scene-Workbench";

  const resolved = resolveProjectRoot({
    appDir: path.join(repositoryRoot, "resources", "app"),
    execPath: "C:\\electron\\electron.exe",
    desktopPath: "C:\\Users\\tester\\Desktop",
    isPackaged: false,
    env: { GS_EDITOR_ROOT: explicitRoot },
    existsSync: fakeExists(explicitRoot, repositoryRoot)
  });

  assert.equal(resolved, explicitRoot);
});

test("workspace discovery keeps existing models in the legacy Desktop workspace", () => {
  const projectRoot = "C:\\work\\Gaussian-Scene-Workbench";
  const desktopPath = "C:\\Users\\tester\\Desktop";
  const legacyOutput = path.join(desktopPath, "3dgs", "output").toLowerCase();

  const resolved = resolveWorkspaceRoot({
    projectRoot,
    desktopPath,
    env: {},
    existsSync: (candidate) => candidate.toLowerCase() === legacyOutput
  });

  assert.equal(resolved, path.join(desktopPath, "3dgs"));
});

test("GS_EDITOR_WORKSPACE_ROOT can select another model workspace", () => {
  const resolved = resolveWorkspaceRoot({
    projectRoot: "C:\\work\\Gaussian-Scene-Workbench",
    desktopPath: "C:\\Users\\tester\\Desktop",
    env: { GS_EDITOR_WORKSPACE_ROOT: "D:\\Gaussian Models" },
    existsSync: () => false
  });

  assert.equal(resolved, "D:\\Gaussian Models");
});
