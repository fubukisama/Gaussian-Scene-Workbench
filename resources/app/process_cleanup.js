const http = require("http");
const { spawn } = require("child_process");

function requestServerShutdown(port, timeoutMs = 1200) {
  if (!port) return Promise.resolve(false);
  return new Promise((resolve) => {
    const body = "{}";
    const req = http.request(
      {
        hostname: "127.0.0.1",
        port,
        path: "/api/shutdown",
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "Content-Length": Buffer.byteLength(body)
        },
        timeout: timeoutMs
      },
      (res) => {
        res.resume();
        resolve(res.statusCode >= 200 && res.statusCode < 500);
      }
    );
    req.on("error", () => resolve(false));
    req.on("timeout", () => {
      req.destroy();
      resolve(false);
    });
    req.end(body);
  });
}

function killProcessTree(pid, options = {}) {
  if (!pid) return Promise.resolve(false);
  const platform = options.platform || process.platform;
  const spawnFn = options.spawnFn || spawn;
  const log = options.log || (() => {});

  if (platform === "win32") {
    return new Promise((resolve) => {
      const child = spawnFn("taskkill.exe", ["/PID", String(pid), "/T", "/F"], {
        windowsHide: true,
        stdio: "ignore"
      });
      child.on("error", (error) => {
        log(`taskkill failed for ${pid}: ${error && error.message ? error.message : error}`);
        resolve(false);
      });
      child.on("exit", (code) => resolve(code === 0));
    });
  }

  try {
    process.kill(pid, "SIGTERM");
    return Promise.resolve(true);
  } catch (error) {
    log(`process.kill failed for ${pid}: ${error && error.message ? error.message : error}`);
    return Promise.resolve(false);
  }
}

async function stopServerProcess(serverProcess, port, options = {}) {
  if (!serverProcess) return false;
  const log = options.log || (() => {});
  const shutdownFn = options.requestServerShutdown || requestServerShutdown;
  const killTreeFn = options.killProcessTree || ((pid) => killProcessTree(pid, options));
  const pid = serverProcess.pid;

  try {
    await shutdownFn(port);
  } catch (error) {
    log(`server shutdown request failed: ${error && error.message ? error.message : error}`);
  }

  if (pid) {
    const killedTree = await killTreeFn(pid);
    if (killedTree) return true;
  }

  try {
    if (typeof serverProcess.kill === "function") {
      return serverProcess.kill();
    }
  } catch (error) {
    log(`server process kill failed: ${error && error.message ? error.message : error}`);
  }
  return false;
}

module.exports = {
  requestServerShutdown,
  killProcessTree,
  stopServerProcess
};
