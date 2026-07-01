const { contextBridge, ipcRenderer, webUtils } = require("electron");

contextBridge.exposeInMainWorld("gsEditor", {
  getPathForFile(file) {
    try {
      return webUtils.getPathForFile(file) || "";
    } catch (_) {
      return "";
    }
  },
  setLanguage(language) {
    if (!["en", "zh", "ja"].includes(language)) return;
    ipcRenderer.send("gs-editor:set-language", language);
  }
});
