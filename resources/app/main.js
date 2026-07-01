const { app, BrowserWindow, dialog, ipcMain, Menu, shell } = require("electron");
const { spawn } = require("child_process");
const fs = require("fs");
const http = require("http");
const path = require("path");
const { createExitConfirmation } = require("./exit_confirmation");
const { stopServerProcess } = require("./process_cleanup");
const { checkPortAvailable, resolveServerPort, serverPortForMode } = require("./port_policy");
const { showEnvironmentWizard } = require("./environment_wizard");

let mainWindow = null;
let serverProcess = null;
let serverPort = null;
let serverProcessPid = null;
let cleanupStarted = false;
let ROOT = null;
let CROP_EDITOR_DIR = null;
let SERVER_PATH = null;
let LOG_PATH = null;
let exitConfirmation = null;

function ignoreBrokenConsolePipe(stream) {
  if (!stream || typeof stream.on !== "function") return;
  stream.on("error", (error) => {
    if (error && error.code === "EPIPE") return;
    writeLogFile(`[${new Date().toISOString()}] console stream error: ${error && error.stack ? error.stack : error}\n`);
  });
}

function writeLogFile(line) {
  if (!LOG_PATH) return;
  try {
    fs.appendFileSync(LOG_PATH, line);
  } catch (_) {
  }
}

function writeConsole(line) {
  if (process.env.GS_EDITOR_CONSOLE_LOGS !== "1") return;
  try {
    if (process.stdout && process.stdout.writable && !process.stdout.destroyed) {
      process.stdout.write(line);
    }
  } catch (error) {
    if (!error || error.code !== "EPIPE") {
      writeLogFile(`[${new Date().toISOString()}] console write failed: ${error && error.stack ? error.stack : error}\n`);
    }
  }
}

function log(message) {
  const line = `[${new Date().toISOString()}] ${message}\n`;
  writeLogFile(line);
  writeConsole(line);
}

ignoreBrokenConsolePipe(process.stdout);
ignoreBrokenConsolePipe(process.stderr);

process.on("uncaughtException", (error) => {
  if (error && error.code === "EPIPE") {
    writeLogFile(`[${new Date().toISOString()}] ignored broken console pipe\n`);
    return;
  }
  dialog.showErrorBox("3DGS Editor failed", error && error.stack ? error.stack : String(error));
  app.quit();
});

for (const signal of ["SIGINT", "SIGTERM"]) {
  process.on(signal, () => {
    log(`Received ${signal}; cleaning up server process tree`);
    cleanupAndExit(0);
  });
}

async function cleanupAndExit(exitCode = 0) {
  if (cleanupStarted) return;
  cleanupStarted = true;
  const processToStop = serverProcess || (serverProcessPid ? {
    pid: serverProcessPid,
    kill() {
      return false;
    }
  } : null);
  serverProcess = null;
  if (processToStop) {
    log(`Stopping server process tree pid=${processToStop.pid}, port=${serverPort}`);
    await stopServerProcess(processToStop, serverPort, { log });
  }
  app.exit(exitCode);
}

function setupExitConfirmation() {
  exitConfirmation = createExitConfirmation({
    dialog,
    getWindow: () => mainWindow,
    cleanupAndExit,
    log
  });
  ipcMain.on("gs-editor:set-language", (_event, language) => {
    const applied = exitConfirmation.setLanguage(language);
    log(`UI language set to ${applied}`);
  });
}

function resolveProjectRoot() {
  const candidates = [
    process.env.GS_EDITOR_ROOT,
    path.resolve(__dirname, ".."),
    path.dirname(process.execPath),
    path.join(app.getPath("desktop"), "3dgs")
  ].filter(Boolean);

  for (const candidate of candidates) {
    const server = path.join(candidate, "crop_editor", "server.py");
    if (fs.existsSync(server)) return candidate;
  }
  throw new Error(`Could not find 3dgs project root. Set GS_EDITOR_ROOT or keep the project at ${path.join(app.getPath("desktop"), "3dgs")}`);
}

function configurePaths() {
  ROOT = resolveProjectRoot();
  CROP_EDITOR_DIR = path.join(ROOT, "crop_editor");
  SERVER_PATH = path.join(CROP_EDITOR_DIR, "server.py");
  LOG_PATH = path.join(ROOT, "desktop_app", "desktop_editor.log");
  fs.mkdirSync(path.dirname(LOG_PATH), { recursive: true });
  log(`ROOT=${ROOT}`);
  log(`SERVER_PATH=${SERVER_PATH}`);
}

function requestOk(url) {
  return new Promise((resolve) => {
    const req = http.get(url, (res) => {
      res.resume();
      resolve(res.statusCode >= 200 && res.statusCode < 500);
    });
    req.on("error", () => resolve(false));
    req.setTimeout(1000, () => {
      req.destroy();
      resolve(false);
    });
  });
}

function requestJson(url) {
  return new Promise((resolve) => {
    const req = http.get(url, (res) => {
      let body = "";
      res.setEncoding("utf8");
      res.on("data", (chunk) => {
        body += chunk;
      });
      res.on("end", () => {
        if (res.statusCode !== 200) {
          resolve(null);
          return;
        }
        try {
          resolve(JSON.parse(body));
        } catch (_) {
          resolve(null);
        }
      });
    });
    req.on("error", () => resolve(null));
    req.setTimeout(8000, () => {
      req.destroy();
      resolve(null);
    });
  });
}

function postJson(url, data = {}) {
  return new Promise((resolve) => {
    const body = JSON.stringify(data);
    const req = http.request(url, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "Content-Length": Buffer.byteLength(body)
      }
    }, (res) => {
      let responseBody = "";
      res.setEncoding("utf8");
      res.on("data", (chunk) => {
        responseBody += chunk;
      });
      res.on("end", () => {
        try {
          resolve({ ok: res.statusCode >= 200 && res.statusCode < 300, data: JSON.parse(responseBody || "{}") });
        } catch (_) {
          resolve({ ok: res.statusCode >= 200 && res.statusCode < 300, data: null });
        }
      });
    });
    req.on("error", () => resolve({ ok: false, data: null }));
    req.setTimeout(1500, () => {
      req.destroy();
      resolve({ ok: false, data: null });
    });
    req.write(body);
    req.end();
  });
}

async function isExistingEditorServer(port) {
  const payload = await requestJson(`http://127.0.0.1:${port}/api/scenes`);
  return Boolean(payload && Array.isArray(payload.scenes));
}

async function isCompatibleEditorServer(port) {
  const payload = await requestJson(`http://127.0.0.1:${port}/api/app/health`);
  return Boolean(payload && payload.app === "3DGS Editor" && payload.capabilities && payload.capabilities.asset_manager);
}

async function isReusableEditorServer(port) {
  return (await isExistingEditorServer(port)) && (await isCompatibleEditorServer(port));
}

async function waitForPortAvailable(port, timeoutMs = 10000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    if (await checkPortAvailable(port)) return true;
    await new Promise((resolve) => setTimeout(resolve, 300));
  }
  return false;
}

async function stopIncompatibleEditorServer(port) {
  if (!(await isExistingEditorServer(port)) || (await isCompatibleEditorServer(port))) return false;
  log(`Existing 3DGS Editor server at http://127.0.0.1:${port} is missing required API capabilities; requesting restart`);
  await postJson(`http://127.0.0.1:${port}/api/shutdown`);
  if (!(await waitForPortAvailable(port))) {
    throw new Error(`Existing 3DGS Editor server on port ${port} did not stop in time. Close old 3DGS Editor windows and try again.`);
  }
  return true;
}

async function waitForServer(port, timeoutMs = 30000) {
  const start = Date.now();
  const url = `http://127.0.0.1:${port}/api/scenes`;
  while (Date.now() - start < timeoutMs) {
    if (await requestOk(url)) return true;
    await new Promise((resolve) => setTimeout(resolve, 400));
  }
  return false;
}

async function startPythonServer() {
  if (!fs.existsSync(SERVER_PATH)) {
    throw new Error(`server.py not found: ${SERVER_PATH}`);
  }

  serverPort = serverPortForMode({ isPackaged: app.isPackaged, env: process.env });
  await stopIncompatibleEditorServer(serverPort);
  const startup = await resolveServerPort(serverPort, {
    isPackaged: app.isPackaged,
    isExpectedServer: isReusableEditorServer
  });
  if (!startup.shouldStartServer) {
    log(`Reusing existing 3DGS Editor server at http://127.0.0.1:${serverPort}`);
    return;
  }
  log(`Selected fixed ${app.isPackaged ? "packaged" : "development"} port ${serverPort}`);
  const condaBat = path.join(process.env.USERPROFILE || "", "miniforge3", "condabin", "conda.bat");
  const envPython = path.join(process.env.USERPROFILE || "", "miniforge3", "envs", "gaussian_splatting", "python.exe");
  const envRoot = path.dirname(envPython);
  const envPath = [
    envRoot,
    path.join(envRoot, "Library", "bin"),
    path.join(envRoot, "Library", "usr", "bin"),
    path.join(envRoot, "Scripts"),
    process.env.PATH || ""
  ].join(path.delimiter);

  if (fs.existsSync(envPython)) {
    log(`Starting server with env python: ${envPython}`);
    serverProcess = spawn(envPython, [SERVER_PATH, "--host", "127.0.0.1", "--port", String(serverPort), "--no-open"], {
      cwd: ROOT,
      windowsHide: true,
      stdio: "pipe",
      env: { ...process.env, PATH: envPath, PYTHONUTF8: "1", CONDA_PREFIX: envRoot }
    });
  } else {
    if (!fs.existsSync(condaBat)) {
      throw new Error(`Neither env python nor conda.bat was found. Tried: ${envPython} and ${condaBat}`);
    }

    log(`Starting server through conda.bat: ${condaBat}`);
    const command = [
      `call "${condaBat}" activate gaussian_splatting`,
      `python "${SERVER_PATH}" --host 127.0.0.1 --port ${serverPort} --no-open`
    ].join(" && ");

    serverProcess = spawn("cmd.exe", ["/d", "/s", "/c", command], {
      cwd: ROOT,
      windowsHide: true,
      stdio: "pipe",
      env: { ...process.env, PYTHONUTF8: "1" }
    });
  }

  serverProcessPid = serverProcess.pid;
  serverProcess.stdout.on("data", (data) => log(`[server stdout] ${data}`));
  serverProcess.stderr.on("data", (data) => log(`[server stderr] ${data}`));
  serverProcess.on("exit", (code) => {
    log(`[server] exited with code ${code}`);
    serverProcess = null;
  });

  const ready = await waitForServer(serverPort);
  if (!ready) {
    throw new Error(`Crop editor server did not start in time. See log: ${LOG_PATH}`);
  }
  log(`Server ready at http://127.0.0.1:${serverPort}`);
}

async function preflightRuntimeEnvironment() {
  const report = await requestJson(`http://127.0.0.1:${serverPort}/api/train/check?backend=3dgs`);
  if (!report) {
    throw new Error("Could not read runtime environment report from the local server.");
  }
  const missing = [];
  const warnings = [];
  if (!report.python_exists) missing.push(`Python executable: ${report.python}`);
  if (!report.env_root_exists) missing.push(`Conda environment: ${report.env_root}`);
  if (!report.gaussian_dir_exists) missing.push(`3DGS source: ${report.gaussian_dir}`);
  if (!report.colmap_exists) missing.push(`COLMAP executable: ${report.colmap}`);
  if (!report.opencv_ok) missing.push(`OpenCV Python module: ${report.opencv_error || "cv2 import failed"}`);
  const ffmpeg = path.join(process.env.USERPROFILE || "", "miniforge3", "envs", "gaussian_splatting", "Library", "bin", "ffmpeg.exe");
  if (!fs.existsSync(ffmpeg)) warnings.push(`ffmpeg executable was not found: ${ffmpeg}`);
  if (!missing.length) return true;
  log(`Runtime preflight failed: ${missing.join("; ")}`);
  await showEnvironmentWizard({
    dialog,
    shell,
    root: ROOT,
    startupError: new Error("Runtime preflight failed. Run setup to install or repair missing components."),
    extraMissing: missing,
    extraWarnings: warnings,
    log,
  });
  return false;
}

function createMenu() {
  const template = [
    {
      label: "File",
      submenu: [
        {
          label: "Open Output Folder",
          click: () => shell.openPath(path.join(ROOT, "output"))
        },
        {
          label: "Open Project Folder",
          click: () => shell.openPath(ROOT)
        },
        { type: "separator" },
        { role: "quit" }
      ]
    },
    {
      label: "View",
      submenu: [
        { role: "reload" },
        { role: "forceReload" },
        { role: "toggleDevTools" },
        { type: "separator" },
        { role: "resetZoom" },
        { role: "zoomIn" },
        { role: "zoomOut" },
        { role: "togglefullscreen" },
        {
          label: "Exit Full Screen",
          accelerator: "Esc",
          click: () => restoreWindow("menu")
        }
      ]
    }
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

function isEscapeInput(input) {
  return input.key === "Escape" || input.key === "Esc" || input.code === "Escape";
}

function restoreWindow(source) {
  if (!mainWindow) return false;
  let changed = false;
  if (mainWindow.isFullScreen()) {
    mainWindow.setFullScreen(false);
    changed = true;
  }
  if (typeof mainWindow.isSimpleFullScreen === "function" && mainWindow.isSimpleFullScreen()) {
    mainWindow.setSimpleFullScreen(false);
    changed = true;
  }
  if (mainWindow.isMaximized()) {
    mainWindow.unmaximize();
    changed = true;
  }
  if (changed) log(`Restored window from ${source}`);
  return changed;
}

async function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1500,
    height: 950,
    minWidth: 1100,
    minHeight: 700,
    title: "3DGS Editor",
    backgroundColor: "#111111",
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      preload: path.join(__dirname, "preload.js")
    }
  });

  mainWindow.on("closed", () => {
    mainWindow = null;
  });
  if (exitConfirmation) {
    exitConfirmation.attachToWindow(mainWindow);
  }

  mainWindow.webContents.on("before-input-event", (event, input) => {
    if (input.type === "keyDown" && isEscapeInput(input) && restoreWindow("Escape")) {
      event.preventDefault();
    }
  });

  await mainWindow.loadURL(`http://127.0.0.1:${serverPort}`);
}

app.whenReady().then(async () => {
  try {
    configurePaths();
    setupExitConfirmation();
    createMenu();
    await startPythonServer();
    if (!(await preflightRuntimeEnvironment())) {
      app.quit();
      return;
    }
    await createWindow();
  } catch (error) {
    log(`Startup failed: ${error && error.stack ? error.stack : error}`);
    if (ROOT) {
      const action = await showEnvironmentWizard({ dialog, shell, root: ROOT, startupError: error, log });
      if (action !== "quit") {
        app.quit();
        return;
      }
    } else {
      dialog.showErrorBox("3DGS Editor failed to start", error.stack || error.message);
    }
    app.quit();
  }
});

app.on("window-all-closed", () => {
  app.quit();
});

app.on("before-quit", (event) => {
  if (cleanupStarted) return;
  if (exitConfirmation && exitConfirmation.handleBeforeQuit(event)) {
    return;
  }
  event.preventDefault();
  cleanupAndExit(0);
});

app.on("will-quit", (event) => {
  if (cleanupStarted) return;
  if (exitConfirmation && exitConfirmation.handleBeforeQuit(event)) {
    return;
  }
  event.preventDefault();
  cleanupAndExit(0);
});
