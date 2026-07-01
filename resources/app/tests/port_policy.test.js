const assert = require("node:assert/strict");
const test = require("node:test");

const {
  DEVELOPMENT_PORT,
  PACKAGED_PORT,
  parsePort,
  resolveServerPort,
  serverPortForMode
} = require("../port_policy");

test("packaged app uses fixed 7860 by default", () => {
  assert.equal(serverPortForMode({ isPackaged: true, env: {} }), PACKAGED_PORT);
});

test("development app uses a separate fixed port by default", () => {
  assert.equal(serverPortForMode({ isPackaged: false, env: {} }), DEVELOPMENT_PORT);
});

test("GS_EDITOR_PORT explicitly overrides fixed mode ports", () => {
  assert.equal(serverPortForMode({ isPackaged: true, env: { GS_EDITOR_PORT: "7900" } }), 7900);
  assert.equal(serverPortForMode({ isPackaged: false, env: { GS_EDITOR_PORT: "7901" } }), 7901);
});

test("invalid port values are rejected", () => {
  assert.throws(() => parsePort("0"), /Invalid port/);
  assert.throws(() => parsePort("7860.5"), /Invalid port/);
  assert.throws(() => parsePort("abc"), /Invalid port/);
});

test("occupied port is reused when it already serves 3DGS Editor", async () => {
  const startup = await resolveServerPort(PACKAGED_PORT, {
    isPackaged: true,
    checkPortAvailable: async () => false,
    isExpectedServer: async () => true
  });

  assert.deepEqual(startup, {
    port: PACKAGED_PORT,
    shouldStartServer: false,
    reusedExistingServer: true
  });
});
