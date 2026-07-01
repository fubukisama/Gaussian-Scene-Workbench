const net = require("net");

const PACKAGED_PORT = 7860;
const DEVELOPMENT_PORT = 7862;

function parsePort(value, name = "port") {
  const port = Number(value);
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw new Error(`Invalid ${name}: ${value}`);
  }
  return port;
}

function serverPortForMode({ isPackaged = false, env = process.env } = {}) {
  if (env.GS_EDITOR_PORT) return parsePort(env.GS_EDITOR_PORT, "GS_EDITOR_PORT");
  return isPackaged ? PACKAGED_PORT : DEVELOPMENT_PORT;
}

function checkPortAvailable(port, host = "127.0.0.1") {
  return new Promise((resolve) => {
    const tester = net.createServer();
    tester.once("error", () => resolve(false));
    tester.once("listening", () => {
      tester.close(() => resolve(true));
    });
    tester.listen(port, host);
  });
}

async function assertPortAvailable(port, { isPackaged = false, host = "127.0.0.1" } = {}) {
  const available = await checkPortAvailable(port, host);
  if (available) return;
  const mode = isPackaged ? "desktop app" : "development app";
  const otherMode = isPackaged
    ? `If npm start is running, close it or run it on ${DEVELOPMENT_PORT}.`
    : `If the packaged 3DGS Editor is running, close it or use GS_EDITOR_PORT to choose another development port.`;
  throw new Error(
    `The ${mode} requires fixed port ${port}, but that port is already in use. ` +
    `${otherMode} The app no longer switches to 7861 silently.`
  );
}

async function resolveServerPort(port, {
  isPackaged = false,
  host = "127.0.0.1",
  checkPortAvailable: checkAvailable = checkPortAvailable,
  isExpectedServer = async () => false
} = {}) {
  const available = await checkAvailable(port, host);
  if (available) {
    return { port, shouldStartServer: true, reusedExistingServer: false };
  }
  if (await isExpectedServer(port)) {
    return { port, shouldStartServer: false, reusedExistingServer: true };
  }
  await assertPortAvailable(port, { isPackaged, host });
  return { port, shouldStartServer: true, reusedExistingServer: false };
}

module.exports = {
  PACKAGED_PORT,
  DEVELOPMENT_PORT,
  parsePort,
  serverPortForMode,
  checkPortAvailable,
  assertPortAvailable,
  resolveServerPort
};
