const fs = require("fs");
const path = require("path");

function exists(filePath) {
  try {
    return Boolean(filePath && fs.existsSync(filePath));
  } catch (_) {
    return false;
  }
}

function defaultSetupPath(root) {
  return path.join(root || "", "Setup 3DGS Editor.cmd");
}

function defaultReadmePath(root) {
  return path.join(root || "", "README_RELEASE.md");
}

function firstExisting(candidates) {
  for (const candidate of candidates) {
    if (exists(candidate)) return candidate;
  }
  return candidates[0];
}

function diagnoseEnvironment(root, startupError) {
  const userProfile = process.env.USERPROFILE || "";
  const miniforge = path.join(userProfile, "miniforge3");
  const condaBat = path.join(miniforge, "condabin", "conda.bat");
  const envPython = path.join(miniforge, "envs", "gaussian_splatting", "python.exe");
  const colmapExe = process.env.COLMAP_EXE || firstExisting([
    path.join(root || "", "third_party", "colmap", "bin", "colmap.exe"),
    path.join(root || "", "tools", "colmap", "bin", "colmap.exe"),
    path.join(root || "", "colmap", "bin", "colmap.exe"),
    path.join(userProfile, "Downloads", "colmap-x64-windows-cuda", "bin", "colmap.exe"),
  ]);
  const ffmpegExe = path.join(miniforge, "envs", "gaussian_splatting", "Library", "bin", "ffmpeg.exe");
  const report = {
    root,
    setupPath: defaultSetupPath(root),
    readmePath: defaultReadmePath(root),
    startupError: startupError ? String(startupError.stack || startupError.message || startupError) : "",
    missing: [],
    warnings: [],
  };

  if (!exists(path.join(root || "", "crop_editor", "server.py"))) {
    report.missing.push("Application files: crop_editor/server.py");
  }
  if (!exists(path.join(root || "", "gaussian-splatting", "train.py"))) {
    report.missing.push("3DGS source: gaussian-splatting/train.py");
  }
  if (!exists(condaBat)) {
    report.missing.push(`Miniforge/Conda: ${condaBat}`);
  }
  if (!exists(envPython)) {
    report.missing.push(`Conda environment: ${envPython}`);
  }
  if (!exists(colmapExe)) {
    report.missing.push(`COLMAP executable: ${colmapExe}`);
  }
  if (!exists(ffmpegExe)) {
    report.missing.push(`Video extraction ffmpeg: ${ffmpegExe}`);
  }
  if (!exists(path.join(root || "", "crop_editor", "node_modules", ".bin", "splat-transform.cmd"))) {
    report.warnings.push("crop_editor Node dependencies are missing; splat import/export helpers may be unavailable.");
  }
  if (!exists(path.join(root || "", "openMVS"))) {
    report.warnings.push("OpenMVS is missing; OpenMVS texture baking will be unavailable.");
  }
  if (!exists(path.join(process.env.SystemRoot || "C:\\Windows", "System32", "nvidia-smi.exe"))) {
    report.warnings.push("NVIDIA driver tools were not detected; CUDA training may be unavailable.");
  }
  return report;
}

async function showEnvironmentWizard({ dialog, shell, root, startupError, log, extraMissing = [], extraWarnings = [] }) {
  const report = diagnoseEnvironment(root, startupError);
  for (const item of extraMissing) {
    if (item && !report.missing.includes(item)) report.missing.push(item);
  }
  for (const item of extraWarnings) {
    if (item && !report.warnings.includes(item)) report.warnings.push(item);
  }
  const detail = [
    "The editor UI is installed, but the local 3DGS runtime is not ready.",
    "",
    "Missing:",
    ...(report.missing.length ? report.missing.map((item) => `- ${item}`) : ["- No required item detected as missing."]),
    "",
    "Warnings:",
    ...(report.warnings.length ? report.warnings.map((item) => `- ${item}`) : ["- None"]),
    "",
    "Startup error:",
    report.startupError || "No startup error text.",
  ].join("\n");

  const buttons = ["Run Setup", "Open Guide", "Open Folder", "Quit"];
  const choice = await dialog.showMessageBox({
    type: "warning",
    title: "3DGS Editor environment setup",
    message: "3DGS Editor needs a local runtime before it can start.",
    detail,
    buttons,
    defaultId: 0,
    cancelId: 3,
    noLink: true,
  });
  if (choice.response === 0) {
    const setup = report.setupPath;
    if (exists(setup)) {
      log && log(`Opening setup script: ${setup}`);
      await shell.openPath(setup);
      return "setup";
    }
    await dialog.showMessageBox({
      type: "error",
      message: "Setup script was not found.",
      detail: setup,
      buttons: ["OK"],
    });
  } else if (choice.response === 1) {
    const readme = exists(report.readmePath) ? report.readmePath : root;
    log && log(`Opening setup guide: ${readme}`);
    await shell.openPath(readme);
    return "guide";
  } else if (choice.response === 2) {
    await shell.openPath(root);
    return "folder";
  }
  return "quit";
}

module.exports = {
  diagnoseEnvironment,
  showEnvironmentWizard,
};
