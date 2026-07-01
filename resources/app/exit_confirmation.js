const EXIT_TEXT = {
  en: {
    title: "Confirm Exit",
    buttons: ["Exit", "Cancel"],
    message: "Exit 3DGS Editor?",
    detail: "Exiting stops the training, COLMAP, server, and other background processes started by this app."
  },
  zh: {
    title: "确认退出",
    buttons: ["退出", "取消"],
    message: "确定要退出 3DGS Editor 吗？",
    detail: "退出后会停止当前软件启动的训练、COLMAP、服务端等后台进程。"
  },
  ja: {
    title: "終了確認",
    buttons: ["終了", "キャンセル"],
    message: "3DGS Editor を終了しますか？",
    detail: "終了すると、このアプリが起動したトレーニング、COLMAP、サーバーなどのバックグラウンドプロセスを停止します。"
  }
};

function normalizeLanguage(language) {
  return Object.prototype.hasOwnProperty.call(EXIT_TEXT, language) ? language : "en";
}

function createExitConfirmation({ dialog, getWindow, cleanupAndExit, log = () => {}, language = "en" }) {
  let exitConfirmed = false;
  let promptOpen = false;
  let currentLanguage = normalizeLanguage(language);

  function activeWindow() {
    const window = typeof getWindow === "function" ? getWindow() : null;
    if (!window || (typeof window.isDestroyed === "function" && window.isDestroyed())) {
      return null;
    }
    return window;
  }

  function confirmExit(parentWindow) {
    if (promptOpen) return false;
    promptOpen = true;
    try {
      const text = EXIT_TEXT[currentLanguage];
      const choice = dialog.showMessageBoxSync(parentWindow || undefined, {
        type: "question",
        buttons: text.buttons,
        defaultId: 1,
        cancelId: 1,
        noLink: true,
        title: text.title,
        message: text.message,
        detail: text.detail
      });
      return choice === 0;
    } finally {
      promptOpen = false;
    }
  }

  function requestExit(event) {
    if (exitConfirmed) return false;
    const window = activeWindow();
    if (!window) return false;
    if (event && typeof event.preventDefault === "function") {
      event.preventDefault();
    }
    if (!confirmExit(window)) {
      log("Exit cancelled by user");
      return true;
    }
    exitConfirmed = true;
    log("Exit confirmed by user");
    cleanupAndExit(0);
    return true;
  }

  function attachToWindow(window) {
    if (!window || typeof window.on !== "function") return;
    window.on("close", requestExit);
  }

  function handleBeforeQuit(event) {
    return requestExit(event);
  }

  function setLanguage(language) {
    currentLanguage = normalizeLanguage(language);
    return currentLanguage;
  }

  return {
    attachToWindow,
    handleBeforeQuit,
    isExitConfirmed: () => exitConfirmed,
    setLanguage,
    getLanguage: () => currentLanguage
  };
}

module.exports = {
  createExitConfirmation,
  EXIT_TEXT,
  normalizeLanguage
};
