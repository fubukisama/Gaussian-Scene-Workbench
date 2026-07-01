const assert = require("node:assert/strict");
const test = require("node:test");
const { EventEmitter } = require("node:events");

const { createExitConfirmation } = require("../exit_confirmation");

function createWindow() {
  const window = new EventEmitter();
  window.destroyed = false;
  window.isDestroyed = () => window.destroyed;
  return window;
}

function createCloseEvent() {
  return {
    prevented: false,
    preventDefault() {
      this.prevented = true;
    }
  };
}

test("window close is cancelled when user chooses cancel", () => {
  const window = createWindow();
  let cleanupCalls = 0;
  const exitConfirmation = createExitConfirmation({
    dialog: { showMessageBoxSync: () => 1 },
    getWindow: () => window,
    cleanupAndExit: () => {
      cleanupCalls += 1;
    }
  });

  exitConfirmation.attachToWindow(window);
  const event = createCloseEvent();
  window.emit("close", event);

  assert.equal(event.prevented, true);
  assert.equal(cleanupCalls, 0);
});

test("exit prompt uses the requested language", () => {
  const window = createWindow();
  const prompts = [];
  const exitConfirmation = createExitConfirmation({
    dialog: {
      showMessageBoxSync: (_window, options) => {
        prompts.push(options);
        return 1;
      }
    },
    getWindow: () => window,
    cleanupAndExit: () => {}
  });

  exitConfirmation.setLanguage("ja");
  exitConfirmation.attachToWindow(window);
  window.emit("close", createCloseEvent());
  exitConfirmation.setLanguage("en");
  window.emit("close", createCloseEvent());
  exitConfirmation.setLanguage("zh");
  window.emit("close", createCloseEvent());

  assert.equal(prompts[0].title, "終了確認");
  assert.deepEqual(prompts[0].buttons, ["終了", "キャンセル"]);
  assert.equal(prompts[1].title, "Confirm Exit");
  assert.deepEqual(prompts[1].buttons, ["Exit", "Cancel"]);
  assert.equal(prompts[2].title, "确认退出");
  assert.deepEqual(prompts[2].buttons, ["退出", "取消"]);
});

test("unsupported exit prompt language falls back to English", () => {
  const window = createWindow();
  let promptOptions = null;
  const exitConfirmation = createExitConfirmation({
    dialog: {
      showMessageBoxSync: (_window, options) => {
        promptOptions = options;
        return 1;
      }
    },
    getWindow: () => window,
    cleanupAndExit: () => {}
  });

  exitConfirmation.setLanguage("de");
  exitConfirmation.attachToWindow(window);
  window.emit("close", createCloseEvent());

  assert.equal(exitConfirmation.getLanguage(), "en");
  assert.equal(promptOptions.title, "Confirm Exit");
});

test("window close triggers cleanup when user confirms exit", () => {
  const window = createWindow();
  let cleanupCalls = 0;
  const exitConfirmation = createExitConfirmation({
    dialog: { showMessageBoxSync: () => 0 },
    getWindow: () => window,
    cleanupAndExit: () => {
      cleanupCalls += 1;
    }
  });

  exitConfirmation.attachToWindow(window);
  const event = createCloseEvent();
  window.emit("close", event);

  assert.equal(event.prevented, true);
  assert.equal(cleanupCalls, 1);
});

test("before quit asks for confirmation and prevents duplicate prompts", () => {
  const window = createWindow();
  let prompts = 0;
  let cleanupCalls = 0;
  const exitConfirmation = createExitConfirmation({
    dialog: {
      showMessageBoxSync: () => {
        prompts += 1;
        return 0;
      }
    },
    getWindow: () => window,
    cleanupAndExit: () => {
      cleanupCalls += 1;
    }
  });

  const first = createCloseEvent();
  const second = createCloseEvent();
  exitConfirmation.handleBeforeQuit(first);
  exitConfirmation.handleBeforeQuit(second);

  assert.equal(first.prevented, true);
  assert.equal(second.prevented, false);
  assert.equal(prompts, 1);
  assert.equal(cleanupCalls, 1);
});
