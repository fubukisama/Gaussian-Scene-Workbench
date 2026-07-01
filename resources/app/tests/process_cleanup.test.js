const assert = require("node:assert/strict");
const test = require("node:test");
const { EventEmitter } = require("node:events");

const { killProcessTree, stopServerProcess } = require("../process_cleanup");

test("killProcessTree uses taskkill recursively on Windows", async () => {
  const calls = [];
  const spawnFn = (file, args, options) => {
    calls.push({ file, args, options });
    const child = new EventEmitter();
    process.nextTick(() => child.emit("exit", 0));
    return child;
  };

  const ok = await killProcessTree(1234, { platform: "win32", spawnFn });

  assert.equal(ok, true);
  assert.equal(calls.length, 1);
  assert.equal(calls[0].file, "taskkill.exe");
  assert.deepEqual(calls[0].args, ["/PID", "1234", "/T", "/F"]);
  assert.equal(calls[0].options.windowsHide, true);
});

test("stopServerProcess requests server shutdown before killing tree", async () => {
  const order = [];
  const serverProcess = {
    pid: 4321,
    kill() {
      order.push("fallback-kill");
      return true;
    }
  };

  const ok = await stopServerProcess(serverProcess, 7860, {
    requestServerShutdown: async (port) => {
      order.push(`shutdown-${port}`);
      return true;
    },
    killProcessTree: async (pid) => {
      order.push(`tree-${pid}`);
      return true;
    }
  });

  assert.equal(ok, true);
  assert.deepEqual(order, ["shutdown-7860", "tree-4321"]);
});

test("stopServerProcess falls back to direct kill if tree kill fails", async () => {
  const order = [];
  const serverProcess = {
    pid: 4321,
    kill() {
      order.push("fallback-kill");
      return true;
    }
  };

  const ok = await stopServerProcess(serverProcess, 7860, {
    requestServerShutdown: async () => false,
    killProcessTree: async () => {
      order.push("tree-failed");
      return false;
    }
  });

  assert.equal(ok, true);
  assert.deepEqual(order, ["tree-failed", "fallback-kill"]);
});
